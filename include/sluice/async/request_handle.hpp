// sluice::async::RequestHandle — opaque public identity for one accepted I/O
// request (ADR-public-request-handle, Phase F3 / issue #98).
//
// Identity, NOT ownership (ADR Decision 1): a handle names the logical request
// the internal detail::RequestKey encodes — (ContextIdentity, SlotIndex,
// Generation) — without owning or pinning the Completion, fd/buffer borrow,
// RequestSlot, RoutingLease, or terminal result. Copying a handle allocates
// nothing and does not extend any borrow lifetime.
//
// Non-forgeable (ADR Decision 2 / AC-13 / AC-14): the identity components are
// PRIVATE; ordinary public code cannot read or set them. The only producer of a
// valid handle is the backend commit path (friend class AsyncBackend), which
// derives the identity from the Completion's private arena binding. Public
// callers receive a handle from submit_*_request and may query it via
// request_state(); they cannot manufacture one.
//
// Lifecycle (ADR Decision 6): a handle is valid while its generation is the
// current occupant of its slot in its context. After the Completion is reset /
// destroyed (slot released, generation++) or the slot is reused, the handle is
// inert — request_state() reports not_found, and no operation can target the new
// occupant through the stale handle.
#pragma once

#include <cstdint>

namespace sluice::async {

class AsyncBackend;  // construction + component-extraction authority (friend)

// Public observation of a request's current state (ADR Decision 6). Orthogonal
// to success/error: it reflects the request's position in its lifecycle, not its
// terminal result. `not_found` covers stale-generation, cross-context, released,
// and invalid handles.
enum class RequestHandleState : std::uint8_t {
    outstanding,        // accepted, not yet terminal (pending/enqueued/running)
    backend_ready,      // terminal won, not yet reaped to Completion-ready
    completion_ready,   // reaped; Completion::ready()
    not_found,          // stale / wrong context / released / invalid
};

// Opaque accepted-request identity value. Trivially copyable; safe to copy,
// store, and compare. Two handles compare equal iff they name the same
// (context, slot, generation) tuple.
class RequestHandle {
public:
    // An invalid handle (valid() == false): the result of default construction
    // or a rejected submit. request_state() on it reports not_found.
    constexpr RequestHandle() noexcept = default;

    constexpr bool valid() const noexcept { return valid_; }

    friend bool operator==(const RequestHandle&, const RequestHandle&) noexcept = default;

private:
    // Only the backend commit path constructs a valid handle (ADR Decision 2).
    friend class AsyncBackend;
    constexpr RequestHandle(std::uint64_t context, std::uint32_t slot,
                            std::uint64_t generation) noexcept
        : context_(context), slot_(slot), generation_(generation), valid_(true) {}

    std::uint64_t context_ = 0;
    std::uint32_t slot_ = 0;
    std::uint64_t generation_ = 0;
    bool valid_ = false;
};

}  // namespace sluice::async
