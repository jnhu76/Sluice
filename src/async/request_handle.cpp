// Phase F3 (issue #98) — public RequestHandle identity plumbing.
// ADR-public-request-handle. These definitions need both Completion's and
// RequestArena's full definitions: identity_of reads the Completion's private
// arena binding (release_arena_ + bound_slot_, friend access via AsyncBackend)
// and dereferences release_arena_->context(); request_handle_state extracts the
// handle's private components (friend access) and delegates to the virtual
// resolve_identity_state() hook.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request_handle.hpp>
#include <sluice/async/detail/request_arena.hpp>

namespace sluice::async {

// Derive a handle from a Completion's private commit-time binding. Returns an
// invalid handle when the Completion has no arena binding (legacy/external
// backend that did not use install_binding). AsyncBackend is a friend of both
// Completion (to read release_arena_/bound_slot_) and RequestHandle (to call the
// private identity constructor).
RequestHandle AsyncBackend::identity_of(Completion<std::size_t>& c) const noexcept {
    if (c.release_arena_ == nullptr) return {};
    return RequestHandle{c.release_arena_->context().value,
                         c.bound_slot_.slot.value,
                         c.bound_slot_.generation.value};
}

RequestHandle AsyncBackend::identity_of(Completion<void>& c) const noexcept {
    if (c.release_arena_ == nullptr) return {};
    return RequestHandle{c.release_arena_->context().value,
                         c.bound_slot_.slot.value,
                         c.bound_slot_.generation.value};
}

// Extract the handle's private identity components (friend of RequestHandle) and
// delegate to the per-backend virtual. An invalid handle short-circuits: it
// cannot name any request.
Result<RequestHandleState> AsyncBackend::request_handle_state(
    const RequestHandle& h) const noexcept {
    if (!h.valid()) return RequestHandleState::not_found;
    return resolve_identity_state(h.context_, h.slot_, h.generation_);
}

}  // namespace sluice::async
