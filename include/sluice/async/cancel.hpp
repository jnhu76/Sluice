#pragma once

#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cstdint>

namespace sluice::async {

class CancelState;

enum class CancelProtection : std::uint8_t {
    unblocked = 0,
    blocked = 1,
};

class CancelToken {
  public:
    CancelToken() = default;

    CancelToken(const CancelToken&) = delete;
    CancelToken& operator=(const CancelToken&) = delete;
    CancelToken(CancelToken&&) = delete;
    CancelToken& operator=(CancelToken&&) = delete;

    void request() noexcept;

    bool is_requested() const noexcept;

    std::uint64_t epoch() const noexcept;

    void rearm() noexcept;

    void clear() noexcept;

  private:
    std::atomic<std::uint64_t> state_{0};

    friend Result<void> check_cancel(const CancelToken& token, CancelState& state) noexcept;
};

class CancelState {
  public:
    CancelProtection protection() const noexcept { return protection_; }

    CancelProtection swap_protection(CancelProtection next) noexcept;

    bool acknowledged(const CancelToken& token) const noexcept;

    void acknowledge(const CancelToken& token) noexcept;

    void reset_acknowledgement() noexcept { acknowledged_epoch_ = 0; }

  private:
    CancelProtection protection_{CancelProtection::unblocked};
    std::uint64_t acknowledged_epoch_{0};

    friend Result<void> check_cancel(const CancelToken& token, CancelState& state) noexcept;
};

class [[nodiscard]] CancelGuard {
  public:
    CancelGuard(CancelState& state, CancelProtection next) noexcept
        : state_(&state), prev_(state.swap_protection(next)) {}
    ~CancelGuard() {
        if (state_)
            (void)state_->swap_protection(prev_);
    }
    CancelGuard(const CancelGuard&) = delete;
    CancelGuard& operator=(const CancelGuard&) = delete;
    CancelGuard(CancelGuard&& other) noexcept : state_(other.state_), prev_(other.prev_) {
        other.state_ = nullptr;
    }
    CancelGuard& operator=(CancelGuard&&) = delete;

  private:
    CancelState* state_;
    CancelProtection prev_;
};

Result<void> check_cancel(const CancelToken& token, CancelState& state) noexcept;

} // namespace sluice::async
