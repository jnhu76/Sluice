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

// Drives the driver thread result into an optional: nullopt means the bounded
// wait returned without a completion (deadline, interrupt, or idle return).
std::optional<std::size_t> wait_one_value(AsyncIoContext& ctx, std::chrono::nanoseconds bound) {
    const auto r = ctx.wait_one(bound);
    if (!r.has_value())
        return std::nullopt;
    return r.value();
}

// R1: a signal preceding the wait is discoverable by the first bounded pass;
// the owner never needs to park.
bool r1_signal_before_wait_is_serviced_without_parking() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncIoContext ctx(std::move(backend));

    Completion<std::size_t> c;
    if (!submit_zero_op(ctx, c))
        return false;
    if (!notification_fd_readable(ctx.progress_notification_fd()))
        return false;

    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);
    const auto r = wait_one_value(ctx, std::chrono::milliseconds{5000});
    ctx.set_progress_prepark_counter_for_test(nullptr);

    return r.has_value() && r.value() == 1 && prepark.load() == 0 && c.ready();
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

    std::optional<std::size_t> result;
    std::thread driver([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });

    wait_gate_paused(gate);
    ctx.set_progress_prerevalidate_pause_gate_for_test(nullptr);

    Completion<std::size_t> zero;
    if (!submit_zero_op(ctx, zero))
        return false;
    resume_gate(gate);
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = result.has_value() && result.value() == 1 && prepark.load() == 0 &&
                    zero.ready();

    read.release();
    const auto final_r = wait_one_value(ctx, std::chrono::milliseconds{8000});
    zero.reset();
    return ok && final_r.has_value() && final_r.value() == 1 && read.completion.ready() &&
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

    std::optional<std::size_t> result;
    std::thread driver([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });

    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    Completion<std::size_t> zero;
    if (!submit_zero_op(ctx, zero))
        return false;
    resume_gate(gate);
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = result.has_value() && result.value() == 1 && prepark.load() == 1 &&
                    zero.ready();

    read.release();
    const auto final_r = wait_one_value(ctx, std::chrono::milliseconds{8000});
    zero.reset();
    return ok && final_r.has_value() && final_r.value() == 1 && read.completion.ready() &&
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

    std::optional<std::size_t> result;
    std::thread driver([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prerevalidate_pause_gate_for_test(nullptr);

    read.release();
    resume_gate(gate);
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = result.has_value() && result.value() == 1 && prepark.load() == 0 &&
                    read.completion.ready();
    read.completion.reset();
    return ok;
}

// R5: readiness raised after the pass boundary is observable while the owner
// is paused and the wake is delivered on resume.
bool r5_postdrain_signal_readiness_persists_into_poll() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);
    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<std::size_t> result;
    std::thread driver([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    Completion<std::size_t> zero;
    if (!submit_zero_op(ctx, zero))
        return false;
    const bool readable_while_paused = notification_fd_readable(ctx.progress_notification_fd());
    resume_gate(gate);
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = readable_while_paused && result.has_value() && result.value() == 1 &&
                    prepark.load() == 1 && zero.ready();

    read.release();
    const auto final_r = wait_one_value(ctx, std::chrono::milliseconds{8000});
    zero.reset();
    return ok && final_r.has_value() && final_r.value() == 1 && read.completion.ready() &&
           (read.completion.reset(), true);
}

// R8 / the especially dangerous empty-poll shape: the pass is empty, the stale
// acknowledgement is already done, and the signal fires at the paused owner.
// The readiness transition is observable while parked; poll(2) itself returns.
bool r8_empty_pass_signal_immediately_before_poll_entry() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);
    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<std::size_t> result;
    std::thread driver([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    const int nfd = ctx.progress_notification_fd();
    const bool drained_before_signal = !notification_fd_readable(nfd);
    Completion<std::size_t> zero;
    if (!submit_zero_op(ctx, zero))
        return false;
    const bool readable_after_signal = notification_fd_readable(nfd);
    resume_gate(gate);
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = drained_before_signal && readable_after_signal && result.has_value() &&
                    result.value() == 1 && prepark.load() == 1 && zero.ready();

    read.release();
    const auto final_r = wait_one_value(ctx, std::chrono::milliseconds{8000});
    zero.reset();
    return ok && final_r.has_value() && final_r.value() == 1 && read.completion.ready() &&
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

    std::optional<std::size_t> result;
    std::thread driver([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    if (!wait_counter_reaches(prepark, 1))
        return false;

    read.release();
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = result.has_value() && result.value() == 1 && prepark.load() == 1 &&
                    read.completion.ready();
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

    std::optional<std::size_t> result;
    std::thread driver([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
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

    bool ok = result.has_value() && result.value() == static_cast<std::size_t>(kCoalesced) &&
              prepark.load() == 1;
    for (auto& z : zeros) {
        ok = ok && z.ready();
        z.reset();
    }

    read.release();
    const auto final_r = wait_one_value(ctx, std::chrono::milliseconds{8000});
    return ok && final_r.has_value() && final_r.value() == 1 && read.completion.ready() &&
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
    std::optional<std::size_t> result;
    std::thread driver([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    ctx.interrupt_progress_waiters();
    resume_gate(gate);
    driver.join();

    const bool no_fabrication = result.has_value() && result.value() == 0 &&
                                !read.completion.ready();

    read.release();
    const auto second = wait_one_value(ctx, std::chrono::milliseconds{8000});
    const bool ok = no_fabrication && second.has_value() && second.value() == 1 &&
                    read.completion.ready();
    read.completion.reset();
    return ok;
}

// R11b: stale readiness from an already-serviced obligation is drained at the
// park without fabricating a completion and without blocking the later wake.
bool r11b_stale_readiness_drained_at_park_without_fabrication() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);

    Completion<std::size_t> zero;
    if (!submit_zero_op(ctx, zero))
        return false;
    const auto drained = ctx.poll_progress();
    if (drained.completed != 1 || !zero.ready())
        return false;
    zero.reset();
    if (!notification_fd_readable(ctx.progress_notification_fd()))
        return false;

    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<std::size_t> result;
    std::thread driver([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    if (!wait_counter_reaches(prepark, 1))
        return false;
    const bool stale_drained_at_park = !notification_fd_readable(ctx.progress_notification_fd());

    read.release();
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = stale_drained_at_park && result.has_value() && result.value() == 1 &&
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
    std::optional<std::size_t> first;
    std::thread driver1([&] { first = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(pre_gate);
    ctx.set_progress_prerevalidate_pause_gate_for_test(nullptr);
    ctx.interrupt_progress_waiters();
    resume_gate(pre_gate);
    driver1.join();
    const bool pre_revalidate = first.has_value() && first.value() == 0 &&
                                !read.completion.ready();

    PauseGate park_gate;
    ctx.set_progress_prepark_pause_gate_for_test(&park_gate);
    std::optional<std::size_t> second;
    std::thread driver2([&] { second = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(park_gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);
    ctx.interrupt_progress_waiters();
    resume_gate(park_gate);
    driver2.join();

    const bool ok = pre_revalidate && second.has_value() && second.value() == 0 &&
                    !read.completion.ready();
    read.release();
    const auto final_r = wait_one_value(ctx, std::chrono::milliseconds{8000});
    return ok && final_r.has_value() && final_r.value() == 1 && read.completion.ready() &&
           (read.completion.reset(), true);
}

// R13 / the #394 L5 progress half: terminal+published work whose public
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
    std::optional<std::size_t> driver_result;
    std::thread driver([&] {
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

    const bool ok = obligation_exists && obligation_serviced && driver_result.has_value() &&
                    driver_result.value() == 1;

    read.release();
    const auto final_r = wait_one_value(ctx, std::chrono::milliseconds{8000});
    return ok && final_r.has_value() && final_r.value() == 1 && read.completion.ready() &&
           (read.completion.reset(), true);
}

// Token exhaustion: saturation, not wrap. The observed token can never alias a
// later epoch and exhaustion closes admission at the owner's next pass.
bool token_exhaustion_saturates_without_alias_and_closes_admission() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncIoContext ctx(std::move(backend));

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
    if (!notification_fd_readable(ctx.progress_notification_fd()))
        return false;

    const auto r = wait_one_value(ctx, std::chrono::milliseconds{5000});
    if (!r.has_value() || r.value() != 1)
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

    std::optional<std::size_t> result;
    std::thread driver([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    if (!wait_counter_reaches(prepark, 1))
        return false;

    ctx.saturate_progress_notification_for_test();
    if (!wait_counter_reaches(prepark, 2))
        return false;

    read.release();
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const auto token = ctx.progress_token_for_test();
    const bool ok = result.has_value() && result.value() == 1 && read.completion.ready() &&
                    token.progress == kMax && ctx.progress_exhausted_for_test();
    read.completion.reset();
    return ok;
}

// Saturation: a signal arriving while the notification fd already holds the
// maximal counter observes EAGAIN and still wakes the parked owner.
bool saturated_notification_still_wakes_parked_owner() {
    ThreadPoolBackend* raw = nullptr;
    AsyncIoContext ctx = make_pool_context(4, 1, &raw);
    GatedWorkerRead read;
    if (!read.arm(ctx, raw))
        return false;

    PauseGate gate;
    ctx.set_progress_prepark_pause_gate_for_test(&gate);
    std::atomic<int> prepark{0};
    ctx.set_progress_prepark_counter_for_test(&prepark);

    std::optional<std::size_t> result;
    std::thread driver([&] { result = wait_one_value(ctx, std::chrono::milliseconds{8000}); });
    wait_gate_paused(gate);
    ctx.set_progress_prepark_pause_gate_for_test(nullptr);

    ctx.saturate_progress_notification_for_test();
    if (!notification_fd_readable(ctx.progress_notification_fd()))
        return false;

    read.release();
    const bool readable_after_saturated_signal =
        notification_fd_readable(ctx.progress_notification_fd());
    resume_gate(gate);
    driver.join();
    ctx.set_progress_prepark_counter_for_test(nullptr);

    const bool ok = readable_after_saturated_signal && result.has_value() &&
                    result.value() == 1 && prepark.load() == 1 && read.completion.ready();
    read.completion.reset();
    return ok;
}

// Coalescing established before the wait: many signals, one readiness, one
// pass discovers every obligation, and one acknowledgement clears the fd.
bool coalesced_prearmed_signals_are_all_discoverable_in_one_pass() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{16, 1});
    AsyncIoContext ctx(std::move(backend));

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

    bool all_ready = r.has_value() && r.value() == static_cast<std::size_t>(kCoalesced) &&
                     prepark.load() == 0;
    for (auto& z : zeros) {
        all_ready = all_ready && z.ready();
        z.reset();
    }
    ctx.acknowledge_progress_notification();
    return all_ready && !notification_fd_readable(ctx.progress_notification_fd());
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
        {"token_exhaustion_saturates_without_alias_and_closes_admission",
         token_exhaustion_saturates_without_alias_and_closes_admission},
        {"owner_parked_at_saturated_epoch_wakes_and_reparks",
         owner_parked_at_saturated_epoch_wakes_and_reparks},
        {"saturated_notification_still_wakes_parked_owner",
         saturated_notification_still_wakes_parked_owner},
        {"coalesced_prearmed_signals_are_all_discoverable_in_one_pass",
         coalesced_prearmed_signals_are_all_discoverable_in_one_pass},
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
