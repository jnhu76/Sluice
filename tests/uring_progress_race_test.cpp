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

#include <csignal>

namespace {

using namespace sluice::async;
using sluice::IoError;

using PauseGate = detail::ProgressSource::PauseGate;

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

bool wait_notification_readable(const AsyncIoContext& ctx) {
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

std::optional<std::size_t> wait_one_value(AsyncIoContext& ctx, std::chrono::nanoseconds bound) {
    const auto r = ctx.wait_one(bound);
    if (!r.has_value())
        return std::nullopt;
    return r.value();
}

struct TimedWait {
    std::optional<std::size_t> value;
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
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    PauseGate gate;
    ctx.set_progress_prerevalidate_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    TimedWait driver;
    std::thread owner([&] { driver = wait_one_timed(ctx, std::chrono::milliseconds{2000}); });

    wait_gate_paused(gate);
    ctx.set_progress_prerevalidate_pause_gate_for_test(nullptr);

    read.release_bytes(1);
    if (!wait_notification_readable(ctx))
        return false;
    resume_gate(gate);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = driver.value.has_value() && driver.value.value() == 1 && prepark.load() == 1 &&
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
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<std::size_t> result;
    std::thread owner([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
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

    const bool ok = no_fabrication && result.has_value() && result.value() == 1 &&
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
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<std::size_t> result;
    std::thread owner([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    read.release_bytes(1);
    if (!wait_notification_readable(ctx))
        return false;
    resume_gate(gate);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = result.has_value() && result.value() == 1 && prepark.load() == 1 &&
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
    std::thread owner([&] { driver = wait_one_timed(ctx, std::chrono::milliseconds{2000}); });
    if (!wait_counter_reaches(prepark, 1))
        return false;

    read.release_bytes(1);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = driver.value.has_value() && driver.value.value() == 1 &&
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

    std::optional<std::size_t> result;
    std::thread owner([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
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

    bool ok = result.has_value() && result.value() == static_cast<std::size_t>(kCoalesced) &&
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
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<std::size_t> result;
    std::thread owner([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
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

    const bool ok = no_fabrication && result.has_value() && result.value() == 1 &&
                    prepark.load() == 2 && read.completion.ready();
    read.completion.reset();
    read.close_pipe();
    return ok;
}

// K10a: a userspace control signal and a kernel CQ notification race on the
// same eventfd while the owner is paused before revalidation; the control
// revalidation wins the wake and the following pass still services the CQE.
bool k10a_control_signal_racing_kernel_cq_before_revalidation() {
    auto backend = std::make_unique<UringAsyncBackend>();
    AsyncIoContext ctx(std::move(backend));
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    PauseGate gate;
    ctx.set_progress_prerevalidate_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<std::size_t> result;
    std::thread owner([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prerevalidate_pause_gate_for_test(nullptr);

    ctx.interrupt_progress_waiters();
    read.release_bytes(1);
    if (!wait_notification_readable(ctx))
        return false;
    resume_gate(gate);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = result.has_value() && result.value() == 1 && prepark.load() == 0 &&
                    read.completion.ready();
    read.completion.reset();
    read.close_pipe();
    return ok;
}

// K10b: the same race while the owner is paused immediately before poll(2);
// the poll returns on the coalesced readiness, the control recheck selects the
// interrupted outcome, and the final pass still reaps the CQE without
// fabricating an extra completion.
bool k10b_control_signal_racing_kernel_cq_before_poll() {
    auto backend = std::make_unique<UringAsyncBackend>();
    AsyncIoContext ctx(std::move(backend));
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<std::size_t> result;
    std::thread owner([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    ctx.interrupt_progress_waiters();
    read.release_bytes(1);
    if (!wait_notification_readable(ctx))
        return false;
    resume_gate(gate);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = result.has_value() && result.value() == 1 && prepark.load() == 1 &&
                    read.completion.ready();
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
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    PauseGate gate;
    ctx.set_progress_prerevalidate_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    TimedWait driver;
    std::thread owner([&] { driver = wait_one_timed(ctx, std::chrono::milliseconds{2000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prerevalidate_pause_gate_for_test(nullptr);

    ctx.saturate_progress_notification_for_test();
    read.release_bytes(1);
    if (!wait_notification_readable(ctx))
        return false;
    resume_gate(gate);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = driver.value.has_value() && driver.value.value() == 1 && prepark.load() == 1 &&
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
    BlockedPipeRead read;
    if (!read.arm(ctx, 4))
        return false;

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<std::size_t> result;
    std::thread owner([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
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

    const bool ok = drained_before_release && result.has_value() && result.value() == 1 &&
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

    const auto pass = ctx.poll_progress();
    if (pass.completed != 0 || pass.immediate_work_remains || !pass.accepted_work_remains)
        return false;

    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);
    std::optional<std::size_t> result;
    std::thread owner([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    if (!wait_counter_reaches(prepark, 1))
        return false;

    read.release_bytes(1);
    owner.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = result.has_value() && result.value() == 1 && prepark.load() == 1 &&
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
    constexpr int kOverflowed = 3;
    std::vector<BlockedPipeRead> reads(kOverflowed);
    for (auto& r : reads) {
        if (!r.arm(ctx, 4))
            return false;
    }

    PauseGate gate;
    ctx.set_progress_prerevalidate_pause_gate_for_test(&gate);
    std::optional<std::size_t> owner_result;
    std::thread owner([&] { owner_result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prerevalidate_pause_gate_for_test(nullptr);

    for (auto& r : reads)
        r.release_bytes(1);
    if (!wait_notification_readable(ctx))
        return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    resume_gate(gate);
    owner.join();

    long long delivered = static_cast<long long>(owner_result.value_or(0));
    for (int i = 0; i < 64; ++i) {
        const auto pass = ctx.poll_progress();
        delivered += static_cast<long long>(pass.completed);
        if (pass.completed == 0 && !pass.immediate_work_remains)
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

// §17 poison-vs-park: a backend poison transition created by another thread's
// submit must wake a parked owner. With the two-entry SQ ring filled by two
// accepted-but-undispatched submissions, the third submit hits the transport
// from the submitting thread: the fatal result poisons the backend, the
// poison path retires every kernel-invisible operation, and the source signal
// must reach the parked owner. M-C5 removes the poison signal and the wake
// arrives only at the deadline, hence the elapsed bound.
bool poison_transition_wakes_parked_owner() {
    PoisonSubmitState state;
    UringBackendSubmitTestHooks hooks;
    hooks.context = &state;
    hooks.submit = &poison_submit_hook;
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{8, 2}, hooks);
    AsyncIoContext ctx(std::move(backend));

    // Two pipe reads fill the SQ ring; the owner's first pass submits both
    // into the kernel, where they block until the test writes the pipes.
    std::vector<BlockedPipeRead> blocked(2);
    for (auto& r : blocked) {
        if (!r.arm(ctx, 4))
            return false;
    }

    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    TimedWait driver;
    std::thread owner([&] { driver = wait_one_timed(ctx, std::chrono::milliseconds{2000}); });
    if (!wait_counter_reaches(prepark, 1))
        return false;

    // Two more submissions prepped from this thread exhaust the ring; the
    // fifth submit must submit-and-fail at the transport from here.
    std::vector<BlockedPipeRead> extra(2);
    for (auto& r : extra) {
        if (!r.arm(ctx, 4))
            return false;
    }
    Completion<std::size_t> poisoned;
    std::vector<std::byte> sink(4, std::byte{0});
    state.fail_next.store(true, std::memory_order_release);
    const bool poisoned_accepted =
        ctx.submit_read(ReadOp{NativeFileRef(::fileno(tmpfile()), sluice::FileAccess::read_only),
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
                    driver.value.has_value() && driver.value.value() == 3 &&
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
        {"poison_transition_wakes_parked_owner", poison_transition_wakes_parked_owner},
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
