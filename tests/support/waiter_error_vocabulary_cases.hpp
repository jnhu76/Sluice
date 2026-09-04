// S0B-CORRECTIVE-1 W1 — shared waiter error-vocabulary regression.
//
// Pins the adjudicated split (ADR-explicit-io-request-contract Decision 6 +
// Decision 10; see docs/investigations/s0b-contract-reality.md, finding W1):
//
//   register_waiter on an unbound / cross-context / stale / released
//   Completion   -> invalid_state (provenance misuse — "direct use of an
//                   invalid/stale key", Decision 6). Registering is an
//                   install action on a caller-held Completion, NOT a benign
//                   lookup.
//   duplicate registration                       -> invalid_state (Decision 10)
//   registration after reap closed it            -> invalid_state
//   cancel_waiter on the SAME misses             -> not_found (the benign
//                   miss Decision 6 grants to cancel lookups); register and
//                   cancel intentionally do NOT share vocabulary.
//
// RequestHandleState::not_found (request_state) is a separate read-only
// observation surface and keeps its own vocabulary; nothing here changes it.
//
// Every case runs through the PUBLIC AsyncBackend interface — no test seams —
// so each arena backend drives the same assertions through its own forwarding
// layer. The candidate RoutingLease is moved into each failing call at the
// by-value boundary and consumed there (the lease is move-only; no copy can
// outlive a failure), which is the structural half of the exactly-once
// lease-transfer rule (ADR Decision 10).
//
// Usage: one SLUICE_TEST_CASE per backend calls
// `run_waiter_error_vocabulary_cases<Backend>` with a factory returning
// `std::unique_ptr<Backend>`, a real writable fd, and a DriveReady lambda
// `(Backend&, Completion<std::size_t>&) -> bool` that settles one submitted
// write (Fake: complete_oldest_with_bytes + one poll; Sync: one poll;
// ThreadPool/Uring: bounded poll loop). Returns nullptr on pass, else the
// failing clause.
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <memory>
#include <utility>

namespace waiter_error_vocabulary {

using WaiterToken = sluice::async::detail::WaiterToken;
using RoutingLease = sluice::async::detail::RoutingLease;

template <typename Backend, typename MakeBackend, typename DriveReady>
const char* run_waiter_error_vocabulary_cases(MakeBackend make_backend, int fd,
                                               DriveReady drive_ready) {
    using sluice::IoError;
    std::byte buf[8]{std::byte{0x57}};

    // (1) Unbound Completion: register is provenance misuse, cancel is a
    // benign miss. The two codes MUST differ for the same target.
    {
        std::unique_ptr<Backend> b = make_backend();
        sluice::async::Completion<std::size_t> unbound;
        auto reg = b->register_waiter(unbound, WaiterToken{1, 0, 1}, RoutingLease{1});
        if (reg.has_value() || reg.error().code != IoError::Code::invalid_state)
            return "unbound register_waiter must be invalid_state";
        auto cw = b->cancel_waiter(unbound);
        if (cw.has_value() || cw.error().code != IoError::Code::not_found)
            return "unbound cancel_waiter must be not_found";
    }

    // (2) Cross-context: the Completion is bound to b1's arena; addressing it
    // at b2 must not resolve (foreign context identity).
    {
        std::unique_ptr<Backend> b1 = make_backend();
        std::unique_ptr<Backend> b2 = make_backend();
        sluice::async::Completion<std::size_t> c;
        if (!b1->submit_write(sluice::async::WriteOp{fd, buf, 8, 0}, c).has_value())
            return "cross-context setup submit failed";
        auto reg = b2->register_waiter(c, WaiterToken{2, 0, 1}, RoutingLease{2});
        if (reg.has_value() || reg.error().code != IoError::Code::invalid_state)
            return "cross-context register_waiter must be invalid_state";
        auto cw = b2->cancel_waiter(c);
        if (cw.has_value() || cw.error().code != IoError::Code::not_found)
            return "cross-context cancel_waiter must be not_found";
        if (!drive_ready(*b1, c) || !c.ready())
            return "cross-context drive_ready failed";
        c.reset();
    }

    // (3) Duplicate registration loses without overwriting: the public
    // no-overwrite proof is that wait-cancel afterwards returns the FIRST
    // waiter's lease, never the duplicate's.
    {
        std::unique_ptr<Backend> b = make_backend();
        sluice::async::Completion<std::size_t> c;
        if (!b->submit_write(sluice::async::WriteOp{fd, buf, 8, 0}, c).has_value())
            return "duplicate setup submit failed";
        if (!b->register_waiter(c, WaiterToken{3, 0, 1}, RoutingLease{300}).has_value())
            return "first registration must succeed";
        auto dup = b->register_waiter(c, WaiterToken{3, 0, 2}, RoutingLease{301});
        if (dup.has_value() || dup.error().code != IoError::Code::invalid_state)
            return "duplicate register_waiter must be invalid_state";
        auto rl = b->cancel_waiter(c);
        if (!rl.has_value() || rl.value().id() != 300)
            return "duplicate must not overwrite the first lease";
        if (!drive_ready(*b, c) || !c.ready())
            return "duplicate drive_ready failed";
        c.reset();
    }

    // (4) Reap-closed registration (Completion ready but still bound), then
    // stale after the caller releases the slot. Both rejections are
    // invalid_state for register; cancel_waiter stays not_found throughout.
    {
        std::unique_ptr<Backend> b = make_backend();
        sluice::async::Completion<std::size_t> c;
        if (!b->submit_write(sluice::async::WriteOp{fd, buf, 8, 0}, c).has_value())
            return "post-reap setup submit failed";
        if (!drive_ready(*b, c) || !c.ready())
            return "post-reap drive_ready failed";
        auto reg = b->register_waiter(c, WaiterToken{4, 0, 1}, RoutingLease{4});
        if (reg.has_value() || reg.error().code != IoError::Code::invalid_state)
            return "post-reap register_waiter must be invalid_state";
        auto cw_closed = b->cancel_waiter(c);
        if (cw_closed.has_value() ||
            cw_closed.error().code != IoError::Code::not_found)
            return "post-reap cancel_waiter must be not_found";
        c.reset();  // releases the slot; generation advances past this handle
        auto stale = b->register_waiter(c, WaiterToken{4, 0, 2}, RoutingLease{5});
        if (stale.has_value() || stale.error().code != IoError::Code::invalid_state)
            return "stale/released register_waiter must be invalid_state";
        auto cw_stale = b->cancel_waiter(c);
        if (cw_stale.has_value() || cw_stale.error().code != IoError::Code::not_found)
            return "stale/released cancel_waiter must be not_found";
    }

    // (5) The register-vs-cancel distinction on one live request: no
    // registration -> not_found; registered -> exact lease returned; a second
    // cancel -> not_found.
    {
        std::unique_ptr<Backend> b = make_backend();
        sluice::async::Completion<std::size_t> c;
        if (!b->submit_write(sluice::async::WriteOp{fd, buf, 8, 0}, c).has_value())
            return "cancel-distinction setup submit failed";
        auto cw_none = b->cancel_waiter(c);
        if (cw_none.has_value() || cw_none.error().code != IoError::Code::not_found)
            return "cancel_waiter with no registration must be not_found";
        if (!b->register_waiter(c, WaiterToken{5, 0, 1}, RoutingLease{500}).has_value())
            return "live registration must succeed";
        auto rl = b->cancel_waiter(c);
        if (!rl.has_value() || rl.value().id() != 500)
            return "wait-cancel must return the registered lease";
        auto cw_again = b->cancel_waiter(c);
        if (cw_again.has_value() ||
            cw_again.error().code != IoError::Code::not_found)
            return "second cancel_waiter must be not_found";
        if (!drive_ready(*b, c) || !c.ready())
            return "cancel-distinction drive_ready failed";
        c.reset();
    }

    return nullptr;
}

}  // namespace waiter_error_vocabulary
