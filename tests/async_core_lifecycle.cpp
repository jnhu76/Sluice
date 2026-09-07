// Async core lifecycle tests, rebuilt from the current implementation.
// Each test pins one surviving invariant of the Completion FSM and the
// RequestArena slot lifecycle as they behave in the live submit_transaction
// path. Run via the sluice_async_core_lifecycle target; optional argv[1]
// filters tests by substring.

#include "async_test_kit.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/ready_sink.hpp>
#include <sluice/async/detail/request_arena.hpp>
#include <sluice/async/detail/request_key.hpp>
#include <sluice/async/detail/submit_transaction.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <memory>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace {

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;
using sluice::make_unexpected;
namespace detail = sluice::async::detail;

// The Completion claim/binding transitions are private and reachable only
// through the AsyncBackend protected static helpers — the designed backend
// boundary. This host re-exposes exactly those statics for tests, mirroring
// how production backends' SubmitPolicies consume them. It never acts as a
// runtime (no threads, no I/O) and exists only in this test TU.
class TestBindingAccess : public AsyncBackend {
  public:
    using AsyncBackend::begin_binding;
    using AsyncBackend::commit_binding;
    using AsyncBackend::rollback_binding_before_accept;
    using AsyncBackend::install_binding;
    using AsyncBackend::publish;
};

// Minimal synthetic op: the lifecycle invariants under test do not depend on
// real read/write semantics, only on the bytes published for size completions.
struct TestOp {
    std::uint64_t len = 0;
};

// Two-phase submit policy shaped like the production backend policies: it
// forwards the FSM transitions through AsyncBackend's static helpers and
// publishes TerminalResult into the bound completion at reap time.
template <class Comp> struct TestSubmitPolicy {
    using completion_type = Comp;
    using op_type = TestOp;

    explicit TestSubmitPolicy(detail::OperationKind kind) noexcept : kind_(kind) {}

    detail::OperationKind kind() const noexcept { return kind_; }

    static detail::BorrowMetadata borrow(const TestOp&) noexcept { return {}; }

    static std::uint64_t requested_bytes(const TestOp& op) noexcept {
        if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
            return op.len;
        } else {
            return 0;
        }
    }

    static void publish_ready(void* completion, const detail::TerminalResult& t) noexcept {
        if constexpr (std::is_same_v<Comp, Completion<std::size_t>>) {
            if (t.stored && t.is_error) {
                TestBindingAccess::publish(*static_cast<Comp*>(completion),
                                      sluice::make_unexpected<std::size_t>(t.error));
            } else {
                TestBindingAccess::publish(*static_cast<Comp*>(completion),
                                      sluice::Result<std::size_t>{static_cast<std::size_t>(t.bytes)});
            }
        } else {
            if (t.stored && t.is_error) {
                TestBindingAccess::publish(*static_cast<Comp*>(completion),
                                      sluice::make_unexpected<void>(t.error));
            } else {
                TestBindingAccess::publish(*static_cast<Comp*>(completion), sluice::Result<void>{});
            }
        }
    }

    static auto publish_thunk() noexcept { return &TestSubmitPolicy::publish_ready; }

    static bool begin_binding(Comp& c) noexcept { return TestBindingAccess::begin_binding(c); }
    static void install_binding(Comp& c, detail::RequestArena* arena,
                                detail::SlotHandle h) noexcept {
        TestBindingAccess::install_binding(c, arena, h);
    }
    static void commit_binding(Comp& c) noexcept { TestBindingAccess::commit_binding(c); }
    static void rollback_binding(Comp& c) noexcept {
        TestBindingAccess::rollback_binding_before_accept(c);
    }

    Result<void> stage0_precheck() const noexcept { return {}; }
    Result<void> validate(const TestOp&) const noexcept {
        if (fail_validate_) {
            return sluice::make_unexpected<void>(IoError{IoError::Code::invalid_argument});
        }
        return {};
    }
    void write_scratch(detail::SlotHandle, const TestOp&) const noexcept {}
    void pause_before_commit_binding() const noexcept {}

    bool fail_validate_ = false;

  private:
    detail::OperationKind kind_;
};

struct CountingSink final : detail::SynchronousReadySink {
    int calls = 0;
    detail::RequestKey last_key{};
    void on_ready(detail::ReadyEvent event) noexcept override {
        ++calls;
        last_key = event.key;
    }
};

template <class Comp>
Result<detail::SlotHandle> submit_ok(detail::RequestArena& arena, Comp& c, std::uint64_t len) {
    constexpr detail::OperationKind kind =
        std::is_same_v<Comp, Completion<std::size_t>> ? detail::OperationKind::read
                                                      : detail::OperationKind::sync_data;
    TestSubmitPolicy<Comp> policy{kind};
    TestOp op{len};
    auto h = detail::submit_transaction(arena, c, op, policy);
    if (h.has_value()) {
        CHECK(arena.enqueue(h.value()) == detail::EnqueueOutcome::enqueued);
    }
    return h;
}

// A Completion destroyed while outstanding fail-fasts by design; each test
// must leave the completion idle. This drains the accepted request
// (terminalize -> reap -> reset) so the lifecycle contract is honored.
template <class Comp>
void drain_and_reset(detail::RequestArena& arena, Comp& c, detail::SlotHandle h) {
    CountingSink sink;
    (void)arena.record_terminal(h, detail::TerminalResult::ok_bytes(0));
    (void)arena.reap(sink);
    c.reset();
}

// T0-1: a successfully accepted request claims the completion through the
// two-phase binding (idle -> binding -> outstanding) and occupies one slot.
SLUICE_TEST(T0_1_completion_binding_success) {
    detail::RequestArena arena{detail::ContextIdentity::for_testing(1), 4};
    Completion<std::size_t> c;
    CHECK(c.idle());

    auto h = submit_ok(arena, c, 128);
    CHECK(h.has_value());
    CHECK(c.outstanding());
    CHECK(!c.ready());
    CHECK(!c.idle());
    CHECK(arena.slot_in_use() == 1);
    CHECK(arena.accepted_outstanding() == 1);

    drain_and_reset(arena, c, h.value());
    CHECK(c.idle());
}

// T0-2: a rejected submit rolls the binding back (binding -> idle) and
// releases the reserved slot; the completion can be bound again afterwards.
SLUICE_TEST(T0_2_binding_rollback_to_idle) {
    detail::RequestArena arena{detail::ContextIdentity::for_testing(1), 4};
    Completion<std::size_t> c;

    TestSubmitPolicy<Completion<std::size_t>> policy{detail::OperationKind::read};
    policy.fail_validate_ = true;
    TestOp op{64};
    auto rejected = detail::submit_transaction(arena, c, op, policy);
    CHECK(!rejected.has_value());
    CHECK(c.idle());
    CHECK(!c.outstanding());
    CHECK(!c.ready());
    CHECK(arena.slot_in_use() == 0);

    auto h = submit_ok(arena, c, 64);
    CHECK(h.has_value());
    CHECK(c.outstanding());

    drain_and_reset(arena, c, h.value());
    CHECK(c.idle());
}

// T0-3: backend terminalization (record_terminal) is not Completion
// publication: after terminalization the slot holds a terminal result and the
// completion is still outstanding, not ready.
SLUICE_TEST(T0_3_terminalization_is_not_publication) {
    detail::RequestArena arena{detail::ContextIdentity::for_testing(1), 4};
    Completion<std::size_t> c;
    auto h = submit_ok(arena, c, 128);
    CHECK(h.has_value());

    CHECK(arena.record_terminal(h.value(), detail::TerminalResult::ok_bytes(128)));
    CHECK(arena.backend_ready_count() == 1);
    CHECK(!c.ready());
    CHECK(c.outstanding());

    drain_and_reset(arena, c, h.value());
    CHECK(!c.outstanding());
}

// T0-4: reap publishes exactly once (outstanding -> publishing -> ready) and
// routes one ReadyEvent; further reaps publish nothing and the result stays.
SLUICE_TEST(T0_4_reap_publishes_once) {
    detail::RequestArena arena{detail::ContextIdentity::for_testing(1), 4};
    Completion<std::size_t> c;
    auto h = submit_ok(arena, c, 128);
    CHECK(h.has_value());
    CHECK(arena.record_terminal(h.value(), detail::TerminalResult::ok_bytes(128)));

    CountingSink sink;
    CHECK(arena.reap(sink) == 1);
    CHECK(c.ready());
    CHECK(!c.outstanding());
    CHECK(c.result().has_value());
    CHECK(c.result().value() == 128);
    CHECK(sink.calls == 1);
    const detail::RequestKey expected_key{detail::ContextIdentity::for_testing(1), h.value().slot,
                                          h.value().generation};
    CHECK(sink.last_key == expected_key);

    CHECK(arena.reap(sink) == 0);
    CHECK(c.ready());
    CHECK(sink.calls == 1);
    CHECK(c.result().value() == 128);

    c.reset();
    CHECK(c.idle());
}

// T0-5: reset moves a ready completion back to idle (ready -> resetting ->
// idle), releases the bound slot for reuse, and is a no-op when already idle.
SLUICE_TEST(T0_5_ready_reset_release) {
    detail::RequestArena arena{detail::ContextIdentity::for_testing(1), 4};
    Completion<std::size_t> c;
    auto h = submit_ok(arena, c, 128);
    CHECK(h.has_value());
    CHECK(arena.record_terminal(h.value(), detail::TerminalResult::ok_bytes(128)));
    CountingSink sink;
    CHECK(arena.reap(sink) == 1);
    CHECK(c.ready());

    c.reset();
    CHECK(c.idle());
    CHECK(!c.ready());
    CHECK(!c.outstanding());
    CHECK(arena.slot_in_use() == 0);
    CHECK(arena.accepted_outstanding() == 0);

    c.reset();
    CHECK(c.idle());

    auto h2 = submit_ok(arena, c, 32);
    CHECK(h2.has_value());
    CHECK(c.outstanding());

    drain_and_reset(arena, c, h2.value());
    CHECK(c.idle());
}

// T0-6: slot generation blocks stale reuse — a handle held across a
// free/reuse cycle no longer terminalizes, cancels, or resolves against the
// reused slot, and the live request publishes its own result.
SLUICE_TEST(T0_6_generation_blocks_stale_reuse) {
    detail::RequestArena arena{detail::ContextIdentity::for_testing(1), 2};
    Completion<std::size_t> c1;
    auto h1 = submit_ok(arena, c1, 8);
    CHECK(h1.has_value());
    CHECK(arena.record_terminal(h1.value(), detail::TerminalResult::ok_bytes(8)));
    CountingSink sink;
    CHECK(arena.reap(sink) == 1);
    c1.reset();
    CHECK(arena.slot_in_use() == 0);

    Completion<std::size_t> c2;
    auto h2 = submit_ok(arena, c2, 16);
    CHECK(h2.has_value());
    CHECK(h2.value().slot == h1.value().slot);
    CHECK(h2.value().generation.value == h1.value().generation.value + 1);

    CHECK(!arena.record_terminal(h1.value(), detail::TerminalResult::ok_bytes(999)));
    CHECK(arena.cancel(h1.value()) == detail::CancelDisposition::not_found);

    CHECK(arena.record_terminal(h2.value(), detail::TerminalResult::ok_bytes(16)));
    CHECK(arena.reap(sink) == 1);
    CHECK(c2.ready());
    CHECK(c2.result().has_value());
    CHECK(c2.result().value() == 16);
    CHECK(sink.calls == 2);

    c2.reset();
    CHECK(arena.slot_in_use() == 0);
}


// A backend exercising the interface capability defaults: it implements only
// the pure-virtual core, so every optional capability falls through to the
// interface default.
class BareCapabilitiesBackend final : public AsyncBackend {
  public:
    Result<void> submit_read(ReadOp, Completion<std::size_t>&) override {
        return make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
    Result<void> submit_write(WriteOp, Completion<std::size_t>&) override {
        return make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&) override {
        return make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&) override {
        return make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override { return Result<std::size_t>{0}; }
    std::size_t outstanding() const noexcept override { return 0; }
};

// A backend that does not implement cancellation must report that explicitly
// like every other unsupported optional capability (the register_waiter /
// cancel_waiter default convention), instead of silently claiming success.
SLUICE_TEST(C1_cancel_unsupported_backend_is_explicit) {
    AsyncIoContext ctx{std::make_unique<BareCapabilitiesBackend>()};
    Completion<std::size_t> cs;
    auto rs = ctx.cancel(cs);
    CHECK(rs.has_value() == false);
    CHECK(rs.error().code == IoError::Code::not_supported);
    Completion<void> cv;
    auto rv = ctx.cancel(cv);
    CHECK(rv.has_value() == false);
    CHECK(rv.error().code == IoError::Code::not_supported);
}

} // namespace

int main(int argc, char** argv) {
    return ::sluice_test::run_all(argc, argv);
}
