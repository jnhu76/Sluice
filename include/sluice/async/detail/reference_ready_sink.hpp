




















#pragma once

#include <sluice/async/detail/ready_sink.hpp>

#include <cstddef>

namespace sluice::async::detail {



















class ReferenceReadySink final : public SynchronousReadySink {
  public:
    void on_ready(ReadyEvent event) noexcept override {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        ++deliveries_;
        last_has_waiter_ = event.waiter.has_waiter;
        if (event.waiter.has_waiter) {
            last_token_ = event.waiter.token;
            last_lease_id_ = event.waiter.lease.id();
        }
#endif
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    std::size_t deliveries() const noexcept { return deliveries_; }
    bool last_has_waiter() const noexcept { return last_has_waiter_; }
    WaiterToken last_token() const noexcept { return last_token_; }
    std::uint64_t last_lease_id() const noexcept { return last_lease_id_; }
#endif

  private:
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    std::size_t deliveries_ = 0;
    bool last_has_waiter_ = false;
    WaiterToken last_token_{};
    std::uint64_t last_lease_id_ = 0;
#endif
};

}
