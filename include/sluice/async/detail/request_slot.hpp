#pragma once

#include <sluice/async/detail/ready_sink.hpp>
#include <sluice/async/detail/request_key.hpp>
#include <sluice/error.hpp>

#include <cstddef>
#include <cstdint>

namespace sluice::async::detail {

enum class RequestState : std::uint8_t {
    free,
    reserved,
    prepared,
    pending,
    enqueued,
    running,
    backend_ready,
    completion_ready,
};

struct TerminalResult {
    bool stored = false;
    bool is_error = false;
    std::uint64_t bytes = 0;
    IoError error{IoError::Code::backend_error};

    static TerminalResult ok_bytes(std::uint64_t n) noexcept { return {true, false, n, {}}; }
    static TerminalResult ok_void() noexcept { return {true, false, 0, {}}; }
    static TerminalResult err(IoError e) noexcept { return {true, true, 0, e}; }
};

enum class WaiterRegistration : std::uint8_t {
    open_no_waiter,
    open_registered,
    closed,
};

struct BorrowMetadata {
    int fd = -1;
    const void* address = nullptr;
    std::size_t length = 0;
    bool active = false;
};

struct CompletionBinding {
    void* completion = nullptr;
    std::uint64_t requested_bytes = 0;
    void (*publish)(void* completion, const TerminalResult&) noexcept = nullptr;

    bool installed() const noexcept { return publish != nullptr; }
};

class RequestSlot {
  public:
    RequestSlot() = default;

    static constexpr std::uint32_t kNotOnReadyRing = static_cast<std::uint32_t>(-1);

  private:
    friend class RequestArena;

    RequestState state_ = RequestState::free;
    Generation generation_{0};
    RequestKey key_{};
    OperationKind op_kind_ = OperationKind::read;

    std::uint64_t submit_seq_ = 0;

    bool enqueue_in_flight_pin_ = false;

    TerminalResult terminal_{};

    WaiterRegistration registration_ = WaiterRegistration::open_no_waiter;
    WaiterToken waiter_token_{};
    RoutingLease waiter_lease_{};

    bool waiter_delivery_present_ = false;

    CompletionBinding publication_binding_{};

    BorrowMetadata borrow_{};

    std::uint32_t ready_next_ = kNotOnReadyRing;

    bool cancel_intent_ = false;
};

} // namespace sluice::async::detail
