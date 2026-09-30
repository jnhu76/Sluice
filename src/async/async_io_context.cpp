#include <sluice/async/async_io_context.hpp>

#include <sluice/async/detail/context_identity.hpp>
#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/detail/file_semantics.hpp>

#include <optional>
#include <utility>

namespace sluice::async {

namespace {

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

AsyncIoContext::~AsyncIoContext() {
    if (core_ && core_->occupancy().public_bindings != 0) {
        detail::async_context_outstanding_fail_fast();
    }
    if (backend_ && backend_->outstanding() != 0) {
        detail::async_context_outstanding_fail_fast();
    }
}

AsyncIoContext::AsyncIoContext(AsyncIoContext&& other) noexcept
    : core_(std::move(other.core_)), progress_(std::move(other.progress_)),
      backend_(std::move(other.backend_)), stats_(other.stats_) {}

AsyncIoContext& AsyncIoContext::operator=(AsyncIoContext&& other) noexcept {
    if (this != &other) {
        if (core_ && core_->occupancy().public_bindings != 0) {
            detail::async_context_outstanding_fail_fast();
        }
        if (backend_ && backend_->outstanding() != 0) {
            detail::async_context_outstanding_fail_fast();
        }
        backend_ = std::move(other.backend_);
        core_ = std::move(other.core_);
        progress_ = std::move(other.progress_);
        stats_ = other.stats_;
    }
    return *this;
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

std::size_t AsyncIoContext::poll() {
    return poll_progress().completed;
}

AsyncBackend::ProgressPass AsyncIoContext::poll_progress() {
    std::lock_guard<std::mutex> lk(access_mtx_);
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

void AsyncIoContext::close_admission_on_progress_exhaustion_() noexcept {
    if (core_ && progress_ && progress_->exhausted()) {
        core_->close_admission();
    }
}

Result<std::size_t> AsyncIoContext::wait_one() {
    return wait_one(std::chrono::nanoseconds::max());
}

Result<std::size_t> AsyncIoContext::wait_one(std::chrono::nanoseconds max_park) {
    if (backend_ == nullptr) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
    }
    if (!backend_->signals_physical_progress()) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::not_supported});
    }

    {
        std::lock_guard<std::mutex> lk(access_mtx_);
        if (stats_)
            ++stats_->wait_calls;
    }

    const detail::ProgressSource::Token invocation_start = progress_->consume_committed_wait();

    const bool bounded_park = max_park != std::chrono::nanoseconds::max();
    const auto park_deadline = bounded_park ? std::chrono::steady_clock::now() + max_park
                                            : std::chrono::steady_clock::time_point{};
    for (;;) {
        const detail::ProgressSource::Token token = progress_->snapshot();

        const AsyncBackend::ProgressPass pass = poll_progress();
        if (pass.completed > 0) {
            return Result<std::size_t>{pass.completed};
        }

        if (!pass.immediate_work_remains && !pass.accepted_work_remains) {
            return Result<std::size_t>{0};
        }

        detail::ProgressSource::WakeReason reason;
        if (bounded_park) {
            auto remaining = park_deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::nanoseconds::zero()) {
                reason = detail::ProgressSource::WakeReason::interrupted;
            } else {
                detail::ProgressSource::Token observed = token;
                observed.control = invocation_start.control;
                observed.control_exhaustion = invocation_start.control_exhaustion;
                reason = progress_->wait_if_unchanged(observed, remaining);
            }
        } else {
            detail::ProgressSource::Token observed = token;
            observed.control = invocation_start.control;
            observed.control_exhaustion = invocation_start.control_exhaustion;
            reason = progress_->wait_if_unchanged(observed);
        }
        if (reason == detail::ProgressSource::WakeReason::progress) {
            continue;
        }

        const AsyncBackend::ProgressPass final_pass = poll_progress();
        if (final_pass.completed > 0) {
            return Result<std::size_t>{final_pass.completed};
        }
        return Result<std::size_t>{0};
    }
}

void AsyncIoContext::interrupt_progress_waiters() noexcept {
    if (progress_) {
        progress_->interrupt();
    }
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

int AsyncIoContext::progress_notification_fd() const noexcept {
    return progress_ ? progress_->notification_fd() : -1;
}

void AsyncIoContext::acknowledge_progress_notification() noexcept {
    if (progress_) {
        progress_->acknowledge_notification();
    }
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
