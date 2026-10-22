// E1 mutant-oracle test (research-only).
//
// The stock uring_pending_control_parity_test kills both E1 mutants, but the
// missing-increment kill surfaces as a request-destructor abort (rc 134)
// because stack unwinding destroys unsettled requests before main can print
// the named FAIL. This companion oracle pins the same mutants to named
// parity/protocol assertion failures: it reports through fail()+_Exit so no
// destructor can preempt the named verdict, and it adds the protocol oracle
// "a sticky intent under SQE pressure must be issued once pressure
// releases". The healthy build must pass; each mutant build must fail with
// its named oracle message.
//
// Build recipe mirrors xmake uring_progress_target: compile the async TUs
// from the candidate worktree with -DSLUICE_ASYNC_INTERNAL_TESTING
// -DSLUICE_HAS_LIBURING and link -luring.

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
#include <memory>
#include <vector>

namespace {

using namespace sluice::async;

constexpr std::uint64_t kControlTag = std::uint64_t{1} << 63u;

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

// Sticky-cancel lifecycle under SQE pressure, pinned by two named oracles:
// parity (count == authoritative scan) at every checkpoint, and the protocol
// property that a released pressure lets the next pass issue the intent.
void sticky_lifecycle_oracle() {
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

    const std::size_t scan = made.raw->pending_control_scan_for_test();
    const std::size_t count = made.raw->pending_control_count_for_test();
    require(scan == 1, "protocol oracle", "sticky intent did not register in the router");
    require(count == scan, "parity oracle",
            "pending_control_count_ diverged from the router scan after the sticky set "
            "(missing increment)");

    for (int pass = 0; pass < 3; ++pass) {
        (void)made.ctx->poll();
        require(made.raw->pending_control_count_for_test() ==
                    made.raw->pending_control_scan_for_test(),
                "parity oracle", "divergence across pressure passes");
        require(made.raw->pending_control_count_for_test() == 1, "protocol oracle",
                "sticky intent lost while SQE pressure persists");
    }

    made.hold->store(false, std::memory_order_release);
    (void)made.ctx->poll();
    (void)made.ctx->poll();
    require(made.raw->live_control_sqes_for_test() == 1, "protocol oracle",
            "released pressure did not issue the pending control SQE (lost intent)");
    require(made.raw->pending_control_count_for_test() == 0, "parity oracle",
            "count did not retire with the issued intent (missing decrement)");

    const auto first_cookie = made.raw->live_cookie_for_offset_for_test(0);
    const auto second_cookie = made.raw->live_cookie_for_offset_for_test(8);
    require(first_cookie.has_value() && second_cookie.has_value(), "fixture", "cookies missing");
    made.raw->inject_cqe_for_test(kControlTag | *first_cookie, 0);
    made.raw->inject_cqe_for_test(*first_cookie, -ECANCELED);
    made.raw->inject_cqe_for_test(*second_cookie, static_cast<int>(sizeof(b)));
    (void)made.ctx->poll();

    require(made.raw->pending_control_count_for_test() ==
                made.raw->pending_control_scan_for_test(),
            "parity oracle", "divergence after settle");
    require(made.raw->internal_work_retired(), "protocol oracle", "control work not retired");
    const auto first_seen = first.value().take_result();
    const auto second_seen = second.value().take_result();
    require(first_seen.readiness == RequestReadiness::ready &&
                second_seen.readiness == RequestReadiness::ready &&
                second_seen.result.has_value() &&
                second_seen.result.value() == sizeof(b),
            "protocol oracle", "requests did not settle through the control path");
}

// Original-CQE-first retirement of a pending intent, pinned by the parity
// oracle: the route dies with the intent and the count follows without a
// resurrection on slot reuse.
void retire_with_route_oracle() {
    Fixture made = Fixture::create(4, 1);
    require(made.ok(), "fixture", "creation failed");

    std::byte a[4];
    auto first = made.ctx->submit_read(made.data_op(a, sizeof(a), 0));
    require(first.has_value(), "fixture", "submit failed");
    require(made.ctx->cancel(first.value().id()).value_or(CancelDisposition::not_found) ==
                CancelDisposition::requested,
            "fixture", "cancel was not requested");
    require(made.raw->pending_control_count_for_test() == 1, "protocol oracle",
            "sticky intent missing");

    const auto identity = made.raw->live_cookie_for_offset_for_test(0);
    require(identity.has_value(), "fixture", "cookie missing");
    made.raw->inject_cqe_for_test(*identity, static_cast<int>(sizeof(a)));
    (void)made.ctx->poll();
    require(made.raw->pending_control_count_for_test() == 0, "parity oracle",
            "count survived the route retirement (missing decrement on retire)");
    require(made.raw->pending_control_scan_for_test() == 0, "parity oracle",
            "scan found a resurrected pending intent after retirement");
    require(first.value().ready(), "protocol oracle", "request did not settle");
    (void)first.value().take_result();
    made.hold->store(false, std::memory_order_release);
    (void)made.ctx->poll();

    // Slot reuse: a fresh operation on the freed slot must start with a clean
    // control state and a zero derived count.
    std::byte c[4];
    auto third = made.ctx->submit_read(made.data_op(c, sizeof(c), 0));
    require(third.has_value(), "fixture", "reuse submit failed");
    require(made.raw->pending_control_count_for_test() == 0, "parity oracle",
            "slot reuse resurrected a pending count");
    require(made.raw->pending_control_scan_for_test() == 0, "parity oracle",
            "slot reuse resurrected a pending intent");
    const auto third_cookie = made.raw->live_cookie_for_offset_for_test(0);
    require(third_cookie.has_value(), "fixture", "reuse cookie missing");
    made.raw->inject_cqe_for_test(*third_cookie, static_cast<int>(sizeof(c)));
    (void)made.ctx->poll();
    (void)third.value().take_result();
    require(made.raw->internal_work_retired(), "protocol oracle", "reuse work not retired");
}

} // namespace

int main() {
    ::alarm(120);
    sticky_lifecycle_oracle();
    retire_with_route_oracle();
    std::printf("all e1 mutant-oracle checks passed\n");
    return 0;
}
