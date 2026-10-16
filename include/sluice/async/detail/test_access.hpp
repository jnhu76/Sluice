#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/async_backend.hpp>
#include <sluice/async/detail/progress_source.hpp>
#include <sluice/async/detail/request_core.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace sluice::async::detail {

// Isolated-test access to internal context and request state. Included only
// by seam tests; never installed, never included by production translation
// units.
struct TestAccess {
    static RequestCore* context_core(AsyncIoContext& ctx) noexcept { return ctx.core_.get(); }

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
