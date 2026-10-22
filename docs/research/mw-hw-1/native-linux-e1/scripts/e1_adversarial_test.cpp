// E1 adversarial-scenario test (research-only).
//
// Complements the stock parity suite with the scenarios it does not cover:
// a latch-gated concurrent cancel storm against one running request, a
// backend poison transition with a sticky pending control present, and a
// shutdown converging while a sticky pending control exists. All ordering
// comes from latches, hook state and poll passes; the only timing bounds are
// watchdog alarms. Every checkpoint asserts the derived-count parity oracle
// (pending_control_count_ == authoritative router scan).

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/uring_backend.hpp>
#include <sluice/async/request.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <liburing.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <latch>
#include <memory>
#include <thread>
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

[[noreturn]] void fail(const char* scenario, const char* detail) {
    std::fprintf(stderr, "FAIL %s: %s\n", scenario, detail);
    std::fflush(stderr);
    std::_Exit(1);
}

void require(bool condition, const char* scenario, const char* detail) {
    if (!condition)
        fail(scenario, detail);
}

void require_parity(UringAsyncBackend* raw, const char* scenario, const char* where) {
    const std::size_t count = raw->pending_control_count_for_test();
    const std::size_t scan = raw->pending_control_scan_for_test();
    if (count != scan) {
        std::fprintf(stderr, "FAIL %s: parity at %s count=%zu scan=%zu\n", scenario, where, count,
                     scan);
        std::fflush(stderr);
        std::_Exit(1);
    }
}

// Latch-gated concurrent cancel storm: every thread cancels the same running
// request while a monitor thread checks parity continuously. The sticky
// intent must register exactly once, survive repeated cancels and pressure
// passes, and settle through the real control path.
void concurrent_cancel_storm() {
    constexpr int kThreads = 8;
    const char* scenario = "concurrent_cancel_storm";

    Fixture made = Fixture::create(8, 2);
    require(made.ok(), scenario, "fixture creation failed");

    std::byte a[4];
    std::byte b[4];
    auto first = made.ctx->submit_read(made.data_op(a, sizeof(a), 0));
    require(first.has_value(), scenario, "first submit failed");
    auto second = made.ctx->submit_read(made.data_op(b, sizeof(b), 8));
    require(second.has_value(), scenario, "second submit failed");

    require(made.ctx->cancel(first.value().id()).value_or(CancelDisposition::not_found) ==
                CancelDisposition::requested,
            scenario, "priming cancel was not requested");
    require_parity(made.raw, scenario, "after priming cancel");
    require(made.raw->pending_control_count_for_test() == 1, scenario,
            "priming sticky intent missing");

    std::latch go{1};
    std::atomic<int> dispositions_ok{0};
    std::atomic<bool> parity_broken{false};
    std::atomic<bool> monitor_done{false};

    std::thread monitor([&] {
        while (!monitor_done.load(std::memory_order_acquire)) {
            if (made.raw->pending_control_count_for_test() !=
                made.raw->pending_control_scan_for_test()) {
                parity_broken.store(true, std::memory_order_release);
            }
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

    require(dispositions_ok.load() == kThreads, scenario, "a concurrent cancel was rejected");
    require(!parity_broken.load(), scenario, "parity diverged during the cancel storm");
    require_parity(made.raw, scenario, "after cancel storm");
    require(made.raw->pending_control_count_for_test() == 1, scenario,
            "cancel storm double-counted or dropped the sticky intent");

    for (int pass = 0; pass < 3; ++pass) {
        (void)made.ctx->poll();
        require_parity(made.raw, scenario, "pressure pass");
        require(made.raw->pending_control_count_for_test() == 1, scenario,
                "sticky intent lost under pressure");
    }

    made.hold->store(false, std::memory_order_release);
    (void)made.ctx->poll();
    (void)made.ctx->poll();
    require(made.raw->live_control_sqes_for_test() == 1, scenario,
            "intent was not issued after pressure release");
    require_parity(made.raw, scenario, "after issue");

    const auto first_cookie = made.raw->live_cookie_for_offset_for_test(0);
    const auto second_cookie = made.raw->live_cookie_for_offset_for_test(8);
    require(first_cookie.has_value() && second_cookie.has_value(), scenario, "cookies missing");
    made.raw->inject_cqe_for_test(kControlTag | *first_cookie, 0);
    made.raw->inject_cqe_for_test(*first_cookie, -ECANCELED);
    made.raw->inject_cqe_for_test(*second_cookie, static_cast<int>(sizeof(b)));
    (void)made.ctx->poll();

    require_parity(made.raw, scenario, "after settle");
    require(made.raw->pending_control_count_for_test() == 0, scenario,
            "count not zero after settle");
    require(made.raw->internal_work_retired(), scenario, "control work not retired");
    const auto first_seen = first.value().take_result();
    const auto second_seen = second.value().take_result();
    require(first_seen.readiness == RequestReadiness::ready &&
                second_seen.readiness == RequestReadiness::ready,
            scenario, "requests did not settle");
}

// Poison transition while a sticky pending control exists: the derived count
// must stay consistent through the poison, shutdown must converge, every
// accepted request must reach a terminal state, and the count must return to
// zero as the router retires.
void poison_with_pending_intent() {
    const char* scenario = "poison_with_pending_intent";

    auto poison = std::make_unique<PoisonState>();
    Fixture made;
    made.backing = ::tmpfile();
    require(made.backing != nullptr, scenario, "tmpfile failed");
    std::byte content[4096];
    std::memset(content, 0x33, sizeof(content));
    require(::fwrite(content, 1, sizeof(content), made.backing) == sizeof(content), scenario,
            "fixture write failed");
    ::rewind(made.backing);
    UringBackendSubmitTestHooks hooks;
    hooks.submit = poison_submit_hook;
    hooks.context = poison.get();
    auto owned = std::make_unique<UringAsyncBackend>(UringConfig{4, 1}, hooks);
    made.raw = owned.get();
    made.ctx = std::make_unique<AsyncIoContext>(detail::make_context(std::move(owned)));
    require(made.ok(), scenario, "fixture creation failed");

    std::byte a[4];
    std::byte b[4];
    auto first = made.ctx->submit_read(made.data_op(a, sizeof(a), 0));
    require(first.has_value(), scenario, "first submit failed");
    auto second = made.ctx->submit_read(made.data_op(b, sizeof(b), 8));
    require(second.has_value(), scenario, "second submit failed");
    require(made.ctx->cancel(first.value().id()).value_or(CancelDisposition::not_found) ==
                CancelDisposition::requested,
            scenario, "cancel was not requested");
    require(made.raw->pending_control_count_for_test() == 1, scenario, "sticky intent missing");
    require_parity(made.raw, scenario, "before poison");

    // The next service attempt must go through a submit that poisons.
    poison->fail_next.store(true, std::memory_order_release);
    poison->hold.store(false, std::memory_order_release);
    (void)made.ctx->poll();

    require_parity(made.raw, scenario, "after poison");

    const auto outcome = made.ctx->shutdown(ShutdownPolicy::drain);
    require(outcome.has_value(), scenario, "shutdown failed to converge after poison");

    require_parity(made.raw, scenario, "after shutdown");
    require(made.raw->pending_control_count_for_test() == 0, scenario,
            "poison left a nonzero derived count behind");
    require(made.raw->pending_control_scan_for_test() == 0, scenario,
            "poison left a pending intent behind");
    const auto first_seen = first.value().take_result();
    const auto second_seen = second.value().take_result();
    require(first_seen.readiness == RequestReadiness::ready &&
                second_seen.readiness == RequestReadiness::ready,
            scenario, "poison left a request without terminal responsibility");
}

// Shutdown converging after a sticky pending control was issued and settled:
// no stale intent may survive and the canceled request keeps its terminal
// responsibility through shutdown.
void shutdown_after_pending_intent_settles() {
    const char* scenario = "shutdown_after_pending_intent_settles";

    Fixture made = Fixture::create(4, 1);
    require(made.ok(), scenario, "fixture creation failed");

    std::byte a[4];
    auto first = made.ctx->submit_read(made.data_op(a, sizeof(a), 0));
    require(first.has_value(), scenario, "submit failed");
    require(made.ctx->cancel(first.value().id()).value_or(CancelDisposition::not_found) ==
                CancelDisposition::requested,
            scenario, "cancel was not requested");
    require(made.raw->pending_control_count_for_test() == 1, scenario, "sticky intent missing");
    require_parity(made.raw, scenario, "before pressure release");

    made.hold->store(false, std::memory_order_release);
    (void)made.ctx->poll();
    (void)made.ctx->poll();
    require(made.raw->live_control_sqes_for_test() == 1, scenario,
            "intent was not issued after pressure release");
    const auto identity = made.raw->live_cookie_for_offset_for_test(0);
    require(identity.has_value(), scenario, "cookie missing");
    made.raw->inject_cqe_for_test(kControlTag | *identity, 0);
    made.raw->inject_cqe_for_test(*identity, -ECANCELED);
    (void)made.ctx->poll();
    require_parity(made.raw, scenario, "after settle");
    require(made.raw->pending_control_count_for_test() == 0, scenario,
            "count not zero after settle");

    const auto outcome = made.ctx->shutdown(ShutdownPolicy::drain);
    require(outcome.has_value(), scenario, "shutdown did not converge after settle");

    require_parity(made.raw, scenario, "after shutdown");
    require(made.raw->pending_control_count_for_test() == 0, scenario,
            "shutdown left a nonzero derived count");
    const auto first_seen = first.value().take_result();
    require(first_seen.readiness == RequestReadiness::ready, scenario,
            "shutdown lost terminal responsibility for the canceled request");
}

// Bounded probe (behavioral parity evidence, not a pass/fail gate): a request
// in flight under a permanently wedged transport, shutdown(cancel_then_drain)
// must converge or not identically across arms. A permanently wedged
// transport is outside the contract's reachable states, so convergence is
// recorded per arm and compared; a hang here is bounded, never infinite.
struct ProbeResult {
    bool converged = false;
    bool request_terminal = false;
};

ProbeResult probe_cancel_then_drain_wedged() {
    ProbeResult result;
    // Leaked on purpose: destroying the context while the shutdown thread is
    // stuck inside shutdown() deadlocks before the probe line can print. The
    // process ends via _Exit right after the verdict.
    static Fixture* made = new Fixture(Fixture::create(4, 1));
    if (!made->ok())
        return result;
    std::byte a[4];
    // Also leaked: the request can never settle under the wedged transport,
    // and destroying a nonterminal Request is a contract abort.
    auto first = new auto(made->ctx->submit_read(made->data_op(a, sizeof(a), 0)));
    if (!first->has_value())
        return result;
    std::atomic<bool> shutdown_done{false};
    std::thread shutdowner([&] {
        (void)made->ctx->shutdown(ShutdownPolicy::cancel_then_drain);
        shutdown_done.store(true, std::memory_order_release);
    });
    for (int i = 0; i < 100 && !shutdown_done.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    result.converged = shutdown_done.load(std::memory_order_acquire);
    result.request_terminal = first->value().ready();
    shutdowner.detach();
    return result;
}

void shutdown_cancel_then_drain_wedged_probe() {
    const char* scenario = "shutdown_cancel_then_drain_wedged_probe";
    const ProbeResult probe = probe_cancel_then_drain_wedged();
    std::fprintf(stderr,
                 "probe %s: converged=%d request_terminal=%d\n", scenario,
                 probe.converged ? 1 : 0, probe.request_terminal ? 1 : 0);
    std::fflush(stderr);
    // The wedged-shutdown thread can never be joined; the probe's verdict is
    // its printed line and the process ends here by design.
    std::_Exit(0);
}

} // namespace

int main() {
    ::alarm(180);
    std::fprintf(stderr, "enter concurrent_cancel_storm\n");
    concurrent_cancel_storm();
    std::fprintf(stderr, "enter poison_with_pending_intent\n");
    poison_with_pending_intent();
    std::fprintf(stderr, "enter shutdown_after_pending_intent_settles\n");
    shutdown_after_pending_intent_settles();
    std::fprintf(stderr, "enter shutdown_cancel_then_drain_wedged_probe\n");
    shutdown_cancel_then_drain_wedged_probe();
    std::printf("all e1 adversarial scenarios passed\n");
    return 0;
}
