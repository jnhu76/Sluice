#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include <csignal>

namespace {

using namespace sluice::async;
using sluice::IoError;

using PauseGate = detail::ProgressSource::PauseGate;

int temp_file_fd(const std::string& content) {
    char path[] = "/tmp/sluice_tp_race_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return -1;
    ::unlink(path);
    if (!content.empty()) {
        const ssize_t n = ::write(fd, content.data(), content.size());
        if (n != static_cast<ssize_t>(content.size())) {
            ::close(fd);
            return -1;
        }
    }
    ::lseek(fd, 0, SEEK_SET);
    return fd;
}

// Borrows the context notification fd once for readiness probes and retires
// the external interest on scope exit; these tests exercise the park
// handshake, not host-registration lifetime. The fd is captured here, before
// any drive starts, because a borrow during an active drive is refused.
struct HostInterest {
    AsyncIoContext& ctx;
    int fd;
    explicit HostInterest(AsyncIoContext& c) noexcept
        : ctx(c), fd(c.progress_notification_fd()) {}
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

// Readiness produced by a resumed worker lands asynchronously; a caller that
// must observe it while holding a pause gate waits bounded instead of racing
// one poll.
bool wait_notification_readable(const HostInterest& interest) {
    for (int i = 0; i < 20000; ++i) {
        if (notification_fd_readable(interest.fd))
            return true;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return notification_fd_readable(interest.fd);
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

// Captures the backend pointer before it moves into the context.
AsyncIoContext make_pool_context(std::size_t capacity, std::size_t workers,
                                 ThreadPoolBackend** raw_out) {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{capacity, workers});
    *raw_out = backend.get();
    return AsyncIoContext(std::move(backend));
}

// A dispatched read whose worker pauses before offering its terminal outcome:
// the request stays accepted-and-unpublished, so a driver has actionable
// outstanding work but nothing to consume yet.
struct GatedWorkerRead {
    ThreadPoolBackend* backend = nullptr;
    ThreadPoolBackend::WorkerOutcomePreTerminalPauseGate gate;
    Completion<std::size_t> completion;
    int fd = -1;

    bool arm(AsyncIoContext& ctx, ThreadPoolBackend* backend_ptr) {
        backend = backend_ptr;
        fd = temp_file_fd(std::string(64, 'x'));
        if (fd < 0)
            return false;
        backend->set_worker_outcome_pre_terminal_pause_gate(&gate);
        buffer_.resize(32, std::byte{0});
        if (!ctx
                 .submit_read(ReadOp{NativeFileRef(fd, sluice::FileAccess::read_only),
                                     buffer_.data(), buffer_.size(), 0},
                              completion)
                 .has_value())
            return false;
        wait_threadpool_gate_paused(gate);
        return true;
    }

    void release() {
        backend->set_worker_outcome_pre_terminal_pause_gate(nullptr);
        resume_threadpool_gate(gate);
        wait_threadpool_gate_exited(gate);
    }

    std::vector<std::byte> buffer_;
};

bool submit_zero_op(AsyncIoContext& ctx, Completion<std::size_t>& c) {
    std::vector<std::byte> buffer(4, std::byte{0});
    return ctx
        .submit_read(ReadOp{NativeFileRef(::fileno(tmpfile()), sluice::FileAccess::read_only),
                            buffer.data(), 0, 0},
                     c)
        .has_value();
}

using WaitKind = AsyncIoContext::ProgressWaitOutcome::Kind;

// The parked waits run on a dedicated driver thread; claiming the driving
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

// Drives the driver thread result into an optional outcome: nullopt means the
// bounded wait was rejected instead of reporting an owner outcome.
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

// R1: a signal preceding the wait is discoverable by the first bounded pass;
// the owner never needs to park.
bool r1_signal_before_wait_is_serviced_without_parking() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncIoContext ctx(std::move(backend));
    HostInterest interest(ctx);

    Completion<std::size_t> c;
    if (!submit_zero_op(ctx, c))
        return false;
    if (!notification_fd_readable(interest.fd))
        return false;

    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);
    const auto r = wait_one_value(ctx, std::chrono::milliseconds{5000});
    ctx.set_progress_prepark_counter_for_test(nullptr);

    return is_progress(r, 1) && prepark.load() == 0 && c.ready();
}

// R2 (also the R6/R7 window): a signal landing between the recorded token and
// the fused stale-acknowledgement point is caught by the epoch revalidation;
// the park returns without draining or entering poll(2).
bool r2_signal_between_token_and_ack_is_revalidation_caught() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);
    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    PauseGate gate;
    ctx.set_progress_prerevalidate_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread driver([&] { DriverClaim owner{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });

    wait_gate_paused(gate);
    ctx.set_progress_prerevalidate_pause_gate_for_test(nullptr);

    Completion<std::size_t> zero;
    if (!submit_zero_op(ctx, zero))
        return false;
    resume_gate(gate);
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = is_progress(result, 1) && prepark.load() == 0 && zero.ready();

    read.release();
    const auto final_r = wait_one_value(ctx, std::chrono::milliseconds{8000});
    zero.reset();
    return ok && is_progress(final_r, 1) && read.completion.ready() &&
           (read.completion.reset(), true);
}

// R3 (the "empty pass, signal after the final check" shape): a signal landing
// after the stale acknowledgement persists as kernel readiness; the parked
// owner wakes and the next token snapshot forwards the loop into a servicing
// pass.
bool r3_signal_after_ack_wakes_poll_before_next_snapshot() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);
    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread driver([&] { DriverClaim owner{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });

    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    Completion<std::size_t> zero;
    if (!submit_zero_op(ctx, zero))
        return false;
    resume_gate(gate);
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = is_progress(result, 1) && prepark.load() == 1 && zero.ready();

    read.release();
    const auto final_r = wait_one_value(ctx, std::chrono::milliseconds{8000});
    zero.reset();
    return ok && is_progress(final_r, 1) && read.completion.ready() &&
           (read.completion.reset(), true);
}

// R4/R5: a signal raised by a worker completion while the owner sits between
// the empty pass and the park decision cannot strand; the revalidation catches
// the epoch change and the re-run pass publishes the read.
bool r4_worker_completion_signal_before_final_recheck_is_caught() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);
    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    PauseGate gate;
    ctx.set_progress_prerevalidate_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread driver([&] { DriverClaim owner{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prerevalidate_pause_gate_for_test(nullptr);

    read.release();
    resume_gate(gate);
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = is_progress(result, 1) && prepark.load() == 0 && read.completion.ready();
    read.completion.reset();
    return ok;
}

// R5: readiness raised after the pass boundary is observable while the owner
// is paused and the wake is delivered on resume.
bool r5_postdrain_signal_readiness_persists_into_poll() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);
    HostInterest interest(ctx);
    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread driver([&] { DriverClaim owner{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    Completion<std::size_t> zero;
    if (!submit_zero_op(ctx, zero))
        return false;
    const bool readable_while_paused = notification_fd_readable(interest.fd);
    resume_gate(gate);
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = readable_while_paused && is_progress(result, 1) && prepark.load() == 1 &&
                    zero.ready();

    read.release();
    const auto final_r = wait_one_value(ctx, std::chrono::milliseconds{8000});
    zero.reset();
    return ok && is_progress(final_r, 1) && read.completion.ready() &&
           (read.completion.reset(), true);
}

// R8 / the especially dangerous empty-poll shape: the pass is empty, the stale
// acknowledgement is already done, and the signal fires at the paused owner.
// The readiness transition is observable while parked; poll(2) itself returns.
bool r8_empty_pass_signal_immediately_before_poll_entry() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);
    HostInterest interest(ctx);
    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread driver([&] { DriverClaim owner{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    const int nfd = interest.fd;
    const bool drained_before_signal = !notification_fd_readable(nfd);
    Completion<std::size_t> zero;
    if (!submit_zero_op(ctx, zero))
        return false;
    const bool readable_after_signal = notification_fd_readable(nfd);
    resume_gate(gate);
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = drained_before_signal && readable_after_signal && is_progress(result, 1) &&
                    prepark.load() == 1 && zero.ready();

    read.release();
    const auto final_r = wait_one_value(ctx, std::chrono::milliseconds{8000});
    zero.reset();
    return ok && is_progress(final_r, 1) && read.completion.ready() &&
           (read.completion.reset(), true);
}

// R9: the signal fires while the owner is already blocked inside poll(2); the
// kernel readiness wakes it and the re-run pass publishes the gated read.
bool r9_signal_while_blocked_in_poll_wakes_owner() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);
    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread driver([&] { DriverClaim owner{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    if (!wait_counter_reaches(prepark, 1))
        return false;

    read.release();
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = is_progress(result, 1) && prepark.load() == 1 && read.completion.ready();
    read.completion.reset();
    return ok;
}

// R10: signals raised while the owner is parked coalesce into one readiness; a
// single wake forwards the loop and one pass discovers every obligation.
bool r10_multiple_signals_coalesce_into_one_readiness() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(16, 1, &raw);
    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread driver([&] { DriverClaim owner{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    constexpr int kCoalesced = 8;
    std::vector<Completion<std::size_t>> zeros(kCoalesced);
    for (int i = 0; i < kCoalesced; ++i) {
        if (!submit_zero_op(ctx, zeros[static_cast<std::size_t>(i)]))
            return false;
    }
    resume_gate(gate);
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    bool ok = is_progress(result, static_cast<std::size_t>(kCoalesced)) && prepark.load() == 1;
    for (auto& z : zeros) {
        ok = ok && z.ready();
        z.reset();
    }

    read.release();
    const auto final_r = wait_one_value(ctx, std::chrono::milliseconds{8000});
    return ok && is_progress(final_r, 1) && read.completion.ready() &&
           (read.completion.reset(), true);
}

// R11: a wake with zero public completions does not fabricate a result; the
// owner re-runs the bounded pass and may park again safely.
bool r11_spurious_wake_with_zero_completions_is_harmless() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);
    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread driver([&] { DriverClaim owner{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    ctx.interrupt_progress_waiters();
    resume_gate(gate);
    driver.join();

    const bool no_fabrication = is_control(result) && !read.completion.ready();

    read.release();
    ctx.acknowledge_progress_control();
    const auto second = wait_one_value(ctx, std::chrono::milliseconds{8000});
    const bool ok = no_fabrication && is_progress(second, 1) && read.completion.ready();
    read.completion.reset();
    return ok;
}

// R11b: stale readiness from an already-serviced obligation is drained at the
// park without fabricating a completion and without blocking the later wake.
bool r11b_stale_readiness_drained_at_park_without_fabrication() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);
    HostInterest interest(ctx);

    Completion<std::size_t> zero;
    if (!submit_zero_op(ctx, zero))
        return false;
    // The settling pass runs on this thread's driving attachment, which is
    // released before the parked driver thread attaches as the next driver.
    bool settled_zero = false;
    bool stale_readable = false;
    {
        DriverClaim owner{ctx};
        const auto drained = ctx.poll_progress();
        settled_zero = drained.has_value() && drained.value().completed == 1 && zero.ready();
        zero.reset();
        stale_readable = notification_fd_readable(interest.fd);
    }
    if (!settled_zero || !stale_readable)
        return false;

    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread driver([&] { DriverClaim owner{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    const bool reached = wait_counter_reaches(prepark, 1);
    const bool stale_drained_at_park =
        reached && !notification_fd_readable(interest.fd);

    read.release();
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = reached && stale_drained_at_park && is_progress(result, 1) &&
                    read.completion.ready();
    read.completion.reset();
    return ok;
}

// R12: a control wake racing the park boundary ends the bounded wait without
// fabricating a completion (the sticky outcome taxonomy is C2-D scope).
bool r12_control_wake_racing_park_boundary() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);
    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    PauseGate pre_gate;
    ctx.set_progress_prerevalidate_pause_gate_for_test(&pre_gate);
    std::optional<AsyncIoContext::ProgressWaitOutcome> first;
    std::thread driver1([&] { DriverClaim owner{ctx}; first = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(pre_gate);
    ctx.set_progress_prerevalidate_pause_gate_for_test(nullptr);
    ctx.interrupt_progress_waiters();
    resume_gate(pre_gate);
    driver1.join();
    const bool pre_revalidate = is_control(first) && !read.completion.ready();

    // Retire the first control so the second round races a fresh park; an
    // unacknowledged pending control would end the next wait at its entry
    // check instead of at the park boundary.
    ctx.acknowledge_progress_control();

    PauseGate park_gate;
    ctx.set_progress_prepark_pause_gate_for_test(&park_gate);
    std::optional<AsyncIoContext::ProgressWaitOutcome> second;
    std::thread driver2([&] { DriverClaim owner{ctx}; second = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(park_gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);
    ctx.interrupt_progress_waiters();
    resume_gate(park_gate);
    driver2.join();

    const bool ok = pre_revalidate && is_control(second) && !read.completion.ready();
    read.release();
    ctx.acknowledge_progress_control();
    const auto final_r = wait_one_value(ctx, std::chrono::milliseconds{8000});
    return ok && is_progress(final_r, 1) && read.completion.ready() &&
           (read.completion.reset(), true);
}

// R13: terminal+published work whose public
// binding is already released leaves only a delayed control/reclaim
// obligation; the parked owner is woken by its signal and services it with no
// new I/O.
bool r13_delayed_reclaim_obligation_is_serviced_without_new_io() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{8, 2});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::optional<AsyncIoContext::ProgressWaitOutcome> driver_result;
    std::thread driver([&] {
        DriverClaim owner{ctx};
        driver_result = wait_one_value(ctx, std::chrono::milliseconds{8000});
    });
    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    Completion<std::size_t> zero;
    if (!ctx
             .submit_read_request(ReadOp{NativeFileRef(::fileno(tmpfile()),
                                                       sluice::FileAccess::read_only),
                                         nullptr, 0, 0},
                                  zero)
             .has_value())
        return false;
    const auto zero_key = raw->request_key_for_test(zero);
    if (!zero_key.has_value())
        return false;
    const detail::SlotIndex zero_slot = zero_key->slot;
    zero.reset();

    detail::RequestCore* core = raw->request_core_for_test();
    const auto obligation = core->observe_slot(zero_slot);
    const bool obligation_exists = obligation.has_value() &&
                                   obligation->phase == detail::RequestCore::SlotPhase::accepted &&
                                   obligation->published && obligation->control_refs == 1 &&
                                   raw->event_owed_for_test(zero_slot.value);

    resume_gate(gate);
    driver.join();

    const auto serviced = core->observe_slot(zero_slot);
    const bool obligation_serviced = serviced.has_value() &&
                                     serviced->phase == detail::RequestCore::SlotPhase::free &&
                                     serviced->control_refs == 0 &&
                                     !raw->event_owed_for_test(zero_slot.value);

    const bool ok =
        obligation_exists && obligation_serviced && is_progress(driver_result, 1);

    read.release();
    const auto final_r = wait_one_value(ctx, std::chrono::milliseconds{8000});
    return ok && is_progress(final_r, 1) && read.completion.ready() &&
           (read.completion.reset(), true);
}

// R15: with the progress epoch saturated, a fresh signal cannot move the
// token's epoch, so a handshake that treated token equality as stale would
// drain the worker's notification and sleep past actionable work. The
// exhaustion sequence must carry the freshness instead.
bool r15_saturated_signal_between_pass_and_revalidation_is_not_drained() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);
    HostInterest interest(ctx);
    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    ctx.set_progress_epoch_for_test(kMax);

    PauseGate gate;
    ctx.set_progress_prerevalidate_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread driver([&] { DriverClaim owner{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prerevalidate_pause_gate_for_test(nullptr);

    read.release();
    if (!wait_notification_readable(interest))
        return false;
    resume_gate(gate);
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = is_progress(result, 1) && prepark.load() == 0 &&
                    read.completion.ready() && ctx.progress_exhausted_for_test();
    read.completion.reset();
    return ok;
}

// Control-domain exhaustion: a saturated control epoch stays sticky and its
// exhaustion sequence carries the interrupt; the parked owner wakes as
// interrupted and never revalidates the fresh interrupt as stale readiness.
bool control_exhaustion_saturates_without_alias_and_wakes_interrupted() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);
    HostInterest interest(ctx);
    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    ctx.set_control_epoch_for_test(kMax);

    // The injected epoch advance is itself an unacknowledged control
    // generation: observe and retire it so the parked wait below tests the
    // fresh interrupt, not the injected gap.
    {
        DriverClaim owner{ctx};
        const auto settle = wait_one_value(ctx, std::chrono::milliseconds{100});
        if (!is_control(settle))
            return false;
    }
    ctx.acknowledge_progress_control();

    PauseGate gate;
    ctx.set_progress_prerevalidate_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread driver([&] { DriverClaim owner{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prerevalidate_pause_gate_for_test(nullptr);

    ctx.interrupt_progress_waiters();
    const bool readable = notification_fd_readable(interest.fd);

    resume_gate(gate);
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = readable && is_control(result) && prepark.load() == 0 &&
                    !read.completion.ready();

    read.release();
    ctx.acknowledge_progress_control();
    const auto final_r = wait_one_value(ctx, std::chrono::milliseconds{8000});
    return ok && is_progress(final_r, 1) && read.completion.ready() &&
           (read.completion.reset(), true);
}

// Once the outer control freshness domain is spent (saturated epoch and
// saturated exhaustion), the generation pair freezes at its absorbing value:
// further interrupts can no longer be distinguished, so every wait must treat
// control as pending instead of parking on a reusable value.
bool control_outer_exhaustion_is_terminal_and_never_aliases() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);
    HostInterest interest(ctx);
    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    ctx.set_control_epoch_for_test(kMax);
    ctx.set_control_exhaustion_for_test(kMax - 1);

    const bool ok = [&] {
        ctx.interrupt_progress_waiters();
        if (!notification_fd_readable(interest.fd))
            return false;

        for (int round = 0; round < 3; ++round) {
            ctx.interrupt_progress_waiters();
            if (!notification_fd_readable(interest.fd))
                return false;

            std::atomic<int> prepark{0};
            ctx.set_progress_prepark_counter_for_test(&prepark);
            const auto r = wait_one_value(ctx, std::chrono::milliseconds{8000});
            ctx.set_progress_prepark_counter_for_test(nullptr);
            if (!is_control(r) || prepark.load() != 0)
                return false;
            if (read.completion.ready())
                return false;
        }
        return true;
    }();

    read.release();
    ctx.acknowledge_progress_control();
    std::optional<AsyncIoContext::ProgressWaitOutcome> final_r;
    for (int i = 0; i < 1000; ++i) {
        final_r = wait_one_value(ctx, std::chrono::milliseconds{8000});
        if (is_progress(final_r, 1))
            break;
    }
    return ok && is_progress(final_r, 1) && read.completion.ready() &&
           (read.completion.reset(), true);
}

// Token exhaustion: saturation, not wrap. The observed token can never alias a
// later epoch and exhaustion closes admission at the owner's next pass.
bool token_exhaustion_saturates_without_alias_and_closes_admission() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncIoContext ctx(std::move(backend));
    HostInterest interest(ctx);

    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    ctx.set_progress_epoch_for_test(kMax);

    Completion<std::size_t> zero;
    if (!submit_zero_op(ctx, zero))
        return false;

    const auto token = ctx.progress_token_for_test();
    if (token.progress != kMax)
        return false;
    if (!ctx.progress_exhausted_for_test())
        return false;
    if (!notification_fd_readable(interest.fd))
        return false;

    const auto r = wait_one_value(ctx, std::chrono::milliseconds{5000});
    if (!is_progress(r, 1))
        return false;

    std::vector<std::byte> buffer(8, std::byte{0});
    Completion<std::size_t> rejected_slot;
    const auto rejected =
        ctx.submit_read(ReadOp{NativeFileRef(::fileno(tmpfile()), sluice::FileAccess::read_only),
                               buffer.data(), buffer.size(), 0},
                        rejected_slot);
    if (rejected.has_value() || rejected.error().code != IoError::Code::invalid_state)
        return false;

    const auto after = ctx.progress_token_for_test();
    zero.reset();
    return after.progress == kMax;
}

// An owner parked on the saturated epoch still wakes (the notification write
// fires without an epoch change), reparks safely on the zero-completion
// readiness, and finishes the real work.
bool owner_parked_at_saturated_epoch_wakes_and_reparks() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);
    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    ctx.set_progress_epoch_for_test(kMax);

    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread driver([&] { DriverClaim owner{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    if (!wait_counter_reaches(prepark, 1))
        return false;

    ctx.saturate_progress_notification_for_test();
    if (!wait_counter_reaches(prepark, 2))
        return false;

    read.release();
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const auto token = ctx.progress_token_for_test();
    const bool ok = is_progress(result, 1) && read.completion.ready() &&
                    token.progress == kMax && ctx.progress_exhausted_for_test();
    read.completion.reset();
    return ok;
}

// Saturation: a signal arriving while the notification fd already holds the
// maximal counter observes EAGAIN and still wakes the parked owner.
bool saturated_notification_still_wakes_parked_owner() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);
    HostInterest interest(ctx);
    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<AsyncIoContext::ProgressWaitOutcome> result;
    std::thread driver([&] { DriverClaim owner{ctx}; result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    ctx.saturate_progress_notification_for_test();
    if (!notification_fd_readable(interest.fd))
        return false;

    read.release();
    const bool readable_after_saturated_signal =
        notification_fd_readable(interest.fd);
    resume_gate(gate);
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok =
        readable_after_saturated_signal && is_progress(result, 1) && prepark.load() == 1 &&
        read.completion.ready();
    read.completion.reset();
    return ok;
}

// Coalescing established before the wait: many signals, one readiness, one
// pass discovers every obligation, and one acknowledgement clears the fd.
bool coalesced_prearmed_signals_are_all_discoverable_in_one_pass() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{16, 1});
    AsyncIoContext ctx(std::move(backend));
    HostInterest interest(ctx);

    constexpr int kCoalesced = 8;
    std::vector<Completion<std::size_t>> zeros(kCoalesced);
    for (int i = 0; i < kCoalesced; ++i) {
        if (!submit_zero_op(ctx, zeros[static_cast<std::size_t>(i)]))
            return false;
    }

    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);
    const auto r = wait_one_value(ctx, std::chrono::milliseconds{5000});
    ctx.set_progress_prepark_counter_for_test(nullptr);

    bool all_ready = is_progress(r, static_cast<std::size_t>(kCoalesced)) && prepark.load() == 0;
    for (auto& z : zeros) {
        all_ready = all_ready && z.ready();
        z.reset();
    }
    ctx.acknowledge_progress_notification();
    return all_ready && !notification_fd_readable(interest.fd);
}

// F1 oracle: one poll_progress invocation publishes at most the publication
// work pending at its entry. Work a concurrent producer posts while the pass
// is mid-publication (here the held worker completing a gated request,
// observed through the progress token) stays for the next pass and is
// reported as remaining immediate work.
bool f1_publication_pass_is_entry_bounded() {
    ThreadPoolBackend* raw = nullptr;
    auto ctx = make_pool_context(8, 1, &raw);
    HostInterest interest(ctx);

    Completion<std::size_t> a;
    std::vector<std::byte> a_buf(32, std::byte{0});
    const int a_fd = temp_file_fd(std::string(64, 'x'));
    if (a_fd < 0)
        return false;
    if (!ctx.submit_read(ReadOp{NativeFileRef(a_fd, sluice::FileAccess::read_only),
                                 a_buf.data(), a_buf.size(), 0},
                         a)
             .has_value())
        return false;
    if (!wait_notification_readable(interest))
        return false;

    GatedWorkerRead gated;
    if (!gated.arm(ctx, raw))
        return false;

    ThreadPoolBackend::PublicationEpiloguePauseGate epilogue;
    raw->set_publication_epilogue_pause_gate(&epilogue);

    std::optional<AsyncBackend::ProgressPass> first;
    std::thread driver([&] {
        DriverClaim claim{ctx};
        const auto r = ctx.poll_progress();
        if (r.has_value())
            first = r.value();
    });
    wait_threadpool_gate_paused(epilogue);
    const bool a_published_at_pause = a.ready();

    const auto token_before_release = ctx.progress_token_for_test();
    gated.release();
    while (ctx.progress_token_for_test().progress == token_before_release.progress)
        std::this_thread::yield();

    resume_threadpool_gate(epilogue);
    driver.join();
    raw->set_publication_epilogue_pause_gate(nullptr);

    const bool first_ok = a_published_at_pause && first.has_value() &&
                          first->completed == 1 && first->immediate_work_remains &&
                          !first->health_failed;
    if (!first_ok)
        return false;
    a.reset();
    ::close(a_fd);

    const auto second = ctx.poll_progress();
    const bool second_ok = second.has_value() && second.value().completed == 1 &&
                           gated.completion.ready() &&
                           !second.value().immediate_work_remains &&
                           !second.value().accepted_work_remains &&
                           !second.value().health_failed;

    gated.completion.reset();
    return second_ok;
}

// The delivery-record handoff under submit-concurrent-with-poll: the
// submitting thread pauses between initializing the slot's delivery record
// and binding it while the owner sweeps every slot repeatedly through the
// backend pass. The paused window is exactly where an unsynchronized record
// read/write would meet; after release the record must hand off whole —
// nothing observed early, exactly one completion and one owed-event
// delivery. Backend-level passes are the owner's sweep; the context entry
// lock is held by the paused submitter, so a context-level drive would
// serialize instead of racing.
bool record_handoff_survives_submit_racing_owner_sweep() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);
    HostInterest interest(ctx);

    ThreadPoolBackend::PreAcceptCommitPauseGate gate;
    raw->set_pre_accept_commit_pause_gate(&gate);

    Completion<std::size_t> zero;
    std::vector<std::byte> buffer(4, std::byte{0});
    std::atomic<bool> submit_ok{false};
    std::thread submitter([&] {
        submit_ok.store(
            ctx.submit_read(ReadOp{NativeFileRef(::fileno(tmpfile()),
                                                sluice::FileAccess::read_only),
                                   buffer.data(), 0, 0},
                            zero)
                .has_value(),
            std::memory_order_release);
    });
    wait_threadpool_gate_paused(gate);

    std::size_t completed_in_window = 0;
    for (int i = 0; i < 64; ++i)
        completed_in_window += raw->poll();
    const bool window_clean = completed_in_window == 0 && !zero.ready();

    raw->set_pre_accept_commit_pause_gate(nullptr);
    resume_threadpool_gate(gate);

    std::size_t delivered = 0;
    for (int i = 0; i < 200000 && delivered == 0; ++i) {
        delivered += raw->poll();
        if (delivered == 0)
            std::this_thread::yield();
    }
    submitter.join();
    const std::size_t settle = raw->poll();

    const bool result_ok = zero.ready() && zero.result().has_value() && zero.result().value() == 0;
    const bool ok = window_clean && delivered == 1 && settle == 0 && result_ok &&
                    submit_ok.load(std::memory_order_acquire);
    zero.reset();
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
        {"r1_signal_before_wait_is_serviced_without_parking",
         r1_signal_before_wait_is_serviced_without_parking},
        {"r2_signal_between_token_and_ack_is_revalidation_caught",
         r2_signal_between_token_and_ack_is_revalidation_caught},
        {"r3_signal_after_ack_wakes_poll_before_next_snapshot",
         r3_signal_after_ack_wakes_poll_before_next_snapshot},
        {"r4_worker_completion_signal_before_final_recheck_is_caught",
         r4_worker_completion_signal_before_final_recheck_is_caught},
        {"r5_postdrain_signal_readiness_persists_into_poll",
         r5_postdrain_signal_readiness_persists_into_poll},
        {"r8_empty_pass_signal_immediately_before_poll_entry",
         r8_empty_pass_signal_immediately_before_poll_entry},
        {"r9_signal_while_blocked_in_poll_wakes_owner",
         r9_signal_while_blocked_in_poll_wakes_owner},
        {"r10_multiple_signals_coalesce_into_one_readiness",
         r10_multiple_signals_coalesce_into_one_readiness},
        {"r11_spurious_wake_with_zero_completions_is_harmless",
         r11_spurious_wake_with_zero_completions_is_harmless},
        {"r11b_stale_readiness_drained_at_park_without_fabrication",
         r11b_stale_readiness_drained_at_park_without_fabrication},
        {"r12_control_wake_racing_park_boundary", r12_control_wake_racing_park_boundary},
        {"r13_delayed_reclaim_obligation_is_serviced_without_new_io",
         r13_delayed_reclaim_obligation_is_serviced_without_new_io},
        {"r15_saturated_signal_between_pass_and_revalidation_is_not_drained",
         r15_saturated_signal_between_pass_and_revalidation_is_not_drained},
        {"control_exhaustion_saturates_without_alias_and_wakes_interrupted",
         control_exhaustion_saturates_without_alias_and_wakes_interrupted},
        {"control_outer_exhaustion_is_terminal_and_never_aliases",
         control_outer_exhaustion_is_terminal_and_never_aliases},
        {"token_exhaustion_saturates_without_alias_and_closes_admission",
         token_exhaustion_saturates_without_alias_and_closes_admission},
        {"owner_parked_at_saturated_epoch_wakes_and_reparks",
         owner_parked_at_saturated_epoch_wakes_and_reparks},
        {"saturated_notification_still_wakes_parked_owner",
         saturated_notification_still_wakes_parked_owner},
        {"coalesced_prearmed_signals_are_all_discoverable_in_one_pass",
         coalesced_prearmed_signals_are_all_discoverable_in_one_pass},
        {"f1_publication_pass_is_entry_bounded", f1_publication_pass_is_entry_bounded},
        {"record_handoff_survives_submit_racing_owner_sweep",
         record_handoff_survives_submit_racing_owner_sweep},
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
    std::printf("all %zu threadpool progress race tests passed\n", passed);
    return 0;
}
