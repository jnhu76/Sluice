#pragma once

#include <cstdint>

namespace sluice::async {

class AsyncBackend;

enum class RequestHandleState : std::uint8_t {
    outstanding,
    backend_ready,
    completion_ready,
    not_found,
};

class RequestHandle {
  public:
    constexpr RequestHandle() noexcept = default;

    constexpr bool valid() const noexcept { return valid_; }

    friend bool operator==(const RequestHandle&, const RequestHandle&) noexcept = default;

  private:
    friend class AsyncBackend;
    constexpr RequestHandle(std::uint64_t context, std::uint32_t slot,
                            std::uint64_t generation) noexcept
        : context_(context), slot_(slot), generation_(generation), valid_(true) {}

    std::uint64_t context_ = 0;
    std::uint32_t slot_ = 0;
    std::uint64_t generation_ = 0;
    bool valid_ = false;
};

} // namespace sluice::async
