#include <sluice/async/async_io_context.hpp>

#include <sluice/async/detail/context_identity.hpp>
#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/detail/file_semantics.hpp>

#include <optional>
#include <utility>

namespace sluice::async {

namespace {

// Retry cadence for a pass that left accepted transport outside the kernel
// (retryable submit failure). PROG-02 allows parking only with a further pass
// explicitly scheduled; the interval bounds the retry rate so a persistent
// EAGAIN/EBUSY cannot busy-spin the owner.
constexpr std::chrono::milliseconds kTransportRetryInterval{10};

std::optional<IoError> initiation_rejection(const NativeFileRef& file, sluice::detail::FileOperation op) {
    return sluice::detail::rejection_of(sluice::detail::precheck_state_op(file.fd < 0, file.access, op));
}

}

AsyncIoContext::AsyncIoContext(std::unique_ptr<AsyncBackend> backend, AsyncStats* stats)
    : progress_(std::make_unique<detail::ProgressSource>()), backend_(std::move(backend)),
      stats_(stats) {
    if (backend_) {
        backend_->attach_stats(stats_);
        backend_->attach_progress_port(detail::BackendProgressPort{progress_.get()});
    }
    const detail::ContextIdentity identity = detail::allocate_context_identity();
    const std::size_t slot_capacity = backend_ ? backend_->slot_capacity() : 0;
    core_ = std::make_unique<detail::RequestCore>(identity, slot_capacity);
    if (backend_) {
        backend_->core_ = core_.get();
    }
}

ProgressOwner::ProgressOwner(ProgressOwner&& other) noexcept
    : context_(other.context_), owner_thread_(other.owner_thread_) {
    other.context_ = nullptr;
    other.owner_thread_ = std::thread::id{};
}

ProgressOwner& ProgressOwner::operator=(ProgressOwner&& other) noexcept {
    if (this != &other) {
        if (context_ != nullptr) {
            std::lock_guard<std::mutex> lk(context_->access_mtx_);
            context_->owner_claimed_ = false;
            context_->owner_thread_ = std::thread::id{};
        }
        context_ = other.context_;
        owner_thread_ = other.owner_thread_;
        other.context_ = nullptr;
        other.owner_thread_ = std::thread::id{};
    }
    return *this;
}

ProgressOwner::~ProgressOwner() {
    if (context_ != nullptr) {
        std::lock_guard<std::mutex> lk(context_->access_mtx_);
        context_->owner_claimed_ = false;
        context_->owner_thread_ = std::thread::id{};
        context_ = nullptr;
    }
}

AsyncIoContext::~AsyncIoContext() {
    {
        std::lock_guard<std::mutex> lk(access_mtx_);
        fail_fast_if_progress_binding_live_();
    }
    if (core_ && core_->occupancy().public_bindings != 0) {
        detail::async_context_outstanding_fail_fast();
    }
    if (backend_ && backend_->outstanding() != 0) {
        detail::async_context_outstanding_fail_fast();
    }
}

AsyncIoContext::AsyncIoContext(AsyncIoContext&& other) noexcept
    : core_(std::move(other.core_)), progress_(std::move(other.progress_)),
      backend_(std::move(other.backend_)), stats_(other.stats_) {
    std::lock_guard<std::mutex> lk(other.access_mtx_);
    other.fail_fast_if_progress_binding_live_();
    other.drive_active_ = false;
    other.owner_claimed_ = false;
    other.owner_thread_ = std::thread::id{};
    other.notification_interest_ = NotificationInterest::none;
    other.stats_ = nullptr;
}

AsyncIoContext& AsyncIoContext::operator=(AsyncIoContext&& other) noexcept {
    if (this != &other) {
        {
            std::lock_guard<std::mutex> lk(access_mtx_);
            fail_fast_if_progress_binding_live_();
        }
        if (core_ && core_->occupancy().public_bindings != 0) {
            detail::async_context_outstanding_fail_fast();
        }
        if (backend_ && backend_->outstanding() != 0) {
            detail::async_context_outstanding_fail_fast();
        }
        std::lock_guard<std::mutex> lk(other.access_mtx_);
        other.fail_fast_if_progress_binding_live_();
        backend_ = std::move(other.backend_);
        core_ = std::move(other.core_);
        progress_ = std::move(other.progress_);
        stats_ = other.stats_;
        // The destination becomes the source's continuation: its retired
        // registration state travels with the notification source it
        // describes, and the source is left inert.
        notification_interest_ = other.notification_interest_;
        other.drive_active_ = false;
        other.owner_claimed_ = false;
        other.owner_thread_ = std::thread::id{};
        other.notification_interest_ = NotificationInterest::none;
        other.stats_ = nullptr;
    }
    return *this;
}

bool AsyncIoContext::drive_entry_admitted_() noexcept {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (drive_active_ || !backend_) {
        return false;
    }
    if (owner_claimed_ && owner_thread_ != std::this_thread::get_id()) {
        return false;
    }
    drive_active_ = true;
    return true;
}

void AsyncIoContext::drive_exit_() noexcept {
    std::lock_guard<std::mutex> lk(access_mtx_);
    drive_active_ = false;
}

void AsyncIoContext::fail_fast_if_progress_binding_live_() noexcept {
    if (drive_active_ || owner_claimed_ ||
        notification_interest_ == NotificationInterest::live) {
        detail::async_context_progress_binding_fail_fast();
    }
}

Result<ProgressOwner> AsyncIoContext::claim_progress_owner() {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_ || owner_claimed_) {
        return make_unexpected<ProgressOwner>(IoError{IoError::Code::invalid_state});
    }
    owner_claimed_ = true;
    owner_thread_ = std::this_thread::get_id();
    ProgressOwner owner;
    owner.context_ = this;
    owner.owner_thread_ = owner_thread_;
    return owner;
}

namespace detail {
bool tax0_f01_gate_outstanding_eval() noexcept;
}
using detail::tax0_f01_gate_outstanding_eval;

namespace {

template <class T>
void tally_submit(AsyncStats* s, const Result<T>& r) {
    if (!s)
        return;
    ++s->submit_calls;
    if (r.has_value()) {
        ++s->submitted_ops;
    } else if (r.error().code == IoError::Code::would_block) {
        ++s->queue_full_retries;
    } else if (r.error().code == IoError::Code::invalid_state) {
        ++s->invalid_state_rejections;
    }
}
void update_max_outstanding(AsyncStats* s, std::size_t cur) {
    if (s && cur > s->max_outstanding)
        s->max_outstanding = cur;
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

void tax0_f01_update_max_outstanding(AsyncStats* s, AsyncBackend& b) {
    if (tax0_f01_gate_outstanding_eval() && s == nullptr)
        return;
    update_max_outstanding(s, b.outstanding());
}
#else

void tax0_f01_update_max_outstanding(AsyncStats* s, AsyncBackend& b) {
    if (s == nullptr)
        return;
    update_max_outstanding(s, b.outstanding());
}
#endif
}

Result<void> AsyncIoContext::submit_read(ReadOp op, Completion<std::size_t>& c) {
    if (auto rejection = initiation_rejection(op.file, sluice::detail::FileOperation::read);
        rejection.has_value()) {
        return make_unexpected<void>(*rejection);
    }
    std::lock_guard<std::mutex> lk(access_mtx_);
    auto r = backend_->submit_read(op, &c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value())
        return make_unexpected<void>(r.error());
    return {};
}
Result<void> AsyncIoContext::submit_write(WriteOp op, Completion<std::size_t>& c) {
    if (auto rejection = initiation_rejection(op.file, sluice::detail::FileOperation::write);
        rejection.has_value()) {
        return make_unexpected<void>(*rejection);
    }
    std::lock_guard<std::mutex> lk(access_mtx_);
    auto r = backend_->submit_write(op, &c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value())
        return make_unexpected<void>(r.error());
    return {};
}
Result<void> AsyncIoContext::submit_sync_data(SyncDataOp op, Completion<void>& c) {
    if (auto rejection = initiation_rejection(op.file, sluice::detail::FileOperation::sync_data);
        rejection.has_value()) {
        return make_unexpected<void>(*rejection);
    }
    std::lock_guard<std::mutex> lk(access_mtx_);
    auto r = backend_->submit_sync_data(op, &c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value())
        return make_unexpected<void>(r.error());
    return {};
}
Result<void> AsyncIoContext::submit_sync_all(SyncAllOp op, Completion<void>& c) {
    if (auto rejection = initiation_rejection(op.file, sluice::detail::FileOperation::sync_all);
        rejection.has_value()) {
        return make_unexpected<void>(*rejection);
    }
    std::lock_guard<std::mutex> lk(access_mtx_);
    auto r = backend_->submit_sync_all(op, &c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value())
        return make_unexpected<void>(r.error());
    return {};
}

Result<Request<std::size_t>> AsyncIoContext::submit_read(ReadOp op) {
    if (auto rejection = initiation_rejection(op.file, sluice::detail::FileOperation::read);
        rejection.has_value()) {
        return make_unexpected<Request<std::size_t>>(*rejection);
    }
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_)
        return make_unexpected<Request<std::size_t>>(IoError{IoError::Code::invalid_state});
    auto r = backend_->submit_read(op, nullptr);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value())
        return make_unexpected<Request<std::size_t>>(r.error());
    return Request<std::size_t>{core_.get(), backend_.get(), r.value()};
}

Result<Request<std::size_t>> AsyncIoContext::submit_write(WriteOp op) {
    if (auto rejection = initiation_rejection(op.file, sluice::detail::FileOperation::write);
        rejection.has_value()) {
        return make_unexpected<Request<std::size_t>>(*rejection);
    }
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_)
        return make_unexpected<Request<std::size_t>>(IoError{IoError::Code::invalid_state});
    auto r = backend_->submit_write(op, nullptr);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value())
        return make_unexpected<Request<std::size_t>>(r.error());
    return Request<std::size_t>{core_.get(), backend_.get(), r.value()};
}

Result<Request<void>> AsyncIoContext::submit_sync_data(SyncDataOp op) {
    if (auto rejection = initiation_rejection(op.file, sluice::detail::FileOperation::sync_data);
        rejection.has_value()) {
        return make_unexpected<Request<void>>(*rejection);
    }
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_)
        return make_unexpected<Request<void>>(IoError{IoError::Code::invalid_state});
    auto r = backend_->submit_sync_data(op, nullptr);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value())
        return make_unexpected<Request<void>>(r.error());
    return Request<void>{core_.get(), backend_.get(), r.value()};
}

Result<Request<void>> AsyncIoContext::submit_sync_all(SyncAllOp op) {
    if (auto rejection = initiation_rejection(op.file, sluice::detail::FileOperation::sync_all);
        rejection.has_value()) {
        return make_unexpected<Request<void>>(*rejection);
    }
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_)
        return make_unexpected<Request<void>>(IoError{IoError::Code::invalid_state});
    auto r = backend_->submit_sync_all(op, nullptr);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value())
        return make_unexpected<Request<void>>(r.error());
    return Request<void>{core_.get(), backend_.get(), r.value()};
}

Result<RequestHandle> AsyncIoContext::submit_read_request(ReadOp op, Completion<std::size_t>& c) {
    if (auto rejection = initiation_rejection(op.file, sluice::detail::FileOperation::read);
        rejection.has_value()) {
        return make_unexpected<RequestHandle>(*rejection);
    }
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_->supports_request_identity())
        return make_unexpected<RequestHandle>(IoError{IoError::Code::not_supported});
    auto r = backend_->submit_read(op, &c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value())
        return make_unexpected<RequestHandle>(r.error());
    return backend_->identity_of(c);
}
Result<RequestHandle> AsyncIoContext::submit_write_request(WriteOp op, Completion<std::size_t>& c) {
    if (auto rejection = initiation_rejection(op.file, sluice::detail::FileOperation::write);
        rejection.has_value()) {
        return make_unexpected<RequestHandle>(*rejection);
    }
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_->supports_request_identity())
        return make_unexpected<RequestHandle>(IoError{IoError::Code::not_supported});
    auto r = backend_->submit_write(op, &c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value())
        return make_unexpected<RequestHandle>(r.error());
    return backend_->identity_of(c);
}
Result<RequestHandle> AsyncIoContext::submit_sync_data_request(SyncDataOp op, Completion<void>& c) {
    if (auto rejection = initiation_rejection(op.file, sluice::detail::FileOperation::sync_data);
        rejection.has_value()) {
        return make_unexpected<RequestHandle>(*rejection);
    }
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_->supports_request_identity())
        return make_unexpected<RequestHandle>(IoError{IoError::Code::not_supported});
    auto r = backend_->submit_sync_data(op, &c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value())
        return make_unexpected<RequestHandle>(r.error());
    return backend_->identity_of(c);
}
Result<RequestHandle> AsyncIoContext::submit_sync_all_request(SyncAllOp op, Completion<void>& c) {
    if (auto rejection = initiation_rejection(op.file, sluice::detail::FileOperation::sync_all);
        rejection.has_value()) {
        return make_unexpected<RequestHandle>(*rejection);
    }
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_->supports_request_identity())
        return make_unexpected<RequestHandle>(IoError{IoError::Code::not_supported});
    auto r = backend_->submit_sync_all(op, &c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value())
        return make_unexpected<RequestHandle>(r.error());
    return backend_->identity_of(c);
}

Result<CancelDisposition> AsyncIoContext::cancel(const RequestId& id) {
    if (!id.valid()) {
        return make_unexpected<CancelDisposition>(IoError{IoError::Code::invalid_state});
    }
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_) {
        return make_unexpected<CancelDisposition>(IoError{IoError::Code::invalid_state});
    }
    const detail::RequestKey key{detail::ContextIdentity{id.context_},
                                 detail::SlotIndex{id.slot_},
                                 detail::Generation{id.generation_}};
    switch (backend_->cancel_identity(key)) {
    case detail::PublicCancel::won_before_execution:
        return CancelDisposition::won_before_execution;
    case detail::PublicCancel::requested:
        return CancelDisposition::requested;
    case detail::PublicCancel::already_terminal:
        return CancelDisposition::already_terminal;
    case detail::PublicCancel::not_found:
        return CancelDisposition::not_found;
    }
    return CancelDisposition::not_found;
}

RequestReadiness AsyncIoContext::lookup(const RequestId& id) const {
    if (!id.valid()) {
        return RequestReadiness::empty;
    }
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!core_) {
        return RequestReadiness::empty;
    }
    const detail::RequestKey key{detail::ContextIdentity{id.context_},
                                 detail::SlotIndex{id.slot_},
                                 detail::Generation{id.generation_}};
    switch (core_->lookup(key)) {
    case detail::PublicLookup::outstanding:
        return RequestReadiness::pending;
    case detail::PublicLookup::published:
        return RequestReadiness::ready;
    case detail::PublicLookup::not_found:
        return RequestReadiness::empty;
    }
    return RequestReadiness::empty;
}

Result<RequestHandleState> AsyncIoContext::request_state(const RequestHandle& h) const {
    std::lock_guard<std::mutex> lk(access_mtx_);
    return backend_->request_handle_state(h);
}

Result<std::size_t> AsyncIoContext::poll() {
    auto pass = poll_progress();
    if (!pass.has_value()) {
        return make_unexpected<std::size_t>(pass.error());
    }
    return pass.value().completed;
}

AsyncBackend::ProgressPass AsyncIoContext::run_progress_pass_() {
    if (stats_)
        ++stats_->poll_calls;
    AsyncBackend::ProgressPass pass;
    if (!backend_)
        return pass;
    pass = backend_->poll_progress();
    if (stats_)
        stats_->completed_ops += pass.completed;
    close_admission_on_progress_exhaustion_();
    return pass;
}

Result<AsyncIoContext::ProgressPass> AsyncIoContext::poll_progress() {
    if (!drive_entry_admitted_()) {
        return make_unexpected<ProgressPass>(IoError{IoError::Code::invalid_state});
    }
    DriveGuard guard(this);
    return run_progress_pass_();
}

void AsyncIoContext::close_admission_on_progress_exhaustion_() noexcept {
    if (core_ && progress_ && progress_->exhausted()) {
        core_->close_admission();
    }
}

bool AsyncIoContext::backend_has_immediate_physical_work_() noexcept {
    return backend_ && backend_->has_immediate_physical_work();
}

Result<AsyncIoContext::ProgressWaitOutcome> AsyncIoContext::wait_one() {
    return wait_one(std::chrono::nanoseconds::max());
}

Result<AsyncIoContext::ProgressWaitOutcome> AsyncIoContext::wait_one(
    std::chrono::nanoseconds max_park) {
    if (backend_ == nullptr) {
        return make_unexpected<ProgressWaitOutcome>(IoError{IoError::Code::invalid_state});
    }
    if (!backend_->signals_physical_progress()) {
        return make_unexpected<ProgressWaitOutcome>(IoError{IoError::Code::not_supported});
    }

    {
        std::lock_guard<std::mutex> lk(access_mtx_);
        if (stats_)
            ++stats_->wait_calls;
    }

    if (!drive_entry_admitted_()) {
        return make_unexpected<ProgressWaitOutcome>(IoError{IoError::Code::invalid_state});
    }
    DriveGuard guard(this);

    if (progress_->wait_health_failed()) {
        return ProgressWaitOutcome{ProgressWaitOutcome::Kind::health_failure, 0};
    }
    if (progress_->control_pending()) {
        return ProgressWaitOutcome{ProgressWaitOutcome::Kind::control_interrupted, 0};
    }

    const detail::ProgressSource::Token invocation_start = progress_->consume_committed_wait();

    const bool bounded_park = max_park != std::chrono::nanoseconds::max();
    const auto park_deadline = bounded_park ? std::chrono::steady_clock::now() + max_park
                                            : std::chrono::steady_clock::time_point{};
    for (;;) {
        const detail::ProgressSource::Token token = progress_->snapshot();

        const AsyncBackend::ProgressPass pass = run_progress_pass_();
        if (pass.completed > 0) {
            return ProgressWaitOutcome{ProgressWaitOutcome::Kind::progress, pass.completed};
        }

        if (!pass.immediate_work_remains && !pass.accepted_work_remains &&
            !pass.dispatch_retry_remains) {
            return ProgressWaitOutcome{ProgressWaitOutcome::Kind::progress, 0};
        }

        detail::ProgressSource::WakeReason reason;
        auto probe = [](void* self) noexcept -> bool {
            return static_cast<AsyncIoContext*>(self)->backend_has_immediate_physical_work_();
        };

        // A pass that left accepted transport outside the kernel schedules a
        // further bounded pass instead of parking past the dispatch
        // obligation; nothing will write the kernel eventfd for that work.
        std::chrono::steady_clock::time_point pass_deadline = park_deadline;
        bool timed_wait = bounded_park;
        if (pass.dispatch_retry_remains) {
            const auto retry_by = std::chrono::steady_clock::now() + kTransportRetryInterval;
            if (!timed_wait || retry_by < pass_deadline) {
                pass_deadline = retry_by;
                timed_wait = true;
            }
        }

        detail::ProgressSource::Token observed = token;
        observed.control = invocation_start.control;
        observed.control_exhaustion = invocation_start.control_exhaustion;
        if (!timed_wait) {
            reason = progress_->wait_if_unchanged(observed, std::chrono::nanoseconds::max(), probe,
                                                  this);
        } else {
            auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
                pass_deadline - std::chrono::steady_clock::now());
            if (remaining < std::chrono::nanoseconds::zero()) {
                remaining = std::chrono::nanoseconds::zero();
            }
            reason = progress_->wait_if_unchanged(observed, remaining, probe, this);
        }

        switch (reason) {
        case detail::ProgressSource::WakeReason::progress:
            continue;
        case detail::ProgressSource::WakeReason::interrupted:
            // Sticky control is the only interrupt that outlives an
            // acknowledgement; a stale committed token after an already
            // retired control is a spurious wake, not a reportable outcome.
            if (progress_->control_pending()) {
                return ProgressWaitOutcome{ProgressWaitOutcome::Kind::control_interrupted, 0};
            }
            continue;
        case detail::ProgressSource::WakeReason::deadline:
            // The dispatch-retry nap expiring is an internal scheduling event
            // that only schedules another bounded pass; only the caller's own
            // deadline produces deadline_expired.
            if (timed_wait && pass_deadline != park_deadline) {
                continue;
            }
            {
                const AsyncBackend::ProgressPass final_pass = run_progress_pass_();
                if (final_pass.completed > 0) {
                    return ProgressWaitOutcome{ProgressWaitOutcome::Kind::progress,
                                               final_pass.completed};
                }
                return ProgressWaitOutcome{ProgressWaitOutcome::Kind::deadline_expired, 0};
            }
        case detail::ProgressSource::WakeReason::failed:
            return ProgressWaitOutcome{ProgressWaitOutcome::Kind::health_failure, 0};
        }
        return make_unexpected<ProgressWaitOutcome>(IoError{IoError::Code::invalid_state});
    }
}

void AsyncIoContext::interrupt_progress_waiters() noexcept {
    if (progress_) {
        progress_->interrupt();
    }
}

void AsyncIoContext::acknowledge_progress_control() {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (progress_ == nullptr) {
        return;
    }
    if (owner_claimed_ && owner_thread_ != std::this_thread::get_id()) {
        detail::async_progress_acknowledgement_fail_fast();
    }
    progress_->acknowledge_control();
}

bool AsyncIoContext::progress_control_pending() const noexcept {
    return progress_ != nullptr && progress_->control_pending();
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
detail::ProgressSource::Token AsyncIoContext::progress_token_for_test() const noexcept {
    if (progress_ != nullptr) {
        return progress_->snapshot();
    }
    return detail::ProgressSource::Token{};
}

void AsyncIoContext::set_progress_prepark_counter_for_test(std::atomic<int>* counter) noexcept {
    if (progress_ != nullptr) {
        progress_->set_prepark_counter_for_test(counter);
    }
}

void AsyncIoContext::set_progress_prerevalidate_pause_gate_for_test(
    detail::ProgressSource::PauseGate* gate) noexcept {
    if (progress_ != nullptr) {
        progress_->set_prerevalidate_pause_gate_for_test(gate);
    }
}

void AsyncIoContext::set_progress_prepark_pause_gate_for_test(
    detail::ProgressSource::PauseGate* gate) noexcept {
    if (progress_ != nullptr) {
        progress_->set_prepark_pause_gate_for_test(gate);
    }
}

void AsyncIoContext::set_progress_epoch_for_test(std::uint64_t epoch) noexcept {
    if (progress_ != nullptr) {
        progress_->set_progress_epoch_for_test(epoch);
    }
}

void AsyncIoContext::set_control_epoch_for_test(std::uint64_t epoch) noexcept {
    if (progress_ != nullptr) {
        progress_->set_control_epoch_for_test(epoch);
    }
}

void AsyncIoContext::set_control_exhaustion_for_test(std::uint64_t exhaustion) noexcept {
    if (progress_ != nullptr) {
        progress_->set_control_exhaustion_for_test(exhaustion);
    }
}

void AsyncIoContext::set_wait_health_failed_for_test() noexcept {
    if (progress_ != nullptr) {
        progress_->set_wait_health_failed_for_test();
    }
}

void AsyncIoContext::saturate_progress_notification_for_test() noexcept {
    if (progress_ != nullptr) {
        progress_->saturate_notification_for_test();
    }
}

bool AsyncIoContext::progress_exhausted_for_test() const noexcept {
    return progress_ != nullptr && progress_->exhausted();
}
#endif

bool AsyncIoContext::has_split_wait_capability() const noexcept {
    return backend_ && backend_->signals_physical_progress();
}

bool AsyncIoContext::has_bounded_split_wait_capability() const noexcept {
    return has_split_wait_capability();
}

void AsyncIoContext::arm_progress_wait_commit() noexcept {
    if (progress_) {
        (void)progress_->arm_committed_wait();
    }
}

int AsyncIoContext::progress_notification_fd() noexcept {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (progress_ == nullptr || notification_interest_ == NotificationInterest::detached) {
        return -1;
    }
    notification_interest_ = NotificationInterest::live;
    return progress_->notification_fd();
}

void AsyncIoContext::acknowledge_progress_notification() noexcept {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (progress_ == nullptr) {
        return;
    }
    if (notification_interest_ != NotificationInterest::live ||
        (owner_claimed_ && owner_thread_ != std::this_thread::get_id())) {
        detail::async_progress_acknowledgement_fail_fast();
    }
    progress_->acknowledge_notification();
}

int AsyncIoContext::detach_progress_host() noexcept {
    std::lock_guard<std::mutex> lk(access_mtx_);
    const bool was_live = notification_interest_ == NotificationInterest::live;
    notification_interest_ = NotificationInterest::detached;
    if (!was_live || progress_ == nullptr) {
        return -1;
    }
    return progress_->notification_fd();
}

void AsyncIoContext::cancel(Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    backend_->cancel(c);
}
void AsyncIoContext::cancel(Completion<void>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    backend_->cancel(c);
}

void AsyncIoContext::set_ready_sink(detail::SynchronousReadySink* sink) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (backend_)
        backend_->attach_ready_sink(sink);
}

AsyncIoContext::ObserverAttachment AsyncIoContext::attach_observer(Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_) {
        return {};
    }
    const RequestHandle identity = backend_->identity_of(c);
    if (!identity.valid()) {
        return {};
    }
    const detail::RequestKey key{detail::ContextIdentity{identity.context_},
                                 detail::SlotIndex{identity.slot_},
                                 detail::Generation{identity.generation_}};
    return {core_->register_observer(key), key};
}

AsyncIoContext::ObserverAttachment AsyncIoContext::attach_observer(Completion<void>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_) {
        return {};
    }
    const RequestHandle identity = backend_->identity_of(c);
    if (!identity.valid()) {
        return {};
    }
    const detail::RequestKey key{detail::ContextIdentity{identity.context_},
                                 detail::SlotIndex{identity.slot_},
                                 detail::Generation{identity.generation_}};
    return {core_->register_observer(key), key};
}

AsyncIoContext::ObserverCancelResult AsyncIoContext::cancel_observer(Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_) {
        return {};
    }
    const RequestHandle identity = backend_->identity_of(c);
    if (!identity.valid()) {
        return {};
    }
    const detail::RequestKey key{detail::ContextIdentity{identity.context_},
                                 detail::SlotIndex{identity.slot_},
                                 detail::Generation{identity.generation_}};
    return {core_->cancel_observer(key), key};
}

AsyncIoContext::ObserverCancelResult AsyncIoContext::cancel_observer(Completion<void>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_) {
        return {};
    }
    const RequestHandle identity = backend_->identity_of(c);
    if (!identity.valid()) {
        return {};
    }
    const detail::RequestKey key{detail::ContextIdentity{identity.context_},
                                 detail::SlotIndex{identity.slot_},
                                 detail::Generation{identity.generation_}};
    return {core_->cancel_observer(key), key};
}

bool AsyncIoContext::retire_delivery(detail::RequestKey key) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    return core_->retire_observer_delivery(key) == detail::ObserverDeliveryRetirement::retired;
}

std::size_t AsyncIoContext::outstanding() const noexcept {
    std::lock_guard<std::mutex> lk(access_mtx_);
    return backend_ ? backend_->outstanding() : 0;
}

}
