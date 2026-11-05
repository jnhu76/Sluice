#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/async_backend.hpp>
#include <sluice/async/detail/initiation_precheck.hpp>
#include <sluice/async/detail/progress_source.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/async/detail/uring_backend.hpp>
#include <sluice/detail/file_semantics.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>

namespace sluice::async::detail {

// Isolated-test access to internal context and request state. Included only
// by seam tests; never installed, never included by production translation
// units.
struct TestAccess {
    static RequestCore* context_core(AsyncIoContext& ctx) noexcept { return ctx.core_.get(); }

    static AsyncBackend* context_backend(AsyncIoContext& ctx) noexcept { return ctx.backend_.get(); }

    template <class T>
    static Request<T> make_request(AsyncIoContext& ctx, RequestKey key) noexcept {
        return Request<T>{ctx.core_.get(), ctx.backend_.get(), request_id_of(key)};
    }

    struct ReadyReadsBatch {
        bool batch_backend = false;
        std::size_t accepted = 0;
        std::size_t untouched_begin = 0;
        std::size_t untouched = 0;
        UringAsyncBackend::ReadyBatchStop stop = UringAsyncBackend::ReadyBatchStop::input_exhausted;
        IoError error{};
        std::array<Request<std::size_t>, UringAsyncBackend::ready_batch_cap> requests{};
        std::size_t flush_attempts = 0;
        bool residue_advertised = false;
    };

    // Lock order: access_mtx_ -> backend dispatch_mtx_ -> core mutex_.
    // Never call submit_read here: it would reacquire access_mtx_.
    static ReadyReadsBatch context_submit_ready_reads(AsyncIoContext& ctx,
                                                      std::span<const ReadOp> ready) noexcept {
        ReadyReadsBatch out;
        if (ready.empty())
            return out;

        std::size_t precheck_prefix = 0;
        std::optional<IoError> precheck_error;
        const std::size_t precheck_limit =
            std::min(ready.size(), UringAsyncBackend::ready_batch_cap);
        for (; precheck_prefix < precheck_limit; ++precheck_prefix) {
            if (auto rejection =
                    initiation_rejection(ready[precheck_prefix].file,
                                         sluice::detail::FileOperation::read);
                rejection.has_value()) {
                precheck_error = rejection;
                break;
            }
        }

        std::lock_guard<std::mutex> lk(ctx.access_mtx_);
        if (ctx.backend_ == nullptr) {
            out.stop = UringAsyncBackend::ReadyBatchStop::rejected;
            out.error = IoError{IoError::Code::invalid_state};
            out.untouched_begin = 1;
            out.untouched = ready.size() - 1;
            return out;
        }
        auto* backend = dynamic_cast<UringAsyncBackend*>(ctx.backend_.get());
        if (backend == nullptr) {
            out.stop = UringAsyncBackend::ReadyBatchStop::rejected;
            out.error = IoError{IoError::Code::not_supported};
            out.untouched_begin = 1;
            out.untouched = ready.size() - 1;
            return out;
        }
        out.batch_backend = true;

        if (precheck_prefix == 0) {
            out.stop = UringAsyncBackend::ReadyBatchStop::rejected;
            out.error = *precheck_error;
            out.untouched_begin = 1;
            out.untouched = ready.size() - 1;
            return out;
        }
        if (ctx.execution_closed_) {
            out.stop = UringAsyncBackend::ReadyBatchStop::rejected;
            out.error = IoError{IoError::Code::invalid_state};
            out.untouched_begin = 1;
            out.untouched = ready.size() - 1;
            return out;
        }

        const std::span<const ReadOp> batch_span =
            precheck_error.has_value() ? ready.first(precheck_prefix) : ready;
        const UringAsyncBackend::ReadyReadBatchReport report =
            backend->submit_ready_reads(batch_span);
        out.accepted = report.accepted;
        out.untouched_begin = report.untouched_begin;
        out.untouched = report.untouched;
        out.stop = report.stop;
        out.error = report.error;
        out.flush_attempts = report.flush_attempts;
        out.residue_advertised = report.residue_advertised;
        if (report.stop == UringAsyncBackend::ReadyBatchStop::recovery &&
            report.accepted == 0 && report.error.code == IoError::Code::backend_error &&
            !ctx.core_->health_failed() && !ctx.core_->admission_open()) {
            out.stop = UringAsyncBackend::ReadyBatchStop::rejected;
            out.error = IoError{IoError::Code::invalid_state};
            out.untouched_begin = 1;
            out.untouched = ready.size() - 1;
            return out;
        }
        for (std::size_t i = 0; i < report.accepted; ++i) {
            out.requests[i] = Request<std::size_t>{ctx.core_.get(), ctx.backend_.get(),
                                                   request_id_of(report.keys[i])};
        }
        if (precheck_error.has_value()) {
            if (report.accepted == precheck_prefix) {
                out.stop = UringAsyncBackend::ReadyBatchStop::rejected;
                out.error = *precheck_error;
                out.untouched_begin = precheck_prefix + 1;
            }
            out.untouched = ready.size() - out.untouched_begin;
        }
        return out;
    }

    static ProgressSource* context_progress_source(AsyncIoContext& ctx) noexcept {
        return ctx.progress_.get();
    }

    static ProgressSource::Token context_progress_token(const AsyncIoContext& ctx) noexcept {
        if (ctx.progress_ != nullptr) {
            return ctx.progress_->snapshot();
        }
        return ProgressSource::Token{};
    }

    static void context_set_ready_sink(AsyncIoContext& ctx,
                                       SynchronousReadySink* sink) noexcept {
        std::lock_guard<std::mutex> lk(ctx.access_mtx_);
        if (ctx.backend_ != nullptr) {
            ctx.backend_->attach_ready_sink(sink);
        }
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    static void context_set_settlement_start_pause_gate(
        AsyncIoContext& ctx, ProgressSource::PauseGate* gate) noexcept {
        if (ctx.progress_ != nullptr) {
            ctx.progress_->set_settlement_start_pause_gate_for_test(gate);
        }
    }

    static void context_set_progress_prepark_counter(AsyncIoContext& ctx,
                                                     std::atomic<int>* counter) noexcept {
        if (ctx.progress_ != nullptr) {
            ctx.progress_->set_prepark_counter_for_test(counter);
        }
    }

    static void context_set_prerevalidate_pause_gate(AsyncIoContext& ctx,
                                                     ProgressSource::PauseGate* gate) noexcept {
        if (ctx.progress_ != nullptr) {
            ctx.progress_->set_prerevalidate_pause_gate_for_test(gate);
        }
    }

    static void context_set_prepark_pause_gate(AsyncIoContext& ctx,
                                               ProgressSource::PauseGate* gate) noexcept {
        if (ctx.progress_ != nullptr) {
            ctx.progress_->set_prepark_pause_gate_for_test(gate);
        }
    }

    static void context_set_progress_epoch(AsyncIoContext& ctx, std::uint64_t epoch) noexcept {
        if (ctx.progress_ != nullptr) {
            ctx.progress_->set_progress_epoch_for_test(epoch);
        }
    }

    static void context_set_control_epoch(AsyncIoContext& ctx, std::uint64_t epoch) noexcept {
        if (ctx.progress_ != nullptr) {
            ctx.progress_->set_control_epoch_for_test(epoch);
        }
    }

    static void context_set_control_exhaustion(AsyncIoContext& ctx,
                                               std::uint64_t exhaustion) noexcept {
        if (ctx.progress_ != nullptr) {
            ctx.progress_->set_control_exhaustion_for_test(exhaustion);
        }
    }

    static void context_set_wait_health_failed(AsyncIoContext& ctx) noexcept {
        if (ctx.progress_ != nullptr) {
            ctx.progress_->set_wait_health_failed_for_test();
        }
    }

    static void context_saturate_progress_notification(AsyncIoContext& ctx) noexcept {
        if (ctx.progress_ != nullptr) {
            ctx.progress_->saturate_notification_for_test();
        }
    }

#endif

    static bool context_progress_exhausted(const AsyncIoContext& ctx) noexcept {
        return ctx.progress_ != nullptr && ctx.progress_->exhausted();
    }

    static RequestKey request_key_of(const RequestId& id) noexcept {
        return RequestKey{ContextIdentity{id.context_}, SlotIndex{id.slot_},
                          Generation{id.generation_}};
    }

    static RequestId request_id_of(const RequestKey& key) noexcept {
        return RequestId{key.context.value, key.slot.value, key.generation.value};
    }

    template <class T>
    static RequestKey request_key(const Request<T>& request) noexcept {
        return RequestKey{ContextIdentity{request.id_.context_},
                          SlotIndex{request.id_.slot_}, Generation{request.id_.generation_}};
    }
};

}
