#pragma once

#include <sluice/async/detail/ready_sink.hpp>

#include <cstddef>

namespace sluice::async::detail {

class ReferenceReadySink final : public SynchronousReadySink {
  public:
    void on_ready(ReadyEvent event) noexcept override {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        ++deliveries_;
        last_key_ = event.key;
#endif
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    std::size_t deliveries() const noexcept { return deliveries_; }
    RequestKey last_key() const noexcept { return last_key_; }
#endif

  private:
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    std::size_t deliveries_ = 0;
    RequestKey last_key_{};
#endif
};

}
