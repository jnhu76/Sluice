// Pending-control counter parity suite for the io_uring backend.
//
// The derived pending_control_count_ summary must equal the authoritative
// router scan (#{route | route.in_use && route.control_pending}) at every
// observable checkpoint, repeated cancel must never double-count, a sticky
// intent must survive SQE pressure and be issued once pressure releases, and
// the count must retire with its route across original-CQE retirement, slot
// reuse, poison recovery and shutdown. Concurrent scenarios check parity
// through the single-critical-section snapshot seam only: sampling the count
// and the scan in two separate locked calls would compare two different
// instants. Every failure exits through a named oracle message with _Exit so
// no destructor abort can preempt the named verdict in mutation builds.

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/uring_backend.hpp>
#include <sluice/async/request.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <liburing.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <latch>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace {

using namespace sluice::async;

constexpr std::uint64_t kControlTag = std::uint64_t{1} << 63u;

// Hold mode consumes nothing: SQEs prepared by the backend stay unconsumed so
// io_uring_get_sqe keeps failing once the ring is full. Release mode mimics a
// kernel submit that consumed every pending SQE.
int switchable_submit(void* context, ::io_uring* ring) noexcept {
    auto* hold = static_cast<std::atomic<bool>*>(context);
    if (hold->load(std::memory_order_acquire)) {
        return 0;
    }
    const unsigned ready = ::io_uring_sq_ready(ring);
    *ring->sq.ktail = ring->sq.sqe_tail;
    *ring->sq.khead = ring->sq.sqe_tail;
    return static_cast<int>(ready);
}

struct PoisonState {
    std::atomic<bool> hold{true};
    std::atomic<bool> fail_next{false};
};

int poison_submit_hook(void* context, ::io_uring* ring) noexcept {
    auto* state = static_cast<PoisonState*>(context);
    if (state->hold.load(std::memory_order_acquire)) {
        return 0;
    }
    if (state->fail_next.exchange(false, std::memory_order_acq_rel))
        return -EINVAL;
    const unsigned ready = ::io_uring_sq_ready(ring);
    *ring->sq.ktail = ring->sq.sqe_tail;
    *ring->sq.khead = ring->sq.sqe_tail;
    return static_cast<int>(ready);
}

struct Fixture {
    std::unique_ptr<AsyncIoContext> ctx;
    UringAsyncBackend* raw = nullptr;
    FILE* backing = nullptr;
    std::unique_ptr<std::atomic<bool>> hold{std::make_unique<std::atomic<bool>>(true)};

    static Fixture create(std::size_t capacity, unsigned queue_depth) {
        Fixture made;
        made.backing = ::tmpfile();
        if (made.backing == nullptr)
            return made;
        std::byte content[4096];
        std::memset(content, 0x33, sizeof(content));
        if (::fwrite(content, 1, sizeof(content), made.backing) != sizeof(content))
            return Fixture{};
        ::rewind(made.backing);
        UringBackendSubmitTestHooks hooks;
        hooks.submit = switchable_submit;
        hooks.context = made.hold.get();
        auto owned = std::make_unique<UringAsyncBackend>(UringConfig{capacity, queue_depth}, hooks);
        made.raw = owned.get();
        made.ctx = std::make_unique<AsyncIoContext>(detail::make_context(std::move(owned)));
        return made;
    }

    bool ok() const { return ctx != nullptr && raw != nullptr && backing != nullptr; }

    ReadOp data_op(std::byte* buffer, std::size_t len, std::uint64_t offset) {
        return ReadOp{NativeFileRef(::fileno(backing), sluice::FileAccess::read_only), buffer, len,
                      offset};
    }
};

[[noreturn]] void fail(const char* oracle, const char* detail) {
    std::fprintf(stderr, "FAIL %s: %s\n", oracle, detail);
    std::fflush(stderr);
    std::_Exit(1);
}

void require(bool condition, const char* oracle, const char* detail) {
    if (!condition)
        fail(oracle, detail);
}

using Snapshot = UringAsyncBackend::PendingControlSnapshot;

Snapshot snap_parity(UringAsyncBackend* raw, const char* where) {
    const Snapshot snap = raw->pending_control_snapshot_for_test();
    if (snap.derived_count != snap.authoritative_pending) {
        std::fprintf(stderr,
                     "FAIL parity oracle at %s: derived pending_control_count_=%zu but "
                     "authoritative router scan=%zu\n",
                     where, snap.derived_count, snap.authoritative_pending);
        std::fflush(stderr);
        std::_Exit(1);
    }
    return snap;
}

// Sticky-cancel lifecycle under SQE pressure, pinned by two named oracles:
// parity at every checkpoint, and the protocol property that releasing the
// pressure lets the next pass issue the intent exactly once.
void sticky_lifecycle_oracle() {
    const char* const where = "sticky_lifecycle";
    Fixture made = Fixture::create(4, 1);
    require(made.ok(), "fixture", "creation failed");

    std::byte a[4];
    std::byte b[4];
    auto first = made.ctx->submit_read(made.data_op(a, sizeof(a), 0));
    require(first.has_value(), "fixture", "first submit failed");
    auto second = made.ctx->submit_read(made.data_op(b, sizeof(b), 8));
    require(second.has_value(), "fixture", "second submit failed");
    require(made.raw->dispatch_size_for_test() == 1, "fixture", "second submit not queued");

    const auto disposition = made.ctx->cancel(first.value().id());
    require(disposition.has_value() && disposition.value() == CancelDisposition::requested,
            "fixture", "cancel was not requested");

    const Snapshot stuck = snap_parity(made.raw, where);
    require(stuck.authoritative_pending == 1, "protocol oracle",
            "sticky intent did not register in the router");
    require(stuck.derived_count == 1, "parity oracle",
            "pending_control_count_ diverged from the router scan after the sticky set "
            "(missing increment)");

    for (int pass = 0; pass < 3; ++pass) {
        (void)made.ctx->poll();
        const Snapshot held = snap_parity(made.raw, where);
        require(held.derived_count == 1, "protocol oracle",
                "sticky intent lost while SQE pressure persists");
        require(held.prepared_controls == 0 && held.submitted_controls == 0, "protocol oracle",
                "control state advanced while SQEs were unavailable");
    }

    made.hold->store(false, std::memory_order_release);
    (void)made.ctx->poll();
    (void)made.ctx->poll();
    require(made.raw->live_control_sqes_for_test() == 1, "protocol oracle",
            "released pressure did not issue the pending control SQE (lost intent)");
    const Snapshot issued = snap_parity(made.raw, where);
    require(issued.derived_count == 0, "parity oracle",
            "count did not retire with the issued intent (missing decrement)");

    const auto first_cookie = made.raw->live_cookie_for_offset_for_test(0);
    const auto second_cookie = made.raw->live_cookie_for_offset_for_test(8);
    require(first_cookie.has_value() && second_cookie.has_value(), "fixture", "cookies missing");
    made.raw->inject_cqe_for_test(kControlTag | *first_cookie, 0);
    made.raw->inject_cqe_for_test(*first_cookie, -ECANCELED);
    made.raw->inject_cqe_for_test(*second_cookie, static_cast<int>(sizeof(b)));
    (void)made.ctx->poll();

    snap_parity(made.raw, where);
    require(made.raw->internal_work_retired(), "protocol oracle", "control work not retired");
    const auto first_seen = first.value().take_result();
    const auto second_seen = second.value().take_result();
    require(first_seen.readiness == RequestReadiness::ready &&
                second_seen.readiness == RequestReadiness::ready &&
                second_seen.result.has_value() && second_seen.result.value() == sizeof(b),
            "protocol oracle", "requests did not settle through the control path");
}

// Repeated cancel while the intent is already pending is legal (CANCEL-01
// idempotence) and must move the count exactly once.
void repeated_cancel_oracle() {
    const char* const where = "repeated_cancel";
    Fixture made = Fixture::create(4, 1);
    require(made.ok(), "fixture", "creation failed");

    std::byte a[4];
    auto first = made.ctx->submit_read(made.data_op(a, sizeof(a), 0));
    require(first.has_value(), "fixture", "submit failed");
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto seen = made.ctx->cancel(first.value().id());
        require(seen.has_value() && seen.value() == CancelDisposition::requested, "fixture",
                "a repeated cancel was not coalesced as requested");
        const Snapshot snap = snap_parity(made.raw, where);
        require(snap.derived_count == 1, "parity oracle",
                "repeated cancel drifted the count above the router scan (double increment)");
    }
    (void)made.ctx->poll();
    const Snapshot held = snap_parity(made.raw, where);
    require(held.derived_count == 1, "protocol oracle", "pressure pass dropped the sticky intent");

    const auto identity = made.raw->live_cookie_for_offset_for_test(0);
    require(identity.has_value(), "fixture", "cookie missing");
    made.raw->inject_cqe_for_test(*identity, static_cast<int>(sizeof(a)));
    (void)made.ctx->poll();
    const Snapshot settled = snap_parity(made.raw, where);
    require(settled.derived_count == 0, "parity oracle",
            "count survived the retirement that consumed the intent");
    require(first.value().ready(), "protocol oracle", "request did not settle");
    (void)first.value().take_result();
    made.hold->store(false, std::memory_order_release);
    (void)made.ctx->poll();
    snap_parity(made.raw, where);
    require(made.raw->internal_work_retired(), "protocol oracle", "work not retired");
}

// Original-CQE-first retirement of a pending intent, then slot reuse: the
// count retires with the route and the reused slot starts with a clean
// control identity.
void retire_and_slot_reuse_oracle() {
    const char* const where = "retire_and_slot_reuse";
    Fixture made = Fixture::create(4, 1);
    require(made.ok(), "fixture", "creation failed");

    std::byte a[4];
    auto first = made.ctx->submit_read(made.data_op(a, sizeof(a), 0));
    require(first.has_value(), "fixture", "submit failed");
    require(made.ctx->cancel(first.value().id()).value_or(CancelDisposition::not_found) ==
                CancelDisposition::requested,
            "fixture", "cancel was not requested");
    const Snapshot pending = snap_parity(made.raw, where);
    require(pending.derived_count == 1, "protocol oracle", "sticky intent missing");

    const auto identity = made.raw->live_cookie_for_offset_for_test(0);
    require(identity.has_value(), "fixture", "cookie missing");
    made.raw->inject_cqe_for_test(*identity, static_cast<int>(sizeof(a)));
    (void)made.ctx->poll();
    const Snapshot retired = snap_parity(made.raw, where);
    require(retired.derived_count == 0, "parity oracle",
            "count survived the route retirement (missing decrement on retire)");
    require(retired.authoritative_pending == 0, "parity oracle",
            "scan found a resurrected pending intent after retirement");
    require(first.value().ready(), "protocol oracle", "request did not settle");
    (void)first.value().take_result();
    made.hold->store(false, std::memory_order_release);
    (void)made.ctx->poll();

    std::byte c[4];
    auto third = made.ctx->submit_read(made.data_op(c, sizeof(c), 0));
    require(third.has_value(), "fixture", "reuse submit failed");
    const Snapshot reused = snap_parity(made.raw, where);
    require(reused.derived_count == 0, "parity oracle", "slot reuse resurrected a pending count");
    require(reused.authoritative_pending == 0, "parity oracle",
            "slot reuse resurrected a pending intent");
    require(reused.live_routes == 1, "protocol oracle", "reuse did not produce one live route");
    const auto third_cookie = made.raw->live_cookie_for_offset_for_test(0);
    require(third_cookie.has_value(), "fixture", "reuse cookie missing");
    require(third_cookie != identity, "protocol oracle",
            "slot reuse aliased the retired request identity");
    made.raw->inject_cqe_for_test(*third_cookie, static_cast<int>(sizeof(c)));
    (void)made.ctx->poll();
    (void)third.value().take_result();
    snap_parity(made.raw, where);
    require(made.raw->internal_work_retired(), "protocol oracle", "reuse work not retired");
}

// Steady state without cancellations: the count stays zero and parity holds
// across submit/complete/publish cycles.
void zero_pending_steady_state() {
    const char* const where = "zero_pending_steady_state";
    Fixture made = Fixture::create(4, 8);
    require(made.ok(), "fixture", "creation failed");
    made.hold->store(false, std::memory_order_release);
    std::byte buffer[4];
    for (int cycle = 0; cycle < 5; ++cycle) {
        auto submitted = made.ctx->submit_read(made.data_op(buffer, sizeof(buffer), 0));
        require(submitted.has_value(), "fixture", "submit failed");
        const Snapshot snap = snap_parity(made.raw, where);
        require(snap.derived_count == 0, "parity oracle", "count nonzero without cancels");
        const auto identity = made.raw->live_cookie_for_offset_for_test(0);
        require(identity.has_value(), "fixture", "cookie missing");
        made.raw->inject_cqe_for_test(*identity, static_cast<int>(sizeof(buffer)));
        (void)made.ctx->poll();
        require(submitted.value().ready(), "fixture", "request did not settle");
        snap_parity(made.raw, where);
        (void)submitted.value().take_result();
    }
    snap_parity(made.raw, where);
    require(made.raw->internal_work_retired(), "protocol oracle", "work not retired");
}

// Latch-gated concurrent cancel storm: every thread cancels the same running
// request while a monitor thread checks snapshot parity continuously. The
// sticky intent must register exactly once, survive repeated cancels and
// pressure passes, and settle through the real control path. Synchronization
// is latch/atomic based; no sleep is used as an oracle.
void concurrent_cancel_storm() {
    constexpr int kThreads = 8;
    const char* const where = "concurrent_cancel_storm";

    Fixture made = Fixture::create(8, 2);
    require(made.ok(), "fixture", "fixture creation failed");

    std::byte a[4];
    std::byte b[4];
    auto first = made.ctx->submit_read(made.data_op(a, sizeof(a), 0));
    require(first.has_value(), "fixture", "first submit failed");
    auto second = made.ctx->submit_read(made.data_op(b, sizeof(b), 8));
    require(second.has_value(), "fixture", "second submit failed");

    require(made.ctx->cancel(first.value().id()).value_or(CancelDisposition::not_found) ==
                CancelDisposition::requested,
            "fixture", "priming cancel was not requested");
    const Snapshot primed = snap_parity(made.raw, where);
    require(primed.derived_count == 1, "protocol oracle", "priming sticky intent missing");

    std::latch go{1};
    std::atomic<int> dispositions_ok{0};
    std::atomic<bool> parity_broken{false};
    std::atomic<bool> monitor_done{false};

    std::thread monitor([&] {
        while (!monitor_done.load(std::memory_order_acquire)) {
            const Snapshot snap = made.raw->pending_control_snapshot_for_test();
            if (snap.derived_count != snap.authoritative_pending)
                parity_broken.store(true, std::memory_order_release);
        }
    });

    std::vector<std::thread> cancelers;
    cancelers.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        cancelers.emplace_back([&] {
            go.wait();
            auto seen = made.ctx->cancel(first.value().id());
            if (seen.has_value() && seen.value() == CancelDisposition::requested)
                dispositions_ok.fetch_add(1, std::memory_order_relaxed);
        });
    }
    go.count_down();
    for (auto& t : cancelers)
        t.join();
    monitor_done.store(true, std::memory_order_release);
    monitor.join();

    require(dispositions_ok.load() == kThreads, "protocol oracle",
            "a concurrent cancel was rejected");
    require(!parity_broken.load(), "parity oracle", "parity diverged during the cancel storm");
    const Snapshot stormed = snap_parity(made.raw, where);
    require(stormed.derived_count == 1, "parity oracle",
            "cancel storm double-counted or dropped the sticky intent");

    for (int pass = 0; pass < 3; ++pass) {
        (void)made.ctx->poll();
        const Snapshot held = snap_parity(made.raw, where);
        require(held.derived_count == 1, "protocol oracle", "sticky intent lost under pressure");
    }

    made.hold->store(false, std::memory_order_release);
    (void)made.ctx->poll();
    (void)made.ctx->poll();
    require(made.raw->live_control_sqes_for_test() == 1, "protocol oracle",
            "intent was not issued after pressure release");
    snap_parity(made.raw, where);

    const auto first_cookie = made.raw->live_cookie_for_offset_for_test(0);
    const auto second_cookie = made.raw->live_cookie_for_offset_for_test(8);
    require(first_cookie.has_value() && second_cookie.has_value(), "fixture", "cookies missing");
    made.raw->inject_cqe_for_test(kControlTag | *first_cookie, 0);
    made.raw->inject_cqe_for_test(*first_cookie, -ECANCELED);
    made.raw->inject_cqe_for_test(*second_cookie, static_cast<int>(sizeof(b)));
    (void)made.ctx->poll();

    const Snapshot settled = snap_parity(made.raw, where);
    require(settled.derived_count == 0, "parity oracle", "count not zero after settle");
    require(made.raw->internal_work_retired(), "protocol oracle", "control work not retired");
    const auto first_seen = first.value().take_result();
    const auto second_seen = second.value().take_result();
    require(first_seen.readiness == RequestReadiness::ready &&
                second_seen.readiness == RequestReadiness::ready,
            "protocol oracle", "requests did not settle");
}

// Poison transition while a sticky pending control exists: the derived count
// must stay consistent through poison recovery, shutdown must converge, every
// accepted request must reach a terminal state, and the count must return to
// zero as the router retires.
void poison_with_pending_intent() {
    const char* const where = "poison_with_pending_intent";

    auto poison = std::make_unique<PoisonState>();
    Fixture made;
    made.backing = ::tmpfile();
    require(made.backing != nullptr, "fixture", "tmpfile failed");
    std::byte content[4096];
    std::memset(content, 0x33, sizeof(content));
    require(::fwrite(content, 1, sizeof(content), made.backing) == sizeof(content), "fixture",
            "fixture write failed");
    ::rewind(made.backing);
    UringBackendSubmitTestHooks hooks;
    hooks.submit = poison_submit_hook;
    hooks.context = poison.get();
    auto owned = std::make_unique<UringAsyncBackend>(UringConfig{4, 1}, hooks);
    made.raw = owned.get();
    made.ctx = std::make_unique<AsyncIoContext>(detail::make_context(std::move(owned)));
    require(made.ok(), "fixture", "fixture creation failed");

    std::byte a[4];
    std::byte b[4];
    auto first = made.ctx->submit_read(made.data_op(a, sizeof(a), 0));
    require(first.has_value(), "fixture", "first submit failed");
    auto second = made.ctx->submit_read(made.data_op(b, sizeof(b), 8));
    require(second.has_value(), "fixture", "second submit failed");
    require(made.ctx->cancel(first.value().id()).value_or(CancelDisposition::not_found) ==
                CancelDisposition::requested,
            "fixture", "cancel was not requested");
    const Snapshot pending = snap_parity(made.raw, where);
    require(pending.derived_count == 1, "protocol oracle", "sticky intent missing");

    poison->fail_next.store(true, std::memory_order_release);
    poison->hold.store(false, std::memory_order_release);
    (void)made.ctx->poll();

    snap_parity(made.raw, where);

    const auto outcome = made.ctx->shutdown(ShutdownPolicy::drain);
    require(outcome.has_value(), "protocol oracle", "shutdown failed to converge after poison");

    const Snapshot drained = snap_parity(made.raw, where);
    require(drained.derived_count == 0, "parity oracle",
            "poison left a nonzero derived count behind");
    require(drained.authoritative_pending == 0, "parity oracle",
            "poison left a pending intent behind");
    const auto first_seen = first.value().take_result();
    const auto second_seen = second.value().take_result();
    require(first_seen.readiness == RequestReadiness::ready &&
                second_seen.readiness == RequestReadiness::ready,
            "protocol oracle", "poison left a request without terminal responsibility");
}

// Shutdown converging after a sticky pending control was issued and settled:
// no stale intent may survive and the canceled request keeps its terminal
// responsibility through shutdown.
void shutdown_after_pending_intent_settles() {
    const char* const where = "shutdown_after_settle";

    Fixture made = Fixture::create(4, 1);
    require(made.ok(), "fixture", "fixture creation failed");

    std::byte a[4];
    auto first = made.ctx->submit_read(made.data_op(a, sizeof(a), 0));
    require(first.has_value(), "fixture", "submit failed");
    require(made.ctx->cancel(first.value().id()).value_or(CancelDisposition::not_found) ==
                CancelDisposition::requested,
            "fixture", "cancel was not requested");
    const Snapshot pending = snap_parity(made.raw, where);
    require(pending.derived_count == 1, "protocol oracle", "sticky intent missing");

    made.hold->store(false, std::memory_order_release);
    (void)made.ctx->poll();
    (void)made.ctx->poll();
    require(made.raw->live_control_sqes_for_test() == 1, "protocol oracle",
            "intent was not issued after pressure release");
    const auto identity = made.raw->live_cookie_for_offset_for_test(0);
    require(identity.has_value(), "fixture", "cookie missing");
    made.raw->inject_cqe_for_test(kControlTag | *identity, 0);
    made.raw->inject_cqe_for_test(*identity, -ECANCELED);
    (void)made.ctx->poll();
    const Snapshot settled = snap_parity(made.raw, where);
    require(settled.derived_count == 0, "parity oracle", "count not zero after settle");

    const auto outcome = made.ctx->shutdown(ShutdownPolicy::drain);
    require(outcome.has_value(), "protocol oracle", "shutdown did not converge after settle");

    const Snapshot drained = snap_parity(made.raw, where);
    require(drained.derived_count == 0, "parity oracle", "shutdown left a nonzero derived count");
    const auto first_seen = first.value().take_result();
    require(first_seen.readiness == RequestReadiness::ready, "protocol oracle",
            "shutdown lost terminal responsibility for the canceled request");
}

} // namespace

int main() {
    ::alarm(180);
    struct NamedTest {
        const char* name;
        void (*fn)();
    };
    const NamedTest tests[] = {
        {"sticky_lifecycle_oracle", sticky_lifecycle_oracle},
        {"repeated_cancel_oracle", repeated_cancel_oracle},
        {"retire_and_slot_reuse_oracle", retire_and_slot_reuse_oracle},
        {"zero_pending_steady_state", zero_pending_steady_state},
        {"concurrent_cancel_storm", concurrent_cancel_storm},
        {"poison_with_pending_intent", poison_with_pending_intent},
        {"shutdown_after_pending_intent_settles", shutdown_after_pending_intent_settles},
    };
    for (const NamedTest& t : tests) {
        t.fn();
        std::printf("ok %s\n", t.name);
        std::fflush(stdout);
    }
    std::printf("all uring pending-control parity tests passed\n");
    return 0;
}
