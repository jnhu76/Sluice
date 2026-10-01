#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include <liburing.h>

#include <cerrno>
#include <csignal>

namespace {

using namespace sluice::async;
using sluice::IoError;

using PauseGate = detail::ProgressSource::PauseGate;

// Borrows the context notification fd for readiness probes and retires the
// external interest on scope exit.
struct HostInterest {
    AsyncIoContext& ctx;
    explicit HostInterest(AsyncIoContext& c) noexcept : ctx(c) {
        (void)ctx.progress_notification_fd();
    }
    ~HostInterest() { ctx.detach_progress_host(); }
};

bool notification_fd_readable(int fd) {
    if (fd < 0)
        return false;
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    const int rc = ::poll(&p, 1, 0);
    return rc > 0 && (p.revents & POLLIN) != 0;
}

bool wait_notification_readable(AsyncIoContext& ctx) {
    const int fd = ctx.progress_notification_fd();
    for (int i = 0; i < 20000; ++i) {
        if (notification_fd_readable(fd))
            return true;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return notification_fd_readable(fd);
}

void wait_gate_paused(PauseGate& gate) {
    std::atomic<bool>& paused = gate.paused;
    bool seen = paused.load(std::memory_order_acquire);
    while (!seen) {
        paused.wait(seen, std::memory_order_acquire);
        seen = paused.load(std::memory_order_acquire);
    }
}

void resume_gate(PauseGate& gate) noexcept {
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
}

bool wait_counter_reaches(const std::atomic<int>& v, int expected) {
    for (int i = 0; i < 20000000 && v.load(std::memory_order_acquire) < expected; ++i)
        std::this_thread::yield();
    return v.load(std::memory_order_acquire) >= expected;
}

// A pipe-backed read cannot complete until the test writes to the write end,
// so "submitted into the kernel and not yet completed" is a deterministically
// holdable backend-physical state. This is the state whose wake must never be
// lost.
struct BlockedPipeRead {
    int r = -1;
    int w = -1;
    Completion<std::size_t> completion;
    std::vector<std::byte> buffer;

    bool arm(AsyncIoContext& ctx, std::size_t bytes) {
        int fds[2];
        if (::pipe(fds) != 0)
            return false;
        r = fds[0];
        w = fds[1];
        buffer.assign(bytes, std::byte{0});
        return ctx
            .submit_read(ReadOp{NativeFileRef(r, sluice::FileAccess::read_only), buffer.data(),
                                buffer.size(), 0},
                         completion)
            .has_value();
    }

    void release_bytes(std::size_t n) {
        const std::string payload(static_cast<std::size_t>(n), 'p');
        const ssize_t wrote = ::write(w, payload.data(), payload.size());
        (void)wrote;
    }

    void close_pipe() {
        if (r >= 0)
            ::close(r);
        if (w >= 0)
            ::close(w);
        r = -1;
        w = -1;
    }
};

using WaitKind = AsyncIoContext::ProgressWaitOutcome::Kind;

// The parked waits run on a dedicated owner thread; claiming the driving
// authority there keeps the owner fixed for that wait and releases it at
// thread exit so the next driver (the calling thread or a later parked wait)
// may attach sequentially.
struct DriverClaim {
    std::optional<ProgressOwner> owner;
    explicit DriverClaim(AsyncIoContext& ctx) {
        if (auto claimed = ctx.claim_progress_owner(); claimed.has_value())
            owner = std::move(claimed).value();
    }
};

std::optional<AsyncIoContext::ProgressWaitOutcome> wait_one_value(AsyncIoContext& ctx,
                                                                  std::chrono::nanoseconds bound) {
    const auto r = ctx.wait_one(bound);
    if (!r.has_value())
        return std::nullopt;
    return r.value();
}

bool is_progress(const std::optional<AsyncIoContext::ProgressWaitOutcome>& r, std::size_t n) {
    return r.has_value() && r->kind == WaitKind::progress && r->completed == n;
}

bool is_control(const std::optional<AsyncIoContext::ProgressWaitOutcome>& r) {
    return r.has_value() && r->kind == WaitKind::control_interrupted && r->completed == 0;
}

struct TimedWait {
    std::optional<AsyncIoContext::ProgressWaitOutcome> value;
    long long elapsed_ms = 0;
};

TimedWait wait_one_timed(AsyncIoContext& ctx, std::chrono::nanoseconds bound) {
    const auto start = std::chrono::steady_clock::now();
    TimedWait out;
    out.value = wait_one_value(ctx, bound);
    out.elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count();
    return out;
}

// K1/K2/K12 and the U2 park decision, and the mandatory M-C1 kill: a real CQE
// posted before the owner's stale-readiness drain has its eventfd notification
// consumed by that drain. The kernel producer never executes the userspace
// epoch protocol, so only the final physical CQ probe can discover it. Without
// the probe the owner parks until the deadline and the stranded CQE is only
// reaped by the final pass — hence the bounded wait must return well inside
// the deadline. No unrelated future I/O is submitted after the release write.
bool k1_k2_cqe_before_drain_recovered_by_final_probe() {
    auto backend = std::make_unique<UringAsyncBackend>();
    AsyncIoContext ctx(std::move(backend));
    HostInterest interest(ctx);
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    PauseGate gate;
    ctx.set_progress_prerevalidate_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    TimedWait driver;
    std::thread owner([&] { DriverClaim claim{ctx}; driver = wait_one_timed(ctx, std::chrono::milliseconds{2000}); });

    wait_gate_paused(gate);
    ctx.set_progress_prerevalidate_pause_gate_for_test(nullptr);

    read.release_bytes(1);
    if (!wait_notification_readable(ctx))
        return false;
    resume_gate(gate);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = is_progress(driver.value, 1) && prepark.load() == 1 &&
                    driver.elapsed_ms < 1000 && read.completion.ready();
    read.completion.reset();
    read.close_pipe();
    return ok;
}

// K3 (synthetic kernel-style seam): a notification written into the context
// eventfd without any userspace epoch mutation, with the CQ empty, must wake
// the parked owner, must not fabricate a completion, and must leave the owner
// able to repark and service a later real completion.
bool k3_notification_without_epoch_mutation_wakes_and_reparks() {
    auto backend = std::make_unique<UringAsyncBackend>();
    AsyncIoContext ctx(std::move(backend));
    HostInterest interest(ctx);
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread owner([&] { DriverClaim claim{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    const std::uint64_t one = 1;
    const ssize_t n = ::write(ctx.progress_notification_fd(), &one, sizeof(one));
    if (n != static_cast<ssize_t>(sizeof(one)))
        return false;
    if (!notification_fd_readable(ctx.progress_notification_fd()))
        return false;
    resume_gate(gate);
    if (!wait_counter_reaches(prepark, 2))
        return false;
    const bool no_fabrication = !read.completion.ready();

    read.release_bytes(1);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = no_fabrication && is_progress(result, 1) &&
                    prepark.load() == 2 && read.completion.ready();
    read.completion.reset();
    read.close_pipe();
    return ok;
}

// K5/K6: a real CQE whose notification arrives after the owner's final probe
// and final token recheck, while the owner is paused immediately before
// poll(2), persists as eventfd readiness and wakes the park.
bool k5_k6_cqe_after_final_recheck_wakes_poll() {
    auto backend = std::make_unique<UringAsyncBackend>();
    AsyncIoContext ctx(std::move(backend));
    HostInterest interest(ctx);
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread owner([&] { DriverClaim claim{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    read.release_bytes(1);
    if (!wait_notification_readable(ctx))
        return false;
    resume_gate(gate);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = is_progress(result, 1) && prepark.load() == 1 &&
                    read.completion.ready();
    read.completion.reset();
    read.close_pipe();
    return ok;
}

// K7 and the M-C3 kill: the owner is blocked in poll(2) on the context
// notification fd alone when the kernel posts a real CQE. Only the registered
// eventfd wiring can wake it; a periodic timeout must not rescue the case,
// hence the bounded wait must return well inside the deadline.
bool k7_cqe_while_blocked_in_poll_wakes_owner() {
    auto backend = std::make_unique<UringAsyncBackend>();
    AsyncIoContext ctx(std::move(backend));
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    TimedWait driver;
    std::thread owner([&] { DriverClaim claim{ctx}; driver = wait_one_timed(ctx, std::chrono::milliseconds{2000}); });
    if (!wait_counter_reaches(prepark, 1))
        return false;

    read.release_bytes(1);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = is_progress(driver.value, 1) &&
                    driver.elapsed_ms < 1000 && prepark.load() == 1 && read.completion.ready();
    read.completion.reset();
    read.close_pipe();
    return ok;
}

// K8 and §14 one-wake-N: several real CQEs coalesce behind one readable
// eventfd state; a single wake's bounded pass discovers and publishes all of
// them. The settle wait only pins the setup precondition that every CQE is
// already posted before the owner resumes; the oracle is the count itself.
bool k8_multiple_cqes_coalesce_into_one_wake() {
    auto backend = std::make_unique<UringAsyncBackend>();
    AsyncIoContext ctx(std::move(backend));
    HostInterest interest(ctx);
    constexpr int kCoalesced = 4;
    std::vector<BlockedPipeRead> reads(kCoalesced);
    for (auto& r : reads) {
        if (!r.arm(ctx, 4))
            return false;
    }

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread owner([&] { DriverClaim claim{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    for (auto& r : reads)
        r.release_bytes(1);
    if (!wait_notification_readable(ctx))
        return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    resume_gate(gate);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    bool ok = is_progress(result, static_cast<std::size_t>(kCoalesced)) &&
              prepark.load() == 1;
    for (auto& r : reads) {
        ok = ok && r.completion.ready();
        r.completion.reset();
        r.close_pipe();
    }
    return ok;
}

// K9: a spurious kernel-style notification (readable eventfd, empty CQ) wakes
// the owner, fabricates nothing, and the owner reparks safely until the real
// completion arrives.
bool k9_spurious_notification_zero_cqes_is_harmless() {
    auto backend = std::make_unique<UringAsyncBackend>();
    AsyncIoContext ctx(std::move(backend));
    HostInterest interest(ctx);
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread owner([&] { DriverClaim claim{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    if (!wait_counter_reaches(prepark, 1))
        return false;

    const std::uint64_t one = 1;
    const ssize_t n = ::write(ctx.progress_notification_fd(), &one, sizeof(one));
    if (n != static_cast<ssize_t>(sizeof(one)))
        return false;
    if (!wait_counter_reaches(prepark, 2))
        return false;
    const bool no_fabrication = !read.completion.ready();

    read.release_bytes(1);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = no_fabrication && is_progress(result, 1) &&
                    prepark.load() == 2 && read.completion.ready();
    read.completion.reset();
    read.close_pipe();
    return ok;
}

// K10a: a userspace control signal and a kernel CQ notification race on the
// same eventfd while the owner is paused before revalidation. The pass runs
// before the control observation, so the wake reaps the CQE while the raced
// control survives the reap and the next wait observes it.
bool k10a_control_signal_racing_kernel_cq_before_revalidation() {
    auto backend = std::make_unique<UringAsyncBackend>();
    AsyncIoContext ctx(std::move(backend));
    HostInterest interest(ctx);
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    PauseGate gate;
    ctx.set_progress_prerevalidate_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread owner([&] { DriverClaim claim{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prerevalidate_pause_gate_for_test(nullptr);

    ctx.interrupt_progress_waiters();
    read.release_bytes(1);
    if (!wait_notification_readable(ctx))
        return false;
    resume_gate(gate);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    // The coalesced wake reaps the kernel completion (pass before control
    // observation); the sticky control is not erased by that reap and the
    // next wait observes it instead of parking.
    const bool progress_first = is_progress(result, 1) && prepark.load() == 0 &&
                                read.completion.ready();
    read.completion.reset();
    const auto observed = wait_one_value(ctx, std::chrono::milliseconds{8000});
    const bool ok = progress_first && is_control(observed);
    read.completion.reset();
    read.close_pipe();
    return ok;
}

// K10b: the same race while the owner is paused immediately before poll(2);
// the poll returns on the coalesced readiness, the wake re-runs the pass
// first so the CQE is reaped, and the raced control is observed by the next
// wait instead of being erased by the reap.
bool k10b_control_signal_racing_kernel_cq_before_poll() {
    auto backend = std::make_unique<UringAsyncBackend>();
    AsyncIoContext ctx(std::move(backend));
    HostInterest interest(ctx);
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread owner([&] { DriverClaim claim{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    ctx.interrupt_progress_waiters();
    read.release_bytes(1);
    if (!wait_notification_readable(ctx))
        return false;
    resume_gate(gate);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    // The interrupted wake re-runs the pass first, reaping the CQE without
    // fabricating an extra completion; the sticky control survives the reap
    // and the next wait observes it instead of parking.
    const bool progress_first = is_progress(result, 1) && prepark.load() == 1 &&
                                read.completion.ready();
    read.completion.reset();
    const auto observed = wait_one_value(ctx, std::chrono::milliseconds{8000});
    const bool ok = progress_first && is_control(observed);
    read.completion.reset();
    read.close_pipe();
    return ok;
}

// K11a: the context eventfd is already saturated when a real CQE posts. The
// owner's stale drain empties the saturated counter together with the CQE
// notification, and the final physical probe must still recover the CQ
// obligation — well inside the deadline.
bool k11a_saturated_eventfd_with_real_cqe_before_drain() {
    auto backend = std::make_unique<UringAsyncBackend>();
    AsyncIoContext ctx(std::move(backend));
    HostInterest interest(ctx);
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    PauseGate gate;
    ctx.set_progress_prerevalidate_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    TimedWait driver;
    std::thread owner([&] { DriverClaim claim{ctx}; driver = wait_one_timed(ctx, std::chrono::milliseconds{2000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prerevalidate_pause_gate_for_test(nullptr);

    ctx.saturate_progress_notification_for_test();
    read.release_bytes(1);
    if (!wait_notification_readable(ctx))
        return false;
    resume_gate(gate);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = is_progress(driver.value, 1) && prepark.load() == 1 &&
                    driver.elapsed_ms < 1000 && read.completion.ready();
    read.completion.reset();
    read.close_pipe();
    return ok;
}

// K11b: after a saturated-eventfd wake, the owner drains the saturation,
// probes an empty CQ, and parks again; a later real completion must still wake
// it. Saturation never erases the CQ obligation.
bool k11b_drained_saturation_then_park_still_wakes_on_cqe() {
    auto backend = std::make_unique<UringAsyncBackend>();
    AsyncIoContext ctx(std::move(backend));
    HostInterest interest(ctx);
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread owner([&] { DriverClaim claim{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    ctx.saturate_progress_notification_for_test();
    resume_gate(gate);
    if (!wait_counter_reaches(prepark, 2))
        return false;
    const bool drained_before_release = !notification_fd_readable(ctx.progress_notification_fd());

    read.release_bytes(1);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = drained_before_release && is_progress(result, 1) &&
                    prepark.load() == 2 && read.completion.ready();
    read.completion.reset();
    read.close_pipe();
    return ok;
}

// U1: a request genuinely in flight in the kernel (pipe-blocked, CQ empty, no
// publication pending) reports accepted=true / immediate=false and the owner
// is allowed to prepare to park; the kernel completion wakes it.
bool u1_accepted_true_immediate_false_reports_and_parks() {
    auto backend = std::make_unique<UringAsyncBackend>();
    AsyncIoContext ctx(std::move(backend));
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    {
        auto claim = ctx.claim_progress_owner();
        const auto pass = ctx.poll_progress();
        if (!pass.has_value() || pass.value().completed != 0 ||
            pass.value().immediate_work_remains || !pass.value().accepted_work_remains)
            return false;
    }

    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);
    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread owner([&] { DriverClaim claim{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    if (!wait_counter_reaches(prepark, 1))
        return false;

    read.release_bytes(1);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = is_progress(result, 1) && prepark.load() == 1 &&
                    read.completion.ready();
    read.completion.reset();
    read.close_pipe();
    return ok;
}

// K13: concurrent CQEs beyond the CQ ring capacity are held in the kernel
// overflow list. The parked owner must still wake, and bounded passes must
// retire every completion without any unrelated future I/O.
bool k13_cq_overflow_completions_are_not_stranded() {
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{8, 1});
    AsyncIoContext ctx(std::move(backend));
    HostInterest interest(ctx);
    constexpr int kOverflowed = 3;
    std::vector<BlockedPipeRead> reads(kOverflowed);
    for (auto& r : reads) {
        if (!r.arm(ctx, 4))
            return false;
    }

    PauseGate gate;
    ctx.set_progress_prerevalidate_pause_gate_for_test(&gate);
    std::optional<AsyncIoContext::ProgressWaitOutcome> owner_result;
    std::thread owner([&] { DriverClaim claim{ctx}; owner_result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prerevalidate_pause_gate_for_test(nullptr);

    for (auto& r : reads)
        r.release_bytes(1);
    if (!wait_notification_readable(ctx))
        return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    resume_gate(gate);
    owner.join();

    long long delivered = 0;
    if (owner_result.has_value() && owner_result->kind == WaitKind::progress)
        delivered = static_cast<long long>(owner_result->completed);
    for (int i = 0; i < 64; ++i) {
        const auto pass = ctx.poll_progress();
        delivered += static_cast<long long>(pass.value().completed);
        if (pass.value().completed == 0 && !pass.value().immediate_work_remains)
            break;
    }

    bool all_ready = true;
    for (auto& r : reads) {
        all_ready = all_ready && r.completion.ready();
        r.completion.reset();
        r.close_pipe();
    }
    return all_ready && delivered == kOverflowed;
}

namespace {
struct PoisonSubmitState {
    std::atomic<bool> fail_next{false};
};

int poison_submit_hook(void* context, ::io_uring* ring) noexcept {
    auto* state = static_cast<PoisonSubmitState*>(context);
    if (state->fail_next.exchange(false, std::memory_order_acq_rel))
        return -EINVAL;
    return ::io_uring_submit(ring);
}
}

// H-interval (multi-driver): a peer pass can reap the CQE before the paused
// owner's physical probe, consuming both the kernel notification and the CQ
// state the probe reads. The peer's publication_pending_ transition signal is
// the only remaining wake; the owner must return through it, never by
// stranding to the deadline.
bool k14_peer_drive_while_owner_parked_is_rejected_and_owner_recovers_cq() {
    auto backend = std::make_unique<UringAsyncBackend>();
    AsyncIoContext ctx(std::move(backend));
    HostInterest interest(ctx);
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    PauseGate gate;
    ctx.set_progress_prerevalidate_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    TimedWait driver;
    std::thread owner([&] { DriverClaim claim{ctx}; driver = wait_one_timed(ctx, std::chrono::milliseconds{2000}); });

    wait_gate_paused(gate);
    ctx.set_progress_prerevalidate_pause_gate_for_test(nullptr);

    read.release_bytes(1);
    if (!wait_notification_readable(ctx))
        return false;

    // A peer drive while the owner holds the domain is rejected, not
    // serialized; the kernel completion is recovered by the owner's own
    // pass after it resumes.
    const auto peer_pass = ctx.poll_progress();
    const bool peer_rejected = !peer_pass.has_value() &&
                               peer_pass.error().code == sluice::IoError::Code::invalid_state;

    resume_gate(gate);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = peer_rejected && is_progress(driver.value, 1) &&
                    driver.elapsed_ms < 1000 && read.completion.ready();
    read.completion.reset();
    read.close_pipe();
    return ok;
}

// §17 poison-vs-park: a backend poison transition created by another thread's
// submit must wake a parked owner. The two pipe reads are already in the
// kernel; the third acceptance's dispatch-time transport fails fatally on the
// submitting thread, the poison path retires the kernel-invisible request,
// and the source signal must reach the parked owner. M-C5 removes the poison
// signal and the wake arrives only at the deadline, hence the elapsed bound.
bool poison_transition_wakes_parked_owner() {
    PoisonSubmitState state;
    UringBackendSubmitTestHooks hooks;
    hooks.context = &state;
    hooks.submit = &poison_submit_hook;
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{8, 2}, hooks);
    AsyncIoContext ctx(std::move(backend));

    std::vector<BlockedPipeRead> blocked(2);
    for (auto& r : blocked) {
        if (!r.arm(ctx, 4))
            return false;
    }

    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    TimedWait driver;
    std::thread owner([&] { DriverClaim claim{ctx}; driver = wait_one_timed(ctx, std::chrono::milliseconds{2000}); });
    if (!wait_counter_reaches(prepark, 1))
        return false;

    Completion<std::size_t> poisoned;
    std::vector<std::byte> sink(4, std::byte{0});
    state.fail_next.store(true, std::memory_order_release);
    const bool poisoned_accepted =
        ctx.submit_read(ReadOp{NativeFileRef(blocked[0].r, sluice::FileAccess::read_only),
                               sink.data(), sink.size(), 0},
                        poisoned)
            .has_value();

    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool poisoned_failed_with_backend_error =
        poisoned.ready() && !poisoned.result().has_value() &&
        poisoned.result().error().code == IoError::Code::backend_error;
    poisoned.reset();

    const bool ok = poisoned_accepted && poisoned_failed_with_backend_error &&
                    is_progress(driver.value, 1) &&
                    driver.elapsed_ms < 1000;

    // Retire the in-kernel reads so the context can be destroyed: the reap
    // path stays live under poison.
    for (auto& r : blocked)
        r.release_bytes(1);
    bool all_retired = true;
    for (auto& r : blocked) {
        for (int i = 0; i < 2000 && !r.completion.ready(); ++i) {
            (void)ctx.poll_progress();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        all_retired = all_retired && r.completion.ready();
        r.completion.reset();
        r.close_pipe();
    }
    return ok && all_retired;
}

// U2/K15: an accepted request whose transport failed retryably (-EAGAIN)
// produces no CQE and no kernel eventfd write; the owner must not park past
// the unadvertised dispatch obligation. The bounded retry nap must resubmit
// until the hook disarms, after which the request completes normally. M-C7
// restores the retryable-return-with-no-advertisement behavior: the owner
// parks at the full handshake, never retries, and the wait strands to the
// deadline with value 0.
struct EagainSubmitState {
    std::atomic<bool> fail{false};
    std::atomic<int> calls{0};
};

int eagain_submit_hook(void* context, ::io_uring* ring) noexcept {
    auto* state = static_cast<EagainSubmitState*>(context);
    state->calls.fetch_add(1, std::memory_order_relaxed);
    if (state->fail.load(std::memory_order_acquire))
        return -EAGAIN;
    return ::io_uring_submit(ring);
}

bool u2_k15_retryable_submit_failure_does_not_strand_owner() {
    EagainSubmitState state;
    UringBackendSubmitTestHooks hooks;
    hooks.context = &state;
    hooks.submit = &eagain_submit_hook;
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{}, hooks);
    AsyncIoContext ctx(std::move(backend));

    // Arm the retryable failure before the acceptance: the dispatch-time
    // submit is the first transport attempt.
    state.fail.store(true, std::memory_order_release);
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    std::thread fail_timer([&state] {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        state.fail.store(false, std::memory_order_release);
    });

    TimedWait driver;
    std::thread owner([&] { DriverClaim claim{ctx}; driver = wait_one_timed(ctx, std::chrono::milliseconds{2000}); });

    fail_timer.join();
    read.release_bytes(1);
    owner.join();

    const bool ok = state.calls.load(std::memory_order_relaxed) >= 2 &&
                    is_progress(driver.value, 1) &&
                    driver.elapsed_ms < 1000 && read.completion.ready();
    read.completion.reset();
    read.close_pipe();
    return ok;
}

// U2b: a request accepted by a peer while the owner is parked must reach the
// kernel from the accepting thread's dispatch — the owner must never be the
// only path that submits accepted work, or the request would wait for a
// completion of an operation no pass dispatched (PROG-01). While the owner is
// frozen at the park gate, the SQ must be empty after the peer's acceptance,
// and both requests must complete once released. Against the pre-corrective
// behavior the prepared SQE stayed in the ring (sq_ready != 0) and only an
// unrelated completion would have driven it.
bool u2b_accepted_dispatch_wakes_parked_owner() {
    auto raw_backend = std::make_unique<UringAsyncBackend>();
    auto* backend_ptr = raw_backend.get();
    AsyncIoContext ctx(std::move(raw_backend));
    BlockedPipeRead first;
    if (!first.arm(ctx, 4))
        return false;
    {
        auto claim = ctx.claim_progress_owner();
        if (ctx.poll_progress().value_or(AsyncBackend::ProgressPass{}).completed != 0)
            return false;
    }

    PauseGate gate;
    ctx.set_progress_prerevalidate_pause_gate_for_test(&gate);
    TimedWait driver;
    std::thread owner([&] { DriverClaim claim{ctx}; driver = wait_one_timed(ctx, std::chrono::milliseconds{2000}); });
    wait_gate_paused(gate);

    BlockedPipeRead second;
    if (!second.arm(ctx, 4))
        return false;
    const bool peer_acceptance_entered_kernel = backend_ptr->sq_ready_for_test() == 0;

    ctx.set_progress_prerevalidate_pause_gate_for_test(nullptr);
    first.release_bytes(1);
    second.release_bytes(1);
    resume_gate(gate);
    owner.join();

    bool all_ready = first.completion.ready() && second.completion.ready();
    for (int i = 0; i < 2000 && !all_ready; ++i) {
        (void)ctx.poll_progress();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        all_ready = first.completion.ready() && second.completion.ready();
    }
    const bool ok = peer_acceptance_entered_kernel &&
                    driver.value.has_value() &&
                    driver.value->kind == WaitKind::progress && driver.value->completed >= 1 &&
                    driver.elapsed_ms < 1000 && all_ready;
    first.completion.reset();
    first.close_pipe();
    second.completion.reset();
    second.close_pipe();
    return ok;
}

// M-C8 oracle: a failing CQ-overflow flush must become an observable health
// event, not a silently discarded no-op. The ring-visible CQE still completes
// the first wait; the poison then makes every later pass report health so
// waits behind the unflushable overflow return the health verdict instead of
// parking or spinning, new submissions must observe the backend health
// failure, and the reap path must stay live so the real flush retires every
// read once the failure clears.
struct FlushFailureState {
    std::atomic<bool> fail{false};
    std::atomic<int> calls{0};
};

int flush_failure_hook(void* context, ::io_uring* ring) noexcept {
    auto* state = static_cast<FlushFailureState*>(context);
    state->calls.fetch_add(1, std::memory_order_relaxed);
    if (state->fail.load(std::memory_order_acquire))
        return -EIO;
    return ::io_uring_get_events(ring);
}

bool overflow_flush_failure_becomes_observable_health_event() {
    FlushFailureState state;
    UringBackendSubmitTestHooks hooks;
    hooks.context = &state;
    hooks.get_events = &flush_failure_hook;
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{8, 1}, hooks);
    AsyncIoContext ctx(std::move(backend));

    std::vector<BlockedPipeRead> reads(3);
    for (auto& r : reads) {
        if (!r.arm(ctx, 4))
            return false;
    }
    {
        auto claim = ctx.claim_progress_owner();
        if (ctx.poll_progress().value_or(AsyncBackend::ProgressPass{}).completed != 0)
            return false;
    }

    // Post every completion before any pass can reap: the CQ ring (twice the
    // one-entry SQ) cannot hold all three, so the owner's first pass observes
    // cq_has_overflow and must flush through the failing hook.
    for (auto& r : reads)
        r.release_bytes(1);
    state.fail.store(true, std::memory_order_release);

    TimedWait driver;
    std::thread owner([&] { DriverClaim claim{ctx}; driver = wait_one_timed(ctx, std::chrono::milliseconds{2000}); });
    owner.join();

    // The injected flush failure must be observed, must become a persistent
    // backend health fact (new submissions are rejected), and must strand
    // nothing: the pass still converges well inside the deadline and every
    // in-flight read retires once the failure clears. The mutant that
    // restores the discarded-return behavior records no flush attempt and no
    // poison, failing both the attempt and health assertions.
    const bool first_wait_ok = driver.value.has_value() &&
                               driver.value->kind == WaitKind::progress &&
                               driver.value->completed >= 2 && driver.elapsed_ms < 1000 &&
                               state.calls.load(std::memory_order_relaxed) >= 1;

    // The poisoned backend must reject new submissions outright. The probe
    // reads a pre-written pipe byte, so the mutant that accepts it can retire
    // the probe completion through the ordinary drain instead of aborting.
    std::vector<std::byte> sink(4, std::byte{0});
    int probe_fds[2];
    if (::pipe(probe_fds) != 0)
        return false;
    const std::string one_byte(1, 'p');
    const ssize_t probe_wrote = ::write(probe_fds[1], one_byte.data(), 1);
    (void)probe_wrote;
    Completion<std::size_t> probe;
    const bool health_visible =
        !ctx.submit_read(
                 ReadOp{NativeFileRef(probe_fds[0], sluice::FileAccess::read_only), sink.data(),
                        sink.size(), 0},
                 probe)
             .has_value();
    for (int i = 0; !health_visible && i < 20000 && !probe.ready(); ++i) {
        (void)ctx.poll_progress();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    probe.reset();
    ::close(probe_fds[0]);
    ::close(probe_fds[1]);

    state.fail.store(false, std::memory_order_release);
    bool all_retired = true;
    for (auto& r : reads) {
        for (int i = 0; i < 20000 && !r.completion.ready(); ++i) {
            (void)ctx.poll_progress();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        all_retired = all_retired && r.completion.ready();
        r.completion.reset();
        r.close_pipe();
    }
    return first_wait_ok && health_visible && all_retired;
}

// F1 oracle: one pass publishes at most the publication work pending at its
// entry. Work a concurrent producer posts while the pass is mid-publication
// (here a won-before-execution cancel of an undispatched entry, held
// undispatched by a retryable submit failure saturating the SQ) stays for the
// next pass and is reported as remaining immediate work.
bool f1_publication_pass_is_entry_bounded() {
    EagainSubmitState state;
    state.fail.store(true, std::memory_order_release);
    UringBackendSubmitTestHooks hooks;
    hooks.context = &state;
    hooks.submit = &eagain_submit_hook;
    auto raw_backend = std::make_unique<UringAsyncBackend>(UringConfig{4, 1}, hooks);
    auto* backend_ptr = raw_backend.get();
    AsyncIoContext ctx(std::move(raw_backend));
    HostInterest interest(ctx);

    BlockedPipeRead a;
    BlockedPipeRead b;
    BlockedPipeRead c;
    if (!a.arm(ctx, 4) || !b.arm(ctx, 4) || !c.arm(ctx, 4))
        return false;

    {
        auto claim = ctx.claim_progress_owner();
        if (ctx.poll_progress().value_or(AsyncBackend::ProgressPass{}).completed != 0)
            return false;
    }
    ctx.cancel(c.completion);

    UringAsyncBackend::PublicationEpiloguePauseGate epilogue;
    backend_ptr->set_publication_epilogue_pause_gate(&epilogue);

    std::optional<AsyncBackend::ProgressPass> first;
    std::thread driver([&] {
        DriverClaim claim{ctx};
        const auto r = ctx.poll_progress();
        if (r.has_value())
            first = r.value();
    });
    while (!epilogue.paused.load(std::memory_order_acquire))
        std::this_thread::yield();

    const bool c_published_at_pause = c.completion.ready();
    ctx.cancel(b.completion);

    epilogue.resume.store(true, std::memory_order_release);
    epilogue.resume.notify_all();
    driver.join();
    backend_ptr->set_publication_epilogue_pause_gate(nullptr);

    const bool first_ok = c_published_at_pause && first.has_value() &&
                          first.value().completed == 1 &&
                          first->immediate_work_remains && !first->health_failed;
    if (!first_ok)
        return false;
    c.completion.reset();
    c.close_pipe();

    const auto second = ctx.poll_progress();
    const bool second_ok = second.has_value() && second.value().completed == 1 &&
                           b.completion.ready() && !second.value().immediate_work_remains;

    state.fail.store(false, std::memory_order_release);
    a.release_bytes(1);
    bool a_retired = false;
    for (int i = 0; i < 20000 && !a_retired; ++i) {
        const auto pass = ctx.poll_progress();
        a_retired = pass.has_value() && a.completion.ready();
        if (!a_retired)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    b.completion.reset();
    b.close_pipe();
    a.completion.reset();
    a.close_pipe();
    return second_ok && a_retired;
}

// F2 oracle: a backend transport poison is a sticky progress-machinery health
// fact. The poisoned backend's retirement pass still reports the completions
// it publishes (progress outranks health), the pass report exposes the health
// verdict, and the next wait returns health_failure promptly instead of
// parking behind work that will never complete.
bool f2_backend_poison_reaches_owner_health_verdict() {
    PoisonSubmitState state;
    state.fail_next.store(true, std::memory_order_release);
    UringBackendSubmitTestHooks hooks;
    hooks.context = &state;
    hooks.submit = &poison_submit_hook;
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{8, 4}, hooks);
    AsyncIoContext ctx(std::move(backend));
    HostInterest interest(ctx);

    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    TimedWait first;
    std::thread owner([&] { DriverClaim claim{ctx}; first = wait_one_timed(ctx, std::chrono::milliseconds{2000}); });
    owner.join();

    bool pass_ok = false;
    std::thread passer([&] {
        DriverClaim claim{ctx};
        const auto pass = ctx.poll_progress();
        pass_ok = pass.has_value() && pass.value().health_failed &&
                  !pass.value().accepted_work_remains && !pass.value().immediate_work_remains;
    });
    passer.join();

    TimedWait second;
    std::thread owner2([&] { DriverClaim claim{ctx}; second = wait_one_timed(ctx, std::chrono::milliseconds{2000}); });
    owner2.join();

    const bool ok = is_progress(first.value, 1) && first.elapsed_ms < 1000 &&
                    read.completion.ready() && pass_ok &&
                    second.value.has_value() &&
                    second.value->kind == WaitKind::health_failure && second.elapsed_ms < 1000;
    read.completion.reset();
    read.close_pipe();
    return ok;
}
}

int main() {
    ::alarm(120);
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"k1_k2_cqe_before_drain_recovered_by_final_probe",
         k1_k2_cqe_before_drain_recovered_by_final_probe},
        {"k3_notification_without_epoch_mutation_wakes_and_reparks",
         k3_notification_without_epoch_mutation_wakes_and_reparks},
        {"k5_k6_cqe_after_final_recheck_wakes_poll", k5_k6_cqe_after_final_recheck_wakes_poll},
        {"k7_cqe_while_blocked_in_poll_wakes_owner", k7_cqe_while_blocked_in_poll_wakes_owner},
        {"k8_multiple_cqes_coalesce_into_one_wake", k8_multiple_cqes_coalesce_into_one_wake},
        {"k9_spurious_notification_zero_cqes_is_harmless",
         k9_spurious_notification_zero_cqes_is_harmless},
        {"k10a_control_signal_racing_kernel_cq_before_revalidation",
         k10a_control_signal_racing_kernel_cq_before_revalidation},
        {"k10b_control_signal_racing_kernel_cq_before_poll",
         k10b_control_signal_racing_kernel_cq_before_poll},
        {"k11a_saturated_eventfd_with_real_cqe_before_drain",
         k11a_saturated_eventfd_with_real_cqe_before_drain},
        {"k11b_drained_saturation_then_park_still_wakes_on_cqe",
         k11b_drained_saturation_then_park_still_wakes_on_cqe},
        {"u1_accepted_true_immediate_false_reports_and_parks",
         u1_accepted_true_immediate_false_reports_and_parks},
        {"k13_cq_overflow_completions_are_not_stranded",
         k13_cq_overflow_completions_are_not_stranded},
        {"k14_peer_drive_while_owner_parked_is_rejected_and_owner_recovers_cq",
         k14_peer_drive_while_owner_parked_is_rejected_and_owner_recovers_cq},
        {"poison_transition_wakes_parked_owner", poison_transition_wakes_parked_owner},
        {"u2_k15_retryable_submit_failure_does_not_strand_owner",
         u2_k15_retryable_submit_failure_does_not_strand_owner},
        {"u2b_accepted_dispatch_wakes_parked_owner", u2b_accepted_dispatch_wakes_parked_owner},
        {"overflow_flush_failure_becomes_observable_health_event",
         overflow_flush_failure_becomes_observable_health_event},
        {"f1_publication_pass_is_entry_bounded", f1_publication_pass_is_entry_bounded},
        {"f2_backend_poison_reaches_owner_health_verdict",
         f2_backend_poison_reaches_owner_health_verdict},
    };

    std::size_t passed = 0;
    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
        ++passed;
        std::printf("ok %s\n", t.name);
        std::fflush(stdout);
    }
    std::printf("all %zu uring progress race tests passed\n", passed);
    return 0;
}
