#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request_handle.hpp>
#include <sluice/async/detail/request_arena.hpp>

namespace sluice::async {

RequestHandle AsyncBackend::identity_of(Completion<std::size_t>& c) const noexcept {
    if (c.release_arena_ == nullptr)
        return {};
    return RequestHandle{c.release_arena_->context().value, c.bound_slot_.slot.value,
                         c.bound_slot_.generation.value};
}

RequestHandle AsyncBackend::identity_of(Completion<void>& c) const noexcept {
    if (c.release_arena_ == nullptr)
        return {};
    return RequestHandle{c.release_arena_->context().value, c.bound_slot_.slot.value,
                         c.bound_slot_.generation.value};
}

Result<RequestHandleState>
AsyncBackend::request_handle_state(const RequestHandle& h) const noexcept {
    if (!h.valid())
        return RequestHandleState::not_found;
    return resolve_identity_state(h.context_, h.slot_, h.generation_);
}

} // namespace sluice::async
