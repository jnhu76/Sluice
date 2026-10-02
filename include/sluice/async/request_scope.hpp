#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace sluice::async {

enum class ScopeCleanupPolicy : std::uint8_t { drain, cancel_then_drain };

enum class ScopeWaitStatus : std::uint8_t { ready, timeout, interrupted, health_failure };

template <class T>
class ScopeTicket {
  public:
    constexpr ScopeTicket() noexcept = default;

    constexpr RequestId id() const noexcept { return id_; }

    constexpr bool valid() const noexcept { return id_.valid(); }

  private:
    friend class RequestScope;

    constexpr explicit ScopeTicket(RequestId id) noexcept : id_(id) {}

    RequestId id_{};
};

class RequestScope {
  public:
    RequestScope(AsyncIoContext& ctx, std::size_t capacity, ScopeCleanupPolicy policy);

    RequestScope(const RequestScope&) = delete;
    RequestScope& operator=(const RequestScope&) = delete;

    ~RequestScope();

    std::size_t capacity() const noexcept { return capacity_; }

    Result<ScopeTicket<std::size_t>> submit_read(ReadOp op);
    Result<ScopeTicket<std::size_t>> submit_write(WriteOp op);
    Result<ScopeTicket<void>> submit_sync_data(SyncDataOp op);
    Result<ScopeTicket<void>> submit_sync_all(SyncAllOp op);

    RequestObservation<std::size_t> take(const ScopeTicket<std::size_t>& ticket) noexcept;
    RequestObservation<void> take(const ScopeTicket<void>& ticket) noexcept;

    Result<ScopeWaitStatus> wait_for(const ScopeTicket<std::size_t>& ticket,
                                     std::chrono::nanoseconds max_wait) noexcept;
    Result<ScopeWaitStatus> wait_for(const ScopeTicket<void>& ticket,
                                     std::chrono::nanoseconds max_wait) noexcept;

    void finish() noexcept;

  private:
    struct Slot {
        enum class State : std::uint8_t { free, reserved, owning };

        State state = State::free;
        std::variant<std::monostate, Request<std::size_t>, Request<void>> request;
    };

    struct Reservation {
        RequestScope* scope = nullptr;
        std::size_t index = 0;
        bool committed = false;

        Reservation(RequestScope* s, std::size_t i) noexcept : scope(s), index(i) {}
        ~Reservation() {
            if (!committed) {
                scope->release_slot_(scope->slots_[index]);
            }
        }
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
    };

    static std::size_t require_capacity_(std::size_t capacity);
    static ProgressOwner claim_owner_(AsyncIoContext& ctx);

    template <class T>
    static constexpr std::size_t slot_index_() noexcept {
        return std::is_void_v<T> ? 2 : 1;
    }

    bool reserve_slot_(std::size_t& index) noexcept;

    template <class T>
    void commit_(std::size_t index, Request<T>&& request) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<Request<T>>);
#if defined(SLUICE_D1_MUTANT_COMMIT_DROPS_REQUEST)
        (void)index;
#else
        Slot& slot = slots_[index];
        slot.request.template emplace<slot_index_<T>()>(std::move(request));
        slot.state = Slot::State::owning;
#endif
    }

    template <class T>
    Result<ScopeTicket<T>>
    commit_tracked_(std::size_t index, Result<Request<T>>&& accepted) {
        Reservation guard{this, index};
        if (!accepted.has_value()) {
            return make_unexpected<ScopeTicket<T>>(accepted.error());
        }
        const RequestId id = accepted.value().id();
        commit_<T>(index, std::move(accepted).value());
        guard.committed = true;
        return ScopeTicket<T>{id};
    }

#if defined(SLUICE_D1_MUTANT_RESERVE_AFTER_ACCEPT)
    template <class T>
    Result<ScopeTicket<T>> commit_tracked_(Result<Request<T>>&& accepted) {
        if (!accepted.has_value()) {
            return make_unexpected<ScopeTicket<T>>(accepted.error());
        }
        std::size_t index = 0;
        if (!reserve_slot_(index)) {
            return make_unexpected<ScopeTicket<T>>(IoError{IoError::Code::would_block});
        }
        return commit_tracked_<T>(index, std::move(accepted));
    }
#endif

    void release_slot_(Slot& slot) noexcept;

    template <class T>
    Slot* resolve_(const ScopeTicket<T>& ticket) noexcept {
        if (!ticket.valid()) {
            return nullptr;
        }
        for (std::size_t i = 0; i < capacity_; ++i) {
            Slot& slot = slots_[i];
            if (slot.state != Slot::State::owning) {
                continue;
            }
            Request<T>* request = std::get_if<Request<T>>(&slot.request);
            if (request != nullptr && request->valid() && request->id() == ticket.id()) {
                return &slot;
            }
        }
        return nullptr;
    }

    template <class T>
    RequestObservation<T> take_(const ScopeTicket<T>& ticket) noexcept {
        Slot* slot = resolve_<T>(ticket);
        if (slot == nullptr) {
            return {};
        }
        RequestObservation<T> observed =
            std::get<slot_index_<T>()>(slot->request).take_result();
        if (observed.readiness == RequestReadiness::ready) {
            release_slot_(*slot);
        }
        return observed;
    }

    template <class T>
    Result<ScopeWaitStatus> wait_for_slot_(Slot& slot,
                                           std::chrono::nanoseconds max_wait) noexcept {
        const bool bounded = max_wait != std::chrono::nanoseconds::max();
        const auto deadline = bounded ? std::chrono::steady_clock::now() + max_wait
                                      : std::chrono::steady_clock::time_point{};
        for (;;) {
            if (std::get<slot_index_<T>()>(slot.request).ready()) {
                return ScopeWaitStatus::ready;
            }
            if (bounded) {
                const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    deadline - std::chrono::steady_clock::now());
                if (remaining.count() <= 0) {
#if defined(SLUICE_D1_MUTANT_TIMEOUT_RELEASES_SLOT)
                    release_slot_(slot);
#endif
                    return ScopeWaitStatus::timeout;
                }
            }
            auto woke = bounded ? ctx_.wait_one(deadline - std::chrono::steady_clock::now())
                                : ctx_.wait_one();
            if (!woke.has_value()) {
                return make_unexpected<ScopeWaitStatus>(woke.error());
            }
            switch (woke.value().kind) {
            case AsyncIoContext::ProgressWaitOutcome::Kind::progress:
                break;
            case AsyncIoContext::ProgressWaitOutcome::Kind::control_interrupted:
                return ScopeWaitStatus::interrupted;
            case AsyncIoContext::ProgressWaitOutcome::Kind::deadline_expired:
                break;
            case AsyncIoContext::ProgressWaitOutcome::Kind::health_failure:
                return ScopeWaitStatus::health_failure;
            }
        }
    }

    bool request_ready_(const Slot& slot) const noexcept;
    void request_cancel_(Slot& slot) noexcept;
    void request_discard_(Slot& slot) noexcept;

    void settle_and_release_() noexcept;

    AsyncIoContext& ctx_;
    std::size_t capacity_;
    ScopeCleanupPolicy policy_;
    ProgressOwner owner_;
    std::unique_ptr<Slot[]> slots_;
    bool finished_ = false;
};

}
