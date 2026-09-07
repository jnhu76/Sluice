















#include <sluice/async/async_io_context.hpp>

#include <sluice/async/detail/fail_fast.hpp>

#include <utility>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include <thread>
#endif

namespace sluice::async {

AsyncIoContext::AsyncIoContext(std::unique_ptr<AsyncBackend> backend, AsyncStats* stats)
    : backend_(std::move(backend)), stats_(stats) {
    if (backend_) backend_->attach_stats(stats_);
}

AsyncIoContext::~AsyncIoContext() {
    if (backend_ && backend_->outstanding() != 0) {







        detail::async_context_outstanding_fail_fast();
    }
}

AsyncIoContext::AsyncIoContext(AsyncIoContext&& other) noexcept
    : backend_(std::move(other.backend_)), stats_(other.stats_) {



}

AsyncIoContext& AsyncIoContext::operator=(AsyncIoContext&& other) noexcept {
    if (this != &other) {








        if (backend_ && backend_->outstanding() != 0) {
            detail::async_context_outstanding_fail_fast();
        }
        backend_ = std::move(other.backend_);
        stats_ = other.stats_;



    }
    return *this;
}






namespace detail {
bool tax0_f01_gate_outstanding_eval() noexcept;
}
using detail::tax0_f01_gate_outstanding_eval;

namespace {














void tally_submit(AsyncStats* s, const Result<void>& r) {
    if (!s) return;
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
    if (s && cur > s->max_outstanding) s->max_outstanding = cur;
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)







void tax0_f01_update_max_outstanding(AsyncStats* s, AsyncBackend& b) {
    if (tax0_f01_gate_outstanding_eval() && s == nullptr) return;
    update_max_outstanding(s, b.outstanding());
}
#else




void tax0_f01_update_max_outstanding(AsyncStats* s, AsyncBackend& b) {
    if (s == nullptr) return;
    update_max_outstanding(s, b.outstanding());
}
#endif
}

Result<void> AsyncIoContext::submit_read(ReadOp op, Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    auto r = backend_->submit_read(op, c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    return r;
}
Result<void> AsyncIoContext::submit_write(WriteOp op, Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    auto r = backend_->submit_write(op, c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    return r;
}
Result<void> AsyncIoContext::submit_sync_data(SyncDataOp op, Completion<void>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    auto r = backend_->submit_sync_data(op, c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    return r;
}
Result<void> AsyncIoContext::submit_sync_all(SyncAllOp op, Completion<void>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    auto r = backend_->submit_sync_all(op, c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    return r;
}






Result<RequestHandle> AsyncIoContext::submit_read_request(ReadOp op,
                                                          Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_->supports_request_identity())
        return make_unexpected<RequestHandle>(IoError{IoError::Code::not_supported});
    auto r = backend_->submit_read(op, c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value()) return make_unexpected<RequestHandle>(r.error());
    return backend_->identity_of(c);
}
Result<RequestHandle> AsyncIoContext::submit_write_request(WriteOp op,
                                                           Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_->supports_request_identity())
        return make_unexpected<RequestHandle>(IoError{IoError::Code::not_supported});
    auto r = backend_->submit_write(op, c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value()) return make_unexpected<RequestHandle>(r.error());
    return backend_->identity_of(c);
}
Result<RequestHandle> AsyncIoContext::submit_sync_data_request(SyncDataOp op,
                                                               Completion<void>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_->supports_request_identity())
        return make_unexpected<RequestHandle>(IoError{IoError::Code::not_supported});
    auto r = backend_->submit_sync_data(op, c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value()) return make_unexpected<RequestHandle>(r.error());
    return backend_->identity_of(c);
}
Result<RequestHandle> AsyncIoContext::submit_sync_all_request(SyncAllOp op,
                                                              Completion<void>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_->supports_request_identity())
        return make_unexpected<RequestHandle>(IoError{IoError::Code::not_supported});
    auto r = backend_->submit_sync_all(op, c);
    tally_submit(stats_, r);
    tax0_f01_update_max_outstanding(stats_, *backend_);
    if (!r.has_value()) return make_unexpected<RequestHandle>(r.error());
    return backend_->identity_of(c);
}


Result<RequestHandleState> AsyncIoContext::request_state(const RequestHandle& h) const {
    std::lock_guard<std::mutex> lk(access_mtx_);
    return backend_->request_handle_state(h);
}

std::size_t AsyncIoContext::poll() {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (stats_) ++stats_->poll_calls;
    std::size_t n = backend_->poll();
    if (stats_) stats_->completed_ops += n;
    return n;
}

Result<std::size_t> AsyncIoContext::wait_one() {


    return wait_one(std::chrono::nanoseconds::max());
}

Result<std::size_t> AsyncIoContext::wait_one(std::chrono::nanoseconds max_park) {














    BackendWaitSource* ws = backend_ ? backend_->wait_source() : nullptr;
    if (ws == nullptr) {











        (void)max_park;
        std::lock_guard<std::mutex> lk(access_mtx_);
        if (stats_) ++stats_->wait_calls;
        auto r = backend_->wait_one();
        if (r.has_value() && stats_) stats_->completed_ops += r.value();
        return r;
    }









    if (max_park != std::chrono::nanoseconds::max() &&
        !ws->supports_bounded_wait()) {
        return make_unexpected<std::size_t>(
            IoError{IoError::Code::not_supported});
    }



    {
        std::lock_guard<std::mutex> lk(access_mtx_);
        if (stats_) ++stats_->wait_calls;
    }






















    const BackendWaitToken invocation_start = ws->consume_committed_wait();
    const std::uint64_t control_baseline = invocation_start.control_generation;









    const bool bounded_park = max_park != std::chrono::nanoseconds::max();
    const auto park_deadline =
        bounded_park ? std::chrono::steady_clock::now() + max_park
                     : std::chrono::steady_clock::time_point{};
    for (;;) {
        BackendWaitToken token = ws->snapshot();




        token.control_generation = control_baseline;
        std::size_t n = 0;
        std::size_t outstanding_now = 0;
        {





            std::lock_guard<std::mutex> lk(access_mtx_);
            n = backend_->poll();
            outstanding_now = backend_->outstanding();
            if (n > 0 && stats_) stats_->completed_ops += n;
        }
        if (n > 0) {
            return Result<std::size_t>{n};
        }


        if (outstanding_now == 0) {
            return Result<std::size_t>{0};
        }









        BackendWakeReason reason;
        if (bounded_park) {
            auto remaining = park_deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::nanoseconds::zero()) {
                reason = BackendWakeReason::interrupted;
            } else {
                reason = ws->wait_for_change(token, remaining);
            }
        } else {
            reason = ws->wait_for_change(token);
        }
        if (reason == BackendWakeReason::progress) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)






            pause_after_wait_source_progress_();
#endif
            continue;
        }





        std::size_t final_n = 0;
        {
            std::lock_guard<std::mutex> lk(access_mtx_);
            final_n = backend_->poll();
            if (final_n > 0 && stats_) stats_->completed_ops += final_n;
        }
        if (final_n > 0) {
            return Result<std::size_t>{final_n};
        }
        return Result<std::size_t>{0};
    }
}

void AsyncIoContext::interrupt_backend_waiters() noexcept {













    if (backend_) {
        if (auto* ws = backend_->wait_source()) {
            ws->interrupt_all();
        }
    }
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
BackendWaitToken AsyncIoContext::backend_wait_token_for_test() const noexcept {




    if (backend_ != nullptr) {
        if (BackendWaitSource* ws = backend_->wait_source()) {
            return ws->snapshot();
        }
    }
    return BackendWaitToken{};
}
#endif

bool AsyncIoContext::has_split_wait_capability() const noexcept {




    return backend_ && backend_->wait_source() != nullptr;
}

bool AsyncIoContext::has_bounded_split_wait_capability() const noexcept {




    return backend_ && backend_->wait_source() != nullptr &&
           backend_->wait_source()->supports_bounded_wait();
}

void AsyncIoContext::arm_backend_wait_commit() noexcept {












    if (backend_) {
        if (auto* ws = backend_->wait_source()) {
            (void)ws->arm_committed_wait();
        }
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
    if (backend_) backend_->attach_ready_sink(sink);
}

Result<void> AsyncIoContext::register_waiter(Completion<std::size_t>& c,
                                             detail::WaiterToken token,
                                             detail::RoutingLease lease) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    return backend_->register_waiter(c, token, std::move(lease));
}
Result<void> AsyncIoContext::register_waiter(Completion<void>& c,
                                             detail::WaiterToken token,
                                             detail::RoutingLease lease) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    return backend_->register_waiter(c, token, std::move(lease));
}
Result<detail::RoutingLease> AsyncIoContext::cancel_waiter(Completion<std::size_t>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_) {
        return make_unexpected<detail::RoutingLease>(
            IoError{IoError::Code::invalid_state});
    }
    return backend_->cancel_waiter(c);
}
Result<detail::RoutingLease> AsyncIoContext::cancel_waiter(Completion<void>& c) {
    std::lock_guard<std::mutex> lk(access_mtx_);
    if (!backend_) {
        return make_unexpected<detail::RoutingLease>(
            IoError{IoError::Code::invalid_state});
    }
    return backend_->cancel_waiter(c);
}

std::size_t AsyncIoContext::outstanding() const noexcept {
    std::lock_guard<std::mutex> lk(access_mtx_);
    return backend_ ? backend_->outstanding() : 0;
}

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
void AsyncIoContext::pause_after_wait_source_progress_() noexcept {















    if (auto* g = wait_source_progress_gate_.load(std::memory_order_acquire)) {
        g->exited.store(false, std::memory_order_release);
        g->paused.store(true, std::memory_order_release);
        g->paused.notify_all();
        g->resume.wait(false, std::memory_order_acquire);
        g->exited.store(true, std::memory_order_release);
    }
}
#endif

}
