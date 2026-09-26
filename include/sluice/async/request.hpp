#pragma once

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace sluice::async {

class AsyncBackend;
class AsyncIoContext;

class RequestId {
  public:
    constexpr RequestId() noexcept = default;

    constexpr bool valid() const noexcept { return valid_; }

    friend constexpr bool operator==(const RequestId&, const RequestId&) noexcept = default;

  private:
    friend class AsyncIoContext;
    template <class T> friend class Request;

    constexpr RequestId(std::uint64_t context, std::uint32_t slot,
                        std::uint64_t generation) noexcept
        : context_(context), slot_(slot), generation_(generation), valid_(true) {}

    std::uint64_t context_ = 0;
    std::uint32_t slot_ = 0;
    std::uint64_t generation_ = 0;
    bool valid_ = false;
};

enum class CancelDisposition : std::uint8_t {
    won_before_execution,
    requested,
    already_terminal,
    not_found,
};

enum class RequestReadiness : std::uint8_t { empty, pending, ready };

template <class T> struct RequestObservation {
    RequestReadiness readiness = RequestReadiness::empty;
    // The payload is only meaningful when readiness is ready.
    Result<T> result{make_unexpected<T>(IoError{IoError::Code::invalid_state})};
};

template <class T> class Request {
  public:
    using value_type = T;

    constexpr Request() noexcept = default;

    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    Request(Request&& other) noexcept
        : core_(std::exchange(other.core_, nullptr)),
          backend_(std::exchange(other.backend_, nullptr)), key_(std::exchange(other.key_, {})) {}

    Request& operator=(Request&& other) noexcept {
        if (this != &other) {
            release_responsibility_();
            core_ = std::exchange(other.core_, nullptr);
            backend_ = std::exchange(other.backend_, nullptr);
            key_ = std::exchange(other.key_, {});
        }
        return *this;
    }

    ~Request() { release_responsibility_(); }

    constexpr bool valid() const noexcept { return core_ != nullptr; }

    constexpr RequestId id() const noexcept {
        return core_ != nullptr ? RequestId{key_.context.value, key_.slot.value,
                                            key_.generation.value}
                                : RequestId{};
    }

    bool ready() const noexcept {
        return core_ != nullptr && core_->lookup(key_) == detail::PublicLookup::published;
    }

    RequestObservation<T> try_result() const noexcept {
        if (core_ == nullptr) {
            return {};
        }
        sluice::detail::IoOutcome outcome;
        const detail::PublicObservation observed = core_->observe_public_result(key_, &outcome);
        if (observed == detail::PublicObservation::pending) {
            return {RequestReadiness::pending};
        }
        if (observed == detail::PublicObservation::stale) {
            detail::request_binding_invariant_fail_fast();
        }
        return {RequestReadiness::ready, result_of_(outcome)};
    }

    RequestObservation<T> take_result() noexcept {
        if (core_ == nullptr) {
            return {};
        }
        sluice::detail::IoOutcome outcome;
        const detail::PublicConsumption consumed = core_->consume_public_result(key_, &outcome);
        if (consumed == detail::PublicConsumption::pending) {
            return {RequestReadiness::pending};
        }
        if (consumed == detail::PublicConsumption::stale) {
            detail::request_binding_invariant_fail_fast();
        }
        core_ = nullptr;
        backend_ = nullptr;
        key_ = {};
        return {RequestReadiness::ready, result_of_(outcome)};
    }

    void discard() noexcept { release_responsibility_(); }

    Result<CancelDisposition> cancel();

  private:
    friend class AsyncIoContext;

    Request(detail::RequestCore* core, AsyncBackend* backend, detail::RequestKey key) noexcept
        : core_(core), backend_(backend), key_(key) {}

    void release_responsibility_() noexcept {
#if defined(SLUICE_B2_MUTANT_RELEASE_FORGETS_BINDING)
        core_ = nullptr;
        backend_ = nullptr;
        key_ = {};
        return;
#endif
        if (core_ == nullptr) {
            return;
        }
        const detail::BindingRelease released = core_->discard_public_result(key_);
#if defined(SLUICE_B2_MUTANT_NONTERMINAL_RELEASE_DETACHES)
        if (released == detail::BindingRelease::stale) {
#else
        if (released == detail::BindingRelease::not_visible_yet) {
#endif
            detail::request_nonterminal_release_fail_fast();
        }
        if (released == detail::BindingRelease::stale) {
            detail::request_binding_invariant_fail_fast();
        }
        core_ = nullptr;
        backend_ = nullptr;
        key_ = {};
    }

    static Result<T> result_of_(const sluice::detail::IoOutcome& outcome) noexcept {
        if constexpr (std::is_void_v<T>) {
            if (outcome.succeeded) {
                return {};
            }
            return make_unexpected<T>(outcome.error);
        } else {
            if (outcome.succeeded) {
                return Result<T>{static_cast<T>(outcome.effect.confirmed_bytes)};
            }
            return make_unexpected<T>(outcome.error);
        }
    }

    detail::RequestCore* core_ = nullptr;
    AsyncBackend* backend_ = nullptr;
    detail::RequestKey key_{};
};

}
