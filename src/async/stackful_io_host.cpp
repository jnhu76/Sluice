#include <sluice/async/stackful_io_host.hpp>

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/detail/file_semantics.hpp>

#include <cstdio>
#include <exception>
#include <new>
#include <system_error>

namespace sluice::async {

thread_local StackfulIoHost* StackfulIoHost::running_host_ = nullptr;
thread_local StackfulIoHost::TaskSlot* StackfulIoHost::running_slot_ = nullptr;

namespace {

[[maybe_unused]] IoError map_current_exception_to_task_error() noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        return IoError{IoError::Code::no_space, 0};
    } catch (const std::system_error& e) {
        const int native = e.code().value();
        return IoError{sluice::detail::canonical_error_code(native), native};
    } catch (...) {
        return IoError{IoError::Code::backend_error, 0};
    }
}

blocking::EffectCertainty publish_certainty(sluice::detail::EffectCertainty certainty) noexcept {
    return certainty == sluice::detail::EffectCertainty::accounted
               ? blocking::EffectCertainty::accounted
               : blocking::EffectCertainty::unknown;
}

blocking::CompositionOutcome
compose_outcome(const sluice::detail::CompositionState& state) noexcept {
    blocking::CompositionOutcome outcome;
    outcome.confirmed_bytes = state.confirmed_bytes;
    outcome.remaining = publish_certainty(sluice::detail::composition_effect_certainty(state));
    switch (state.stop) {
    case sluice::detail::CompositionStop::complete:
        outcome.end = blocking::CompositionEnd::complete;
        break;
    case sluice::detail::CompositionStop::eof_before_full:
        outcome.end = blocking::CompositionEnd::eof_before_full;
        break;
    case sluice::detail::CompositionStop::write_no_progress:
        outcome.end = blocking::CompositionEnd::write_no_progress;
        break;
    case sluice::detail::CompositionStop::primitive_error:
        outcome.end = blocking::CompositionEnd::primitive_error;
        outcome.error = state.error;
        break;
    case sluice::detail::CompositionStop::impossible_count:
        outcome.end = blocking::CompositionEnd::primitive_error;
        outcome.error = IoError{.code = IoError::Code::invalid_state};
        break;
    }
    return outcome;
}

// One settled primitive of a host composition. A step the submission
// transaction rejected (`accepted == false`) left no request behind: its
// `settled` error is that rejection, not an operation result.
struct HostStep {
    bool accepted;
    Result<std::size_t> settled;
};

// A rejection before this invocation's first accepted request accepted
// nothing anywhere, so it rejects the invocation itself (outer result).
// After any acceptance the composition owns its progress and every later
// failure — including a terminal error with zero confirmed bytes and a
// later admission rejection — reports through the outcome's primitive-error
// arm with the confirmed prefix. An accepted first step also proves offset
// + total stays representable, so every later offset advance lands inside
// the range that step validated.
template <class Primitive>
Result<blocking::CompositionOutcome> compose_host_steps(sluice::detail::CompositionKind kind,
                                                        std::size_t total,
                                                        Primitive&& primitive) {
    sluice::detail::CompositionState state;
    bool any_accepted = false;
    while (!state.stopped && state.confirmed_bytes < total) {
        const std::size_t confirmed = state.confirmed_bytes;
        const HostStep step = primitive(confirmed);
        if (!step.accepted && !any_accepted) {
            return make_unexpected<blocking::CompositionOutcome>(step.settled.error());
        }
        any_accepted = any_accepted || step.accepted;
        if (!step.accepted || !step.settled.has_value()) {
            state = sluice::detail::compose_error(state, step.settled.error());
            break;
        }
        state = sluice::detail::compose_progress(kind, total, state, step.settled.value());
    }
    return compose_outcome(state);
}

template <class Span, class Primitive>
Result<blocking::CompositionOutcome> compose_host_invocation(
    NativeFileRef file, std::uint64_t offset, Span buffer,
    sluice::detail::FileOperation operation, bool host_stopped, Primitive&& primitive) {
    [[maybe_unused]] const sluice::detail::DataOpVerdict verdict =
        sluice::detail::precheck_data_op(
            {file.fd < 0, file.access, operation, offset, buffer.size()});
#if !defined(SLUICE_STACKFUL_HOST_MUTANT_EMPTY_COMPOSITION_BYPASSES_PRECHECK)
    if (const auto rejection = sluice::detail::rejection_of(verdict); rejection.has_value()) {
        return make_unexpected<blocking::CompositionOutcome>(*rejection);
    }
    if (verdict == sluice::detail::DataOpVerdict::execute && buffer.data() == nullptr) {
        return make_unexpected<blocking::CompositionOutcome>(
            IoError{.code = IoError::Code::invalid_argument});
    }
    if (host_stopped) {
        return make_unexpected<blocking::CompositionOutcome>(IoError{.code = IoError::Code::canceled});
    }
    if (verdict == sluice::detail::DataOpVerdict::complete_empty) {
        const HostStep no_op = primitive(0);
        if (!no_op.accepted) {
            return make_unexpected<blocking::CompositionOutcome>(no_op.settled.error());
        }
        if (!no_op.settled.has_value()) {
            sluice::detail::CompositionState state;
            state = sluice::detail::compose_error(state, no_op.settled.error());
            return compose_outcome(state);
        }
        return blocking::CompositionOutcome{};
    }
#endif
    const sluice::detail::CompositionKind kind =
        operation == sluice::detail::FileOperation::read
            ? sluice::detail::CompositionKind::read_exact
            : sluice::detail::CompositionKind::write_all;
    return compose_host_steps(kind, buffer.size(),
                              [&](std::size_t confirmed) { return primitive(confirmed); });
}

} // namespace

Result<std::size_t> IoTaskContext::read(NativeFileRef file, std::span<std::byte> dst,
                                        std::uint64_t offset) {
    return StackfulIoHost::await_request_<std::size_t>(
        *host_, [&] { return host_->ctx_.submit_read(ReadOp{file, dst.data(), dst.size(), offset}); },
        false, std::chrono::nanoseconds::max());
}

Result<std::size_t> IoTaskContext::write(NativeFileRef file, std::span<const std::byte> src,
                                         std::uint64_t offset) {
    return StackfulIoHost::await_request_<std::size_t>(
        *host_,
        [&] { return host_->ctx_.submit_write(WriteOp{file, src.data(), src.size(), offset}); },
        false, std::chrono::nanoseconds::max());
}

Result<void> IoTaskContext::sync_data(NativeFileRef file) {
    return StackfulIoHost::await_request_<void>(
        *host_, [&] { return host_->ctx_.submit_sync_data(SyncDataOp{file}); }, false,
        std::chrono::nanoseconds::max());
}

Result<void> IoTaskContext::sync_all(NativeFileRef file) {
    return StackfulIoHost::await_request_<void>(
        *host_, [&] { return host_->ctx_.submit_sync_all(SyncAllOp{file}); }, false,
        std::chrono::nanoseconds::max());
}

Result<std::size_t> IoTaskContext::read_for(NativeFileRef file, std::span<std::byte> dst,
                                            std::uint64_t offset, std::chrono::nanoseconds wait) {
    return StackfulIoHost::await_request_<std::size_t>(
        *host_, [&] { return host_->ctx_.submit_read(ReadOp{file, dst.data(), dst.size(), offset}); },
        true, wait);
}

Result<std::size_t> IoTaskContext::write_for(NativeFileRef file, std::span<const std::byte> src,
                                             std::uint64_t offset,
                                             std::chrono::nanoseconds wait) {
    return StackfulIoHost::await_request_<std::size_t>(
        *host_,
        [&] { return host_->ctx_.submit_write(WriteOp{file, src.data(), src.size(), offset}); },
        true, wait);
}

Result<void> IoTaskContext::sync_data_for(NativeFileRef file, std::chrono::nanoseconds wait) {
    return StackfulIoHost::await_request_<void>(
        *host_, [&] { return host_->ctx_.submit_sync_data(SyncDataOp{file}); }, true, wait);
}

Result<void> IoTaskContext::sync_all_for(NativeFileRef file, std::chrono::nanoseconds wait) {
    return StackfulIoHost::await_request_<void>(
        *host_, [&] { return host_->ctx_.submit_sync_all(SyncAllOp{file}); }, true, wait);
}

Result<blocking::CompositionOutcome> IoTaskContext::read_exact(NativeFileRef file,
                                                               std::span<std::byte> dst,
                                                               std::uint64_t offset) {
    return compose_host_invocation(
        file, offset, dst, sluice::detail::FileOperation::read, host_->stop_requested(),
        [&](std::size_t confirmed) -> HostStep {
            bool accepted = false;
            auto settled = StackfulIoHost::await_request_<std::size_t>(
                *host_,
                [&] {
                    const std::span<std::byte> remaining = dst.subspan(confirmed);
                    return host_->ctx_.submit_read(
                        ReadOp{file, remaining.data(), remaining.size(), offset + confirmed});
                },
                false, std::chrono::nanoseconds::max(), &accepted);
            return HostStep{accepted, std::move(settled)};
        });
}

Result<blocking::CompositionOutcome> IoTaskContext::write_all(NativeFileRef file,
                                                              std::span<const std::byte> src,
                                                              std::uint64_t offset) {
    return compose_host_invocation(
        file, offset, src, sluice::detail::FileOperation::write, host_->stop_requested(),
        [&](std::size_t confirmed) -> HostStep {
            bool accepted = false;
            auto settled = StackfulIoHost::await_request_<std::size_t>(
                *host_,
                [&] {
                    const std::span<const std::byte> remaining = src.subspan(confirmed);
                    return host_->ctx_.submit_write(
                        WriteOp{file, remaining.data(), remaining.size(), offset + confirmed});
                },
                false, std::chrono::nanoseconds::max(), &accepted);
            return HostStep{accepted, std::move(settled)};
        });
}

StackfulIoHost::StackfulIoHost(AsyncIoContext& ctx, std::size_t task_capacity)
    : ctx_(ctx), task_capacity_(task_capacity) {}

Result<std::unique_ptr<StackfulIoHost>> StackfulIoHost::create(AsyncIoContext& ctx,
                                                                StackfulHostConfig config) {
    if (config.task_capacity == 0) {
        return make_unexpected<std::unique_ptr<StackfulIoHost>>(
            IoError{IoError::Code::invalid_argument});
    }
    if (config.stack_bytes < kMinStackBytes) {
        return make_unexpected<std::unique_ptr<StackfulIoHost>>(
            IoError{IoError::Code::invalid_argument});
    }
    if constexpr (!fiber_ctx::supported) {
        return make_unexpected<std::unique_ptr<StackfulIoHost>>(
            IoError{IoError::Code::not_supported});
    }
    if (!ctx.has_split_wait_capability()) {
        return make_unexpected<std::unique_ptr<StackfulIoHost>>(
            IoError{IoError::Code::not_supported});
    }
    return create_validated_(ctx, config);
}

Result<std::unique_ptr<StackfulIoHost>> StackfulIoHost::create_validated_(
    AsyncIoContext& ctx, StackfulHostConfig config) {
    std::unique_ptr<StackfulIoHost> host(new StackfulIoHost(ctx, config.task_capacity));
    host->stack_bytes_ = config.stack_bytes;
    host->slots_ = std::make_unique<TaskSlot[]>(config.task_capacity);
    host->ready_ring_ = std::make_unique<std::size_t[]>(config.task_capacity);
    for (std::size_t i = 0; i < config.task_capacity; ++i) {
        TaskSlot& slot = host->slots_[i];
        slot.host = host.get();
        slot.stack = std::make_unique<std::byte[]>(config.stack_bytes);
        if (!host->prepare_slot_fiber_(slot)) {
            return make_unexpected<std::unique_ptr<StackfulIoHost>>(
                IoError{IoError::Code::invalid_state});
        }
    }
    return host;
}

bool StackfulIoHost::prepare_slot_fiber_(TaskSlot& slot) noexcept {
    std::destroy_at(&slot.fiber);
    new (&slot.fiber) Fiber();
    return fiber_ctx::init_context(slot.fiber.ctx, &StackfulIoHost::task_entry_bridge_, &slot,
                                   slot.stack.get(), stack_bytes_);
}

StackfulIoHost::~StackfulIoHost() {
    for (std::size_t i = 0; i < task_capacity_; ++i) {
        if (slots_ && slots_[i].live) {
            detail::stackful_host_live_task_fail_fast();
        }
    }
}

void StackfulIoHost::task_entry_bridge_(fiber_ctx::Switch* resumed_by, void* userdata) {
    (void)resumed_by;
    TaskSlot& slot = *static_cast<TaskSlot*>(userdata);
    StackfulIoHost& host = *slot.host;

    {
        auto entry = std::move(slot.entry);
        slot.entry = nullptr;

        IoTaskContext task(host, host.stop_token_);
        try {
            entry(task);
        } catch (...) {
#if !defined(SLUICE_STACKFUL_HOST_MUTANT_TASK_ERROR_SWALLOWED)
            host.record_task_error_(map_current_exception_to_task_error());
#endif
        }
    }

#if !defined(SLUICE_STACKFUL_HOST_MUTANT_WAKE_RETIRED_TASK)
    slot.await_link = nullptr;
#endif
    slot.fiber.make_done();
    fiber_ctx::context_switch_final(slot.fiber.ctx, host.driver_ctx_);
}

void StackfulIoHost::run_task_(TaskSlot& slot) {
    running_slot_ = &slot;
    if (!slot.fiber.make_running()) {
        detail::stackful_host_suspend_invariant_fail_fast();
    }
    fiber_ctx::Switch s;
    s.old = &driver_ctx_;
    s.new_ = &slot.fiber.ctx;
    (void)fiber_ctx::context_switch(&s);
    running_slot_ = nullptr;
}

void StackfulIoHost::suspend_current_(AwaitLink& link) {
    if (running_host_ != this || running_slot_ == nullptr) {
        detail::stackful_host_suspend_invariant_fail_fast();
    }
    TaskSlot& slot = *running_slot_;
    slot.await_link = &link;
    if (!slot.fiber.make_waiting()) {
        detail::stackful_host_suspend_invariant_fail_fast();
    }
    fiber_ctx::Switch s;
    s.old = &slot.fiber.ctx;
    s.new_ = &driver_ctx_;
    (void)fiber_ctx::context_switch(&s);
#if !defined(SLUICE_STACKFUL_HOST_MUTANT_WAKE_RETIRED_TASK)
    slot.await_link = nullptr;
#endif
}

void StackfulIoHost::wake_suspended_ready_() {
    for (std::size_t i = 0; i < task_capacity_; ++i) {
        TaskSlot& slot = slots_[i];
#if defined(SLUICE_STACKFUL_HOST_MUTANT_WAKE_RETIRED_TASK)
        if (slot.await_link == nullptr) {
            continue;
        }
#else
        if (!slot.live || slot.await_link == nullptr) {
            continue;
        }
#endif
        AwaitLink& link = *slot.await_link;
        if (!link.ready_fn(link.request)) {
            continue;
        }
        if (!slot.fiber.make_runnable()) {
            detail::stackful_host_suspend_invariant_fail_fast();
        }
        ready_ring_[(ring_head_ + ring_size_) % task_capacity_] = i;
        ++ring_size_;
#if defined(SLUICE_STACKFUL_HOST_MUTANT_DOUBLE_WAKE)
        ready_ring_[(ring_head_ + ring_size_) % task_capacity_] = i;
        ++ring_size_;
#endif
    }
}

std::chrono::nanoseconds StackfulIoHost::next_park_() const noexcept {
    // A deadline bounds the initial park window only: once it is past, it
    // stops bounding parks and the host waits unboundedly for natural
    // settlement. Expiry never cancels.
    const auto now = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point earliest =
        std::chrono::steady_clock::time_point::max();
    for (std::size_t i = 0; i < task_capacity_; ++i) {
        const TaskSlot& slot = slots_[i];
        if (!slot.live || slot.await_link == nullptr) {
            continue;
        }
        const AwaitLink& link = *slot.await_link;
        if (!link.has_deadline) {
            continue;
        }
#if defined(SLUICE_STACKFUL_HOST_MUTANT_EXPIRED_DEADLINE_SPIN)
        if (link.deadline <= now) {
            return std::chrono::nanoseconds::zero();
        }
#endif
        if (link.deadline <= now) {
            continue;
        }
        if (link.deadline < earliest) {
            earliest = link.deadline;
        }
    }
    if (earliest == std::chrono::steady_clock::time_point::max()) {
        return std::chrono::nanoseconds::max();
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(earliest - now);
}

void StackfulIoHost::record_task_error_(const IoError& error) noexcept {
    if (first_task_error_set_) {
        return;
    }
    first_task_error_set_ = true;
    first_task_error_ = error;
}

Result<void> StackfulIoHost::spawn(std::function<void(IoTaskContext&)> task) {
    if (!task) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }
    if (stop_requested()) {
        return make_unexpected_void(IoError{IoError::Code::canceled});
    }
    if (running_host_ != nullptr && running_host_ != this) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }

    TaskSlot* free_slot = nullptr;
    std::size_t free_index = 0;
    for (std::size_t i = 0; i < task_capacity_; ++i) {
        if (!slots_[i].live) {
            free_slot = &slots_[i];
            free_index = i;
            break;
        }
    }
    if (free_slot == nullptr) {
        return make_unexpected_void(IoError{IoError::Code::no_space});
    }

    free_slot->entry = std::move(task);
    if (!prepare_slot_fiber_(*free_slot)) {
        free_slot->entry = nullptr;
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }
    if (!free_slot->fiber.make_runnable()) {
        detail::stackful_host_suspend_invariant_fail_fast();
    }
    free_slot->live = true;
    ++live_tasks_;
    ready_ring_[(ring_head_ + ring_size_) % task_capacity_] = free_index;
    ++ring_size_;
    return {};
}

void StackfulIoHost::request_stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
    stop_token_.request();
#if defined(SLUICE_STACKFUL_HOST_MUTANT_STOP_INTERRUPTS_CONTROL)
    ctx_.interrupt_progress_waiters();
#endif
}

Result<void> StackfulIoHost::run() {
    if (running_host_ != nullptr) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }

#if !defined(SLUICE_STACKFUL_HOST_MUTANT_SECOND_OWNER_ALLOWED)
    auto owner = ctx_.claim_progress_owner();
    if (!owner.has_value()) {
        return make_unexpected_void(owner.error());
    }
#endif

#if !defined(SLUICE_STACKFUL_HOST_MUTANT_STALE_ERROR_RETAINED)
    first_task_error_set_ = false;
    first_task_error_ = {};
#endif

    struct RunningGuard {
        ~RunningGuard() {
            running_host_ = nullptr;
            running_slot_ = nullptr;
        }
    } running_guard;
    running_host_ = this;

    for (;;) {
        while (ring_size_ > 0) {
            const std::size_t index = ready_ring_[ring_head_];
            ring_head_ = (ring_head_ + 1) % task_capacity_;
            --ring_size_;
            TaskSlot& slot = slots_[index];
            run_task_(slot);
            if (slot.fiber.state() == FiberState::done) {
                slot.live = false;
                slot.entry = nullptr;
                --live_tasks_;
            } else if (slot.fiber.state() != FiberState::waiting) {
                detail::stackful_host_suspend_invariant_fail_fast();
            }
        }

        if (live_tasks_ == 0) {
            break;
        }

#if defined(SLUICE_STACKFUL_HOST_MUTANT_STOP_RETURNS_UNSETTLED)
        if (stop_requested()) {
            break;
        }
#endif

        auto pass = ctx_.poll_progress();
        if (!pass.has_value()) {
            return make_unexpected_void(pass.error());
        }
        if (pass.value().health_failed) {
            detail::stackful_host_drive_health_fail_fast();
        }

        wake_suspended_ready_();
        if (ring_size_ > 0) {
            continue;
        }

#if defined(SLUICE_STACKFUL_HOST_MUTANT_DEADLINE_CANCELS)
        for (std::size_t i = 0; i < task_capacity_; ++i) {
            TaskSlot& slot = slots_[i];
            if (!slot.live || slot.await_link == nullptr) {
                continue;
            }
            AwaitLink& link = *slot.await_link;
            if (!link.has_deadline ||
                std::chrono::steady_clock::now() < link.deadline) {
                continue;
            }
            link.cancel_fn(link.request);
        }
#endif

#if defined(SLUICE_STACKFUL_HOST_MUTANT_STOP_IMPLICIT_CANCEL)
        if (stop_requested()) {
            for (std::size_t i = 0; i < task_capacity_; ++i) {
                TaskSlot& slot = slots_[i];
                if (!slot.live || slot.await_link == nullptr) {
                    continue;
                }
                AwaitLink& link = *slot.await_link;
                link.cancel_fn(link.request);
            }
        }
#endif

        const std::chrono::nanoseconds park = next_park_();
        auto woke = park == std::chrono::nanoseconds::max() ? ctx_.wait_one()
                                                            : ctx_.wait_one(park);
        if (!woke.has_value()) {
            return make_unexpected_void(woke.error());
        }
        if (woke.value().kind == AsyncIoContext::ProgressWaitOutcome::Kind::health_failure) {
            detail::stackful_host_drive_health_fail_fast();
        }
        if (woke.value().kind == AsyncIoContext::ProgressWaitOutcome::Kind::control_interrupted) {
#if !defined(SLUICE_STACKFUL_HOST_MUTANT_SKIP_CONTROL_ACK)
            ctx_.acknowledge_progress_control();
#endif
        }
    }

    // A progress return can consume a control wake without reporting it, and
    // completions of other threads' requests may keep preceding the control
    // observation, so one bounded wait proves nothing: the release repeats
    // the zero-duration wait while passes keep reaping completions. The first
    // pass that reaps nothing is the release linearization point — either it
    // observed no control pending, or it observed the control (the latest
    // generation, atomically) and the acknowledge above retired it; control
    // arriving after that observation belongs to the next owner.
    for (;;) {
        auto release_control = ctx_.wait_one(std::chrono::nanoseconds::zero());
        if (!release_control.has_value()) {
            return make_unexpected_void(release_control.error());
        }
        if (release_control.value().kind ==
            AsyncIoContext::ProgressWaitOutcome::Kind::health_failure) {
            detail::stackful_host_drive_health_fail_fast();
        }
        if (release_control.value().kind == AsyncIoContext::ProgressWaitOutcome::Kind::progress &&
            release_control.value().completed > 0) {
            continue;
        }
        if (release_control.value().kind ==
            AsyncIoContext::ProgressWaitOutcome::Kind::control_interrupted) {
#if !defined(SLUICE_STACKFUL_HOST_MUTANT_SKIP_CONTROL_ACK)
            ctx_.acknowledge_progress_control();
#endif
        }
        break;
    }

    if (first_task_error_set_) {
        return make_unexpected_void(first_task_error_);
    }
    return {};
}

} // namespace sluice::async
