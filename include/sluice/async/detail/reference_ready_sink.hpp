// sluice::async::detail — no-op SynchronousReadySink used by the
// RequestArena-backed production backends (FakeAsyncBackend, SyncBackend,
// ThreadPoolBackend, UringAsyncBackend).
//
// ADR-explicit-io-request-contract (Accepted) Decision 9: reap invokes
// on_ready exactly once per Completion-ready publication, AFTER releasing every
// slot/backend lock. The contract is noexcept, allocation-independent, and must
// not retain the event reference.
//
// The name "Reference" is historical: ALL four production backends hold a
// `detail::ReferenceReadySink` as their internal default reap sink on the
// unified RequestArena lifecycle. It is the stateless no-op ReadySink for
// RequestArena-backed production reap paths; a Scheduler-owned routing sink
// can be attached per backend (AsyncBackend::attach_ready_sink) and then
// receives the by-value events instead (ADR Decision 10 :674-676). Its
// on_ready therefore proves the by-value transfer and exactly-once mechanics
// without side effects.
//
// This is a header-only detail (not installed beyond the async surface). A
// rename to e.g. `NoopReadySink` is deferred to avoid churn; the class name is
// historical, the role is not.
#pragma once

#include <sluice/async/detail/ready_sink.hpp>

#include <cstddef>

namespace sluice::async::detail {

// No-op SynchronousReadySink for the RequestArena-backed production backends
// (Fake, Sync, ThreadPool, Uring). The sink is STATELESS: on_ready does nothing
// (this no-op holds no Scheduler routing record; the Scheduler-owned sink is
// attached separately). The delivery
// counter exists ONLY for
// test assertions of exactly-once publication, so it is guarded by
// SLUICE_ASYNC_INTERNAL_TESTING (keep test-only delivery
// accounting out of the production sink — AGENTS.md §8). Production builds
// therefore carry no counter field and no exported test surface.
//
// The guarded observation additionally records the LAST delivered event's
// waiter payload (has_waiter, token, lease id) as plain by-value scalars — a
// FIXED-SIZE, allocation-free, test-only observation (AGENTS.md §12: no
// long-lived per-delivery storage, no heap history). The lease itself is NOT
// stored or consumed: on_ready still drops the by-value event exactly like the
// production no-op, so the observation never changes ownership semantics. A
// test reads the payload after reap returns (single-threaded observation) to
// prove exactly-once delivery of a specific token/lease.
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

} // namespace sluice::async::detail
