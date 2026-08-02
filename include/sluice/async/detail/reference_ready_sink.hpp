// sluice::async::detail — no-op SynchronousReadySink for the Phase B reference
// backends (FakeAsyncBackend, SyncBackend).
//
// ADR-explicit-io-request-contract (Accepted) Decision 9: reap invokes
// on_ready exactly once per Completion-ready publication, AFTER releasing every
// slot/backend lock. The contract is noexcept, allocation-independent, and must
// not retain the event reference.
//
// The reference backends have no Scheduler routing record to update (Phase B
// uses fake stable tokens/leases only; the real Scheduler seam is Phase F —
// ADR Decision 10 :674-676). Their reap therefore delivers ReadyEvents to a
// sink that proves the by-value transfer and exactly-once mechanics without
// side effects. Phase F will replace this with a real Scheduler-owned sink; the
// ReadySink contract here is what Phase F must satisfy.
//
// This is a header-only detail (not installed beyond the async surface) used by
// the reference backends; production code outside the reference backends MUST
// NOT depend on it.
#pragma once

#include <sluice/async/detail/ready_sink.hpp>

namespace sluice::async::detail {

// Counts deliveries so a test can assert exactly-once (one on_ready per
// Completion-ready publication). The count is the only observable side effect.
class ReferenceReadySink final : public SynchronousReadySink {
  public:
    void on_ready(ReadyEvent /*event*/) noexcept override { ++deliveries_; }

    std::size_t deliveries() const noexcept { return deliveries_; }

  private:
    std::size_t deliveries_ = 0;
};

} // namespace sluice::async::detail
