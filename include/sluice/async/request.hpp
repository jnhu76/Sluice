#pragma once

#include <sluice/effect.hpp>
#include <sluice/error.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace sluice::async {

class AsyncBackend;
class AsyncIoContext;

namespace detail {
class RequestCore;
struct RequestKey;
struct TestAccess;

// Out-of-line operations of the public Request<T> template, defined in the
// library so installed consumers need no internal header. RequestResultData
// carries the terminal payload in public vocabulary.
struct RequestBridge;
}

class RequestId {
  public:
    constexpr RequestId() noexcept = default;

    constexpr bool valid() const noexcept { return valid_; }

    friend constexpr bool operator==(const RequestId&, const RequestId&) noexcept = default;

  private:
    friend class AsyncIoContext;
    template <class T> friend class Request;
    friend struct detail::RequestBridge;
    friend struct detail::TestAccess;

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
    physical_interruption_unsupported,
};

enum class RequestReadiness : std::uint8_t { empty, pending, ready };

template <class T> struct RequestObservation {
    RequestReadiness readiness = RequestReadiness::empty;
    // The payload is only meaningful when readiness is ready.
    Result<T> result{make_unexpected<T>(IoError{IoError::Code::invalid_state})};
    // Data-effect report for the terminal (all-zero/accounted before ready,
    // and for operations without a data effect).
    sluice::EffectReport effect{};
};

struct RequestResultData {
    bool succeeded = false;
    IoError error{};
    sluice::EffectReport effect{};
    sluice::FileInfo metadata{};
};

namespace detail {

struct RequestBridge {
    static RequestKey key_of(const RequestId& id) noexcept;

    static RequestReadiness ready(const RequestCore* core, const RequestId& id) noexcept;

    // Pending leaves the handle unchanged; stale never returns (fail-fast).
    static RequestReadiness observe(const RequestCore* core, const RequestId& id,
                                    RequestResultData* out) noexcept;

    static RequestReadiness consume(RequestCore* core, const RequestId& id,
                                    RequestResultData* out) noexcept;

    // Releases the public binding; nonterminal and stale releases fail fast.
    static void discard(RequestCore* core, const RequestId& id) noexcept;

    static CancelDisposition cancel(AsyncBackend* backend, const RequestId& id) noexcept;
};

}

template <class T> class Request {
  public:
    using value_type = T;

    constexpr Request() noexcept = default;

    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    Request(Request&& other) noexcept
        : core_(std::exchange(other.core_, nullptr)),
          backend_(std::exchange(other.backend_, nullptr)), id_(std::exchange(other.id_, RequestId{})) {}

    Request& operator=(Request&& other) noexcept {
        if (this != &other) {
            release_responsibility_();
            core_ = std::exchange(other.core_, nullptr);
            backend_ = std::exchange(other.backend_, nullptr);
            id_ = std::exchange(other.id_, RequestId{});
        }
        return *this;
    }

    ~Request() { release_responsibility_(); }

    constexpr bool valid() const noexcept { return core_ != nullptr; }

    constexpr RequestId id() const noexcept { return id_; }

    bool ready() const noexcept {
        return core_ != nullptr && detail::RequestBridge::ready(core_, id_) == RequestReadiness::ready;
    }

    RequestObservation<T> try_result() const noexcept {
        if (core_ == nullptr) {
            return {};
        }
        RequestResultData data;
        const RequestReadiness readiness = detail::RequestBridge::observe(core_, id_, &data);
        if (readiness == RequestReadiness::pending) {
            return {RequestReadiness::pending};
        }
        return {RequestReadiness::ready, result_of_(data), data.effect};
    }

    RequestObservation<T> take_result() noexcept {
        if (core_ == nullptr) {
            return {};
        }
        RequestResultData data;
        const RequestReadiness readiness = detail::RequestBridge::consume(core_, id_, &data);
        if (readiness == RequestReadiness::pending) {
            return {RequestReadiness::pending};
        }
        core_ = nullptr;
        backend_ = nullptr;
        id_ = RequestId{};
        return {RequestReadiness::ready, result_of_(data), data.effect};
    }

    void discard() noexcept { release_responsibility_(); }

    Result<CancelDisposition> cancel() {
        if (backend_ == nullptr) {
            return make_unexpected<CancelDisposition>(IoError{IoError::Code::invalid_state});
        }
        return detail::RequestBridge::cancel(backend_, id_);
    }

  private:
    friend class AsyncIoContext;
    friend struct detail::TestAccess;

    Request(detail::RequestCore* core, AsyncBackend* backend, RequestId id) noexcept
        : core_(core), backend_(backend), id_(id) {}

    void release_responsibility_() noexcept {
#if defined(SLUICE_B2_MUTANT_RELEASE_FORGETS_BINDING)
        core_ = nullptr;
        backend_ = nullptr;
        id_ = RequestId{};
        return;
#endif
        if (core_ == nullptr) {
            return;
        }
        detail::RequestBridge::discard(core_, id_);
        core_ = nullptr;
        backend_ = nullptr;
        id_ = RequestId{};
    }

    static Result<T> result_of_(const RequestResultData& data) noexcept {
        if constexpr (std::is_void_v<T>) {
            if (data.succeeded) {
                return {};
            }
            return make_unexpected<T>(data.error);
        } else if constexpr (std::is_same_v<T, sluice::FileInfo>) {
            if (data.succeeded) {
                return Result<T>{data.metadata};
            }
            return make_unexpected<T>(data.error);
        } else if constexpr (std::is_same_v<T, sluice::FileSize>) {
            if (data.succeeded) {
                return Result<T>{FileSize{data.metadata.size}};
            }
            return make_unexpected<T>(data.error);
        } else {
            if (data.succeeded) {
                return Result<T>{static_cast<T>(data.effect.confirmed_bytes)};
            }
            return make_unexpected<T>(data.error);
        }
    }

    detail::RequestCore* core_ = nullptr;
    AsyncBackend* backend_ = nullptr;
    RequestId id_{};
};

}
