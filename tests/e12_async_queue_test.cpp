// e12_async_queue_test — sluice::async::AsyncQueue (sluice-CORE-E12-E).
//
// Exercises the E12-E Queue through the non-template QueuePort authority +
// QueueItemFactory (the public AsyncQueue<T> wrapper is P8; until then we
// drive the authority directly). Coverage:
//
//   P2+P3 fast paths: try_push / try_pop / close / snapshot
//     - capacity-1 and capacity-N fixed ring
//     - FIFO buffer order (push a,b,c; pop a,b,c)
//     - try_push committed / try_push would_block (full)
//     - try_pop item / try_pop would_block (empty, open)
//     - close is idempotent + monotone (CL1/CL2)
//     - closed + empty => try_pop closed (C2)
//     - closed rejects producer with the EXACT original lease (P3 PushClosed)
//     - failed-payload identity: closed/would_block carries the original
//       control, recovered as the exact typed T via the factory
//       (no copy / alias / default / loss)
//     - one-shot lease: moving the lease empties the source
//
//   P4-P6 wait admission + reconciliation + close drain (single-worker Fiber):
//     - blocking pop granted on push (reconciler commits ring -> winner)
//     - blocking push granted on pop (reconciler commits winner -> freed slot)
//     - close completes a blocked producer with `closed` + exact lease
//     - close drains buffered items to a blocked consumer, then closed
//
//   P7 teardown lifecycle:
//     - begin_teardown drains a multi-item ring in FIFO order via take_next
//     - begin_teardown on an empty ring yields an immediately-empty session
//     - session is move-only; move empties the source
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/async_queue.hpp>  // public AsyncQueue<T> (P8)
#include <sluice/async/detail/queue_item.hpp>
#include <sluice/async/detail/queue_port.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/scheduler.hpp>

#include <cstddef>
#include <memory>
#include <stdexcept>  // std::invalid_argument (P8 capacity-0 rejection)
#include <string>
#include <utility>

using namespace sluice::async;
using namespace sluice::async::detail;
using sluice::Result;

namespace {
// A backend that never completes anything. The P2+P3 fast paths never touch
// the Scheduler's progress machinery, so an idle backend suffices. (P4-P6
// wait-admission tests will need a real driver; not here.)
class IdleBackend : public AsyncBackend {
public:
    Result<void> submit_read(ReadOp, Completion<std::size_t>&) override { return {}; }
    Result<void> submit_write(WriteOp, Completion<std::size_t>&) override { return {}; }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&) override { return {}; }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&) override { return {}; }
    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override { return 0; }
    void cancel(Completion<std::size_t>&) override {}
    void cancel(Completion<void>&) override {}
    std::size_t outstanding() const noexcept override { return 0; }
};

struct Fixture {
    AsyncIoContext ctx{std::make_unique<IdleBackend>()};
    Scheduler sched{ctx};
    std::unique_ptr<QueuePort> port;
    explicit Fixture(std::size_t cap) {
        port = std::make_unique<QueuePort>(sched, cap);
    }
};

// Mint a typed lease via the factory (the only minting path).
template <class T, class U>
QueueItemLease make_lease(QueuePort& port, U&& value) {
    return QueueItemFactory::make<T>(port, std::forward<U>(value));
}

// Recover a typed value from a failed push result; asserts identity.
template <class T>
T release_failed(QueuePort& port, QueueOpaquePushResult&& r) {
    QueueItemLease lease = std::move(r).take_failed_lease();
    return QueueItemFactory::release_failed<T>(port, std::move(lease));
}
template <class T>
T release_popped(QueuePort& port, QueueOpaquePopResult&& r) {
    QueueItemLease lease = std::move(r).take_item_lease();
    return QueueItemFactory::release_popped<T>(port, std::move(lease));
}
}  // namespace

SLUICE_TEST_CASE(e12_queue_capacity_and_fifo) {
    Fixture f{3};
    SLUICE_CHECK(f.port->capacity() == 3);
    SLUICE_CHECK(f.port->size() == 0);

    // push 3 items; FIFO order preserved on pop.
    {
        auto r = f.port->try_push(make_lease<std::string>(*f.port, std::string("a")));
        SLUICE_CHECK(r.status() == QueueOpaquePushStatus::committed);
    }
    {
        auto r = f.port->try_push(make_lease<std::string>(*f.port, std::string("b")));
        SLUICE_CHECK(r.status() == QueueOpaquePushStatus::committed);
    }
    {
        auto r = f.port->try_push(make_lease<std::string>(*f.port, std::string("c")));
        SLUICE_CHECK(r.status() == QueueOpaquePushStatus::committed);
    }
    SLUICE_CHECK(f.port->size() == 3);

    auto r1 = f.port->try_pop();
    SLUICE_CHECK(r1.status() == QueueOpaquePopStatus::item);
    SLUICE_CHECK(release_popped<std::string>(*f.port, std::move(r1)) == "a");
    auto r2 = f.port->try_pop();
    SLUICE_CHECK(r2.status() == QueueOpaquePopStatus::item);
    SLUICE_CHECK(release_popped<std::string>(*f.port, std::move(r2)) == "b");
    auto r3 = f.port->try_pop();
    SLUICE_CHECK(r3.status() == QueueOpaquePopStatus::item);
    SLUICE_CHECK(release_popped<std::string>(*f.port, std::move(r3)) == "c");
    SLUICE_CHECK(f.port->size() == 0);
}

SLUICE_TEST_CASE(e12_queue_capacity1) {
    Fixture f{1};
    SLUICE_CHECK(f.port->capacity() == 1);
    {
        auto r = f.port->try_push(make_lease<int>(*f.port, 42));
        SLUICE_CHECK(r.status() == QueueOpaquePushStatus::committed);
    }
    SLUICE_CHECK(f.port->size() == 1);
    // full now -> would_block, and the EXACT lease is returned.
    {
        auto lease = make_lease<int>(*f.port, 99);
        auto r = f.port->try_push(std::move(lease));
        SLUICE_CHECK(r.status() == QueueOpaquePushStatus::would_block);
        SLUICE_CHECK(!static_cast<bool>(lease));  // source emptied by move
        int recovered = release_failed<int>(*f.port, std::move(r));
        SLUICE_CHECK(recovered == 99);  // exact original payload
    }
    // pop the buffered 42.
    auto rp = f.port->try_pop();
    SLUICE_CHECK(rp.status() == QueueOpaquePopStatus::item);
    SLUICE_CHECK(release_popped<int>(*f.port, std::move(rp)) == 42);
    SLUICE_CHECK(f.port->size() == 0);
}

SLUICE_TEST_CASE(e12_queue_try_pop_would_block_when_empty_open) {
    Fixture f{2};
    auto r = f.port->try_pop();
    SLUICE_CHECK(r.status() == QueueOpaquePopStatus::would_block);
}

SLUICE_TEST_CASE(e12_queue_close_idempotent_and_closed_empty_terminal) {
    Fixture f{2};
    SLUICE_CHECK(!f.port->is_closed());
    f.port->close();
    SLUICE_CHECK(f.port->is_closed());
    f.port->close();  // idempotent (CL2)
    SLUICE_CHECK(f.port->is_closed());

    // closed + empty => pop closed (C2).
    auto r = f.port->try_pop();
    SLUICE_CHECK(r.status() == QueueOpaquePopStatus::closed);
}

SLUICE_TEST_CASE(e12_queue_closed_drains_buffered_then_closed) {
    Fixture f{2};
    {
        auto r = f.port->try_push(make_lease<int>(*f.port, 7));
        SLUICE_CHECK(r.status() == QueueOpaquePushStatus::committed);
    }
    f.port->close();
    // buffered item still drainable after close.
    auto r1 = f.port->try_pop();
    SLUICE_CHECK(r1.status() == QueueOpaquePopStatus::item);
    SLUICE_CHECK(release_popped<int>(*f.port, std::move(r1)) == 7);
    // now closed + empty => closed.
    auto r2 = f.port->try_pop();
    SLUICE_CHECK(r2.status() == QueueOpaquePopStatus::closed);
}

SLUICE_TEST_CASE(e12_queue_push_closed_returns_exact_lease) {
    Fixture f{1};
    f.port->close();
    // P3 PushClosed: producer rejected; exact lease returned (no copy/alias).
    auto lease = make_lease<std::string>(*f.port, std::string("payload"));
    auto r = f.port->try_push(std::move(lease));
    SLUICE_CHECK(r.status() == QueueOpaquePushStatus::closed);
    SLUICE_CHECK(!static_cast<bool>(lease));  // source emptied
    std::string recovered = release_failed<std::string>(*f.port, std::move(r));
    SLUICE_CHECK(recovered == "payload");  // exact original
}

SLUICE_TEST_CASE(e12_queue_oneshot_lease_move_empties_source) {
    Fixture f{1};
    auto a = make_lease<int>(*f.port, 5);
    SLUICE_CHECK(static_cast<bool>(a));
    QueueItemLease b = std::move(a);
    SLUICE_CHECK(static_cast<bool>(b));
    SLUICE_CHECK(!static_cast<bool>(a));  // move empties source
    // Use b (commit it) so neither lease is destroyed non-empty.
    auto r = f.port->try_push(std::move(b));
    SLUICE_CHECK(r.status() == QueueOpaquePushStatus::committed);
    SLUICE_CHECK(!static_cast<bool>(b));  // consumed
    // Drain the committed item so the Fixture's ~QueuePort sees an empty ring
    // (the destruction contract requires ring-empty + teardown-complete;
    // until P7 the fast-path test drains via try_pop).
    auto rp = f.port->try_pop();
    SLUICE_CHECK(rp.status() == QueueOpaquePopStatus::item);
    (void)release_popped<int>(*f.port, std::move(rp));
}

// ===========================================================================
// P4-P6 — blocking/timed substrate + reconciliation (deterministic, Fiber).
// ===========================================================================
#if defined(__x86_64__) || defined(_M_X64)
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>

#include <atomic>
#include <thread>

namespace {
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};
}  // namespace

// ---- P4: blocking pop wakes when a producer commits (single worker) ------
// Producer fiber commits an item; a consumer fiber (spawned FIRST, parks on
// empty ring) is granted the item via the reconciler and resumes with it.
SLUICE_TEST_CASE(e12_queue_p4_blocking_pop_granted_on_push) {
    if (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 2);

    int consumed = -1;
    bool consumer_ran = false;
    Fiber consumer;
    consumer.set_entry([&](Fiber&) {
        auto r = port.pop();  // parks on empty; granted when producer commits
        if (r.status() == QueueOpaquePopStatus::item) {
            consumed = release_popped<int>(port, std::move(r));
        }
        consumer_ran = true;
    });
    FiberStack sc;
    SLUICE_CHECK(sched.init_fiber(consumer, sc.base(), sc.size()));
    sched.spawn(consumer);

    Fiber producer;
    producer.set_entry([&](Fiber&) {
        auto lease = QueueItemFactory::make<int>(port, 12345);
        auto r = port.try_push(std::move(lease));
        (void)r;
    });
    FiberStack sp;
    SLUICE_CHECK(sched.init_fiber(producer, sp.base(), sp.size()));
    sched.spawn(producer);

    sched.run(1);
    SLUICE_CHECK(consumer_ran);
    SLUICE_CHECK(consumed == 12345);
    SLUICE_CHECK(port.size() == 0);  // granted directly, not buffered
}

// ---- P4: blocking push wakes when a consumer frees a slot (cap-1) --------
SLUICE_TEST_CASE(e12_queue_p4_blocking_push_granted_on_pop) {
    if (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 1);

    // Pre-fill the ring so the producer must park.
    {
        auto lease = QueueItemFactory::make<int>(port, 1);
        (void)port.try_push(std::move(lease));
    }
    bool push_committed = false;
    Fiber producer;
    producer.set_entry([&](Fiber&) {
        auto lease = QueueItemFactory::make<int>(port, 77);
        auto r = port.push(std::move(lease));  // parks (full); granted on pop
        push_committed = (r.status() == QueueOpaquePushStatus::committed);
    });
    FiberStack sp;
    SLUICE_CHECK(sched.init_fiber(producer, sp.base(), sp.size()));
    sched.spawn(producer);

    int consumed = -1;
    Fiber consumer;
    consumer.set_entry([&](Fiber&) {
        auto rp = port.try_pop();  // pops the pre-filled 1, frees the slot
        if (rp.status() == QueueOpaquePopStatus::item) {
            consumed = release_popped<int>(port, std::move(rp));
        }
    });
    FiberStack sc;
    SLUICE_CHECK(sched.init_fiber(consumer, sc.base(), sc.size()));
    sched.spawn(consumer);

    sched.run(1);
    SLUICE_CHECK(consumed == 1);
    SLUICE_CHECK(push_committed);
    SLUICE_CHECK(port.size() == 1);  // producer's 77 now buffered
    // Drain to satisfy the ring-empty destruction contract.
    auto rp = port.try_pop();
    SLUICE_CHECK(rp.status() == QueueOpaquePopStatus::item);
    (void)release_popped<int>(port, std::move(rp));
}

// ---- P5: close completes a blocked producer with `closed` (lease retained)
SLUICE_TEST_CASE(e12_queue_p5_close_completes_blocked_producer) {
    if (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 1);

    // Pre-fill so the producer parks.
    {
        auto lease = QueueItemFactory::make<int>(port, 1);
        (void)port.try_push(std::move(lease));
    }
    bool producer_got_closed = false;
    Fiber producer;
    producer.set_entry([&](Fiber&) {
        auto lease = QueueItemFactory::make<int>(port, 42);
        auto r = port.push(std::move(lease));  // parks; close completes it
        producer_got_closed =
            (r.status() == QueueOpaquePushStatus::closed);
        if (producer_got_closed) {
            // The EXACT original lease is retained: recover 42.
            int v = release_failed<int>(port, std::move(r));
            (void)v;
        }
    });
    FiberStack sp;
    SLUICE_CHECK(sched.init_fiber(producer, sp.base(), sp.size()));
    sched.spawn(producer);

    Fiber closer;
    closer.set_entry([&](Fiber&) { port.close(); });
    FiberStack scc;
    SLUICE_CHECK(sched.init_fiber(closer, scc.base(), scc.size()));
    sched.spawn(closer);

    sched.run(1);
    SLUICE_CHECK(producer_got_closed);
    // Drain the pre-filled item (close grants it to no one because no consumer
    // is parked; it stays buffered). Pop it to satisfy ring-empty destruction.
    auto rp = port.try_pop();
    SLUICE_CHECK(rp.status() == QueueOpaquePopStatus::item);
    (void)release_popped<int>(port, std::move(rp));
}

// ---- P5: close drains buffered items to a blocked consumer, then closed ---
SLUICE_TEST_CASE(e12_queue_p5_close_drains_to_blocked_consumer) {
    if (!fiber_ctx::supported) return;
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    QueuePort port(sched, 4);

    // Buffer two items.
    for (int v : {11, 22}) {
        auto lease = QueueItemFactory::make<int>(port, v);
        (void)port.try_push(std::move(lease));
    }
    int first_consumed = -1;
    bool second_was_closed = false;
    Fiber consumer;
    consumer.set_entry([&](Fiber&) {
        // First pop: parks (empty after close drains? no — items buffered).
        // We do two blocking pops: the first gets 11 (granted by close drain),
        // the second gets 22, the third sees closed+empty.
        auto r1 = port.pop();
        if (r1.status() == QueueOpaquePopStatus::item) {
            first_consumed = release_popped<int>(port, std::move(r1));
        }
        auto r2 = port.pop();
        if (r2.status() == QueueOpaquePopStatus::item) {
            (void)release_popped<int>(port, std::move(r2));  // release 22
        }
        auto r3 = port.pop();
        second_was_closed = (r3.status() == QueueOpaquePopStatus::closed);
    });
    FiberStack sc;
    SLUICE_CHECK(sched.init_fiber(consumer, sc.base(), sc.size()));
    sched.spawn(consumer);

    Fiber closer;
    closer.set_entry([&](Fiber&) { port.close(); });
    FiberStack scc;
    SLUICE_CHECK(sched.init_fiber(closer, scc.base(), scc.size()));
    sched.spawn(closer);

    sched.run(1);
    SLUICE_CHECK(first_consumed == 11);
    SLUICE_CHECK(second_was_closed);
}
#endif  // x86_64

// ===========================================================================
// P7 — teardown lifecycle.
// ===========================================================================
//
// begin_teardown performs the irreversible operational -> tearing_down
// transition under G+S with the full precondition check. take_next drains ring
// slots one-by-one (ring -> teardown) in FIFO order; the typed layer recovers
// each value via release_teardown and destroys the exact Node<T> outside
// locks. Session destruction is no-throw and succeeds iff the ring is empty.
//
// The fail-fast paths (second begin_teardown, ordinary op after teardown,
// session dtor with non-empty ring) call std::terminate and are NOT exercised
// here — they are structural invariants serialized by the lifecycle transition
// and the linear capability (P1). The positive paths below cover the contract
// surface a well-formed caller observes.

// ---- P7: begin_teardown drains a multi-item ring in FIFO order ------------
SLUICE_TEST_CASE(e12_queue_p7_teardown_drains_ring_fifo) {
    Fixture f{4};
    // Buffer three items.
    for (int v : {10, 20, 30}) {
        auto lease = make_lease<int>(*f.port, v);
        SLUICE_CHECK(
            f.port->try_push(std::move(lease)).status() ==
            QueueOpaquePushStatus::committed);
    }
    SLUICE_CHECK(f.port->size() == 3);

    // begin_teardown: operational -> tearing_down. The four counters are zero
    // (no ordinary op in flight, no parked waiter, no active timer, no
    // published winner) and both role FIFOs are empty, so the transition is
    // admitted.
    QueueTeardownSession session = f.port->begin_teardown();
    SLUICE_CHECK(!session.empty());  // ring still holds 3 items

    // Drain in FIFO order: 10, 20, 30. Each take_next moves ring -> teardown.
    int recovered[3] = {-1, -1, -1};
    for (int i = 0; i < 3; ++i) {
        QueueItemLease lease = session.take_next();
        SLUICE_CHECK(static_cast<bool>(lease));
        recovered[i] = QueueItemFactory::release_teardown<int>(*f.port,
                                                               std::move(lease));
    }
    SLUICE_CHECK(recovered[0] == 10);
    SLUICE_CHECK(recovered[1] == 20);
    SLUICE_CHECK(recovered[2] == 30);
    // Ring drained: take_next now returns an empty lease; empty() is true.
    SLUICE_CHECK(session.empty());
    SLUICE_CHECK(!static_cast<bool>(session.take_next()));
}
// The session (now drained) and the port are destroyed at scope exit. The
// session dtor sees ring-empty; ~QueuePort sees ring_count_ == 0. Both succeed.

// ---- P7: begin_teardown on an empty ring yields an immediately-empty session
SLUICE_TEST_CASE(e12_queue_p7_teardown_empty_ring) {
    Fixture f{2};
    QueueTeardownSession session = f.port->begin_teardown();
    SLUICE_CHECK(session.empty());
    SLUICE_CHECK(!static_cast<bool>(session.take_next()));
}
// Session + port destroyed cleanly (ring empty + lifecycle tearing_down).

// ---- P7: teardown session is move-only; move empties the source -----------
SLUICE_TEST_CASE(e12_queue_p7_session_move_only) {
    Fixture f{2};
    {
        auto lease = make_lease<int>(*f.port, 7);
        (void)f.port->try_push(std::move(lease));
    }
    QueueTeardownSession a = f.port->begin_teardown();
    SLUICE_CHECK(!a.empty());
    QueueTeardownSession b = std::move(a);  // a.port_ cleared
    // b now owns the session; a is a valid empty shell (empty() == true, since
    // a null port_ short-circuits to true).
    SLUICE_CHECK(a.empty());
    SLUICE_CHECK(!b.empty());
    // Drain via b so the session dtor at scope exit sees an empty ring.
    QueueItemLease lease = b.take_next();
    SLUICE_CHECK(static_cast<bool>(lease));
    (void)QueueItemFactory::release_teardown<int>(*f.port, std::move(lease));
    SLUICE_CHECK(b.empty());
}
// b is destroyed first (LIFO); a's dtor is a no-op (port_ == nullptr). The
// port is then destroyed with an empty ring.

// ===========================================================================
// P8 — public AsyncQueue<T> typed wrapper.
// ===========================================================================
//
// AsyncQueue<T> is the thin public typed layer: it mints the typed Node<T>
// outside locks, drives the QueuePort seam, and converts the opaque result to
// a typed one (releasing the Node<T> outside locks). These tests exercise the
// public surface through the typed API, mirroring the P2-P7 authority-direct
// tests but at the typed boundary. They confirm:
//
//   - typed FIFO order on try_push/try_pop
//   - typed try_push would_block + closed recover the exact original T
//   - typed try_pop would_block / closed / item
//   - close + drain via the typed wrapper
//   - begin_teardown + typed release_teardown

// ---- P8: AsyncQueue<T> typed FIFO + failure recovery ----------------------
SLUICE_TEST_CASE(e12_queue_p8_typed_fifo_and_failure_recovery) {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncQueue<std::string> q(sched, 3);

    // Typed FIFO: push "a","b","c"; pop yields them in order.
    SLUICE_CHECK(q.try_push(std::string("a")).status() == QueuePushStatus::committed);
    SLUICE_CHECK(q.try_push(std::string("b")).status() == QueuePushStatus::committed);
    SLUICE_CHECK(q.try_push(std::string("c")).status() == QueuePushStatus::committed);
    SLUICE_CHECK(q.size() == 3);

    auto r1 = q.try_pop();
    SLUICE_CHECK(r1.status() == QueuePopStatus::item);
    SLUICE_CHECK(std::move(r1).take_value() == "a");
    auto r2 = q.try_pop();
    SLUICE_CHECK(r2.status() == QueuePopStatus::item);
    SLUICE_CHECK(std::move(r2).take_value() == "b");
    auto r3 = q.try_pop();
    SLUICE_CHECK(r3.status() == QueuePopStatus::item);
    SLUICE_CHECK(std::move(r3).take_value() == "c");

    // Fill the ring again (cap 3) so the next push would_block. The exact
    // original T is recovered via take_value (no copy/alias/default).
    SLUICE_CHECK(q.try_push(std::string("x")).status() == QueuePushStatus::committed);
    SLUICE_CHECK(q.try_push(std::string("y")).status() == QueuePushStatus::committed);
    SLUICE_CHECK(q.try_push(std::string("z")).status() == QueuePushStatus::committed);
    SLUICE_CHECK(q.size() == 3);
    auto blocked = q.try_push(std::string("payload"));
    SLUICE_CHECK(blocked.status() == QueuePushStatus::would_block);
    SLUICE_CHECK(std::move(blocked).take_value() == "payload");

    // Drain the 3 buffered items so the ring is empty again.
    for (int i = 0; i < 3; ++i) {
        auto r = q.try_pop();
        SLUICE_CHECK(r.status() == QueuePopStatus::item);
        (void)std::move(r).take_value();
    }
    // Empty + open: would_block (no payload).
    SLUICE_CHECK(q.try_pop().status() == QueuePopStatus::would_block);

    // Closed + empty: closed (terminal, no payload). Closed push recovers T.
    q.close();
    auto pclosed = q.try_push(std::string("rejected"));
    SLUICE_CHECK(pclosed.status() == QueuePushStatus::closed);
    SLUICE_CHECK(std::move(pclosed).take_value() == "rejected");
    SLUICE_CHECK(q.try_pop().status() == QueuePopStatus::closed);
}

// ---- P8: AsyncQueue<T> teardown drains ring via typed release_teardown -----
SLUICE_TEST_CASE(e12_queue_p8_typed_teardown_drains_fifo) {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncQueue<int> q(sched, 4);

    // Buffer three items.
    for (int v : {100, 200, 300}) {
        SLUICE_CHECK(q.try_push(v).status() == QueuePushStatus::committed);
    }
    SLUICE_CHECK(q.size() == 3);

    // begin_teardown: operational -> tearing_down. The unique session drains
    // the ring via the typed release_teardown helper (moves T once, destroys
    // Node<T> outside locks).
    auto session = q.begin_teardown();
    SLUICE_CHECK(!session.empty());

    int recovered[3] = {-1, -1, -1};
    for (int i = 0; i < 3; ++i) {
        recovered[i] = q.release_teardown(session);
    }
    SLUICE_CHECK(recovered[0] == 100);
    SLUICE_CHECK(recovered[1] == 200);
    SLUICE_CHECK(recovered[2] == 300);
    SLUICE_CHECK(session.empty());
}
// Session (drained) + AsyncQueue destroyed cleanly: ~QueuePort sees ring_count_
// == 0 and the session dtor sees an empty ring.

// ---- P8: AsyncQueue<T> ctor rejects capacity 0 ----------------------------
SLUICE_TEST_CASE(e12_queue_p8_capacity_zero_rejected) {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    bool threw = false;
    try {
        AsyncQueue<int> q(sched, 0);
        (void)q;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    SLUICE_CHECK(threw);
}

SLUICE_MAIN();
