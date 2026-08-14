// request_handle_authority_negative_probe.cpp
//
// ADR-public-request-handle — negative-compile authority probe.
//
// Proves the public RequestHandle is construction-controlled (non-forgeable):
// ordinary application code cannot manufacture a valid handle, read its private
// identity components, or convert an internal detail::RequestKey into a handle.
// It ALSO proves the AsyncBackend identity seam is sealed: the handle-minting
// helper (identity_of) and the raw identity-tuple resolvers
// (request_handle_state / the concrete-backend resolve_identity_state override)
// are PRIVATE — even code holding a raw backend pointer (AsyncBackend is a
// public extension point) cannot bypass AsyncIoContext::submit_*_request /
// request_state.
// Each NEG_<KIND> macro selects ONE forbidden usage; the verify script compiles
// this file with exactly one NEG_* macro defined at a time and asserts the
// compile FAILS with a private-access / no-conversion diagnostic.
//
// Without any NEG_* macro this file compiles cleanly (positive control): it
// exercises only the public RequestHandle surface (default ctor, valid(),
// copy, equality) plus the read-only Result<RequestHandle> shape.
#include <sluice/async/async_io_context.hpp>   // AsyncBackend seam (bypass cases)
#include <sluice/async/completion.hpp>         // Completion<T> (identity_of case)
#include <sluice/async/fake_backend.hpp>       // concrete-backend override (sealed case)
#include <sluice/async/request_handle.hpp>
#include <sluice/async/detail/request_key.hpp> // internal RequestKey (conversion case)

#include <utility>

using namespace sluice::async;

// Positive control: public API only. Always compiles.
void positive_control() {
    RequestHandle h;            // default ctor -> invalid
    (void)h.valid();
    RequestHandle h2 = h;       // copy
    (void)(h == h2);            // equality
    (void)std::move(h2);
}

#if defined(NEG_FORGE_HANDLE_CTOR)
// The identity constructor is PRIVATE (friend AsyncBackend only). Ordinary code
// cannot construct a valid handle from raw (context, slot, generation).
void neg_forge_handle_ctor() {
    RequestHandle h{1, 7, 4};  // expected compile failure
    (void)h;
}
#endif

#if defined(NEG_READ_HANDLE_CONTEXT)
// The identity components are PRIVATE. A caller cannot read the context token
// (which would let it forge or cross-check authority out-of-band).
void neg_read_handle_context() {
    RequestHandle h;
    auto v = h.context_;  // expected compile failure
    (void)v;
}
#endif

#if defined(NEG_READ_HANDLE_SLOT)
void neg_read_handle_slot() {
    RequestHandle h;
    auto v = h.slot_;  // expected compile failure
    (void)v;
}
#endif

#if defined(NEG_READ_HANDLE_GENERATION)
void neg_read_handle_generation() {
    RequestHandle h;
    auto v = h.generation_;  // expected compile failure
    (void)v;
}
#endif

#if defined(NEG_SET_HANDLE_VALID)
// valid_ is private; a caller cannot flip an invalid handle to valid.
void neg_set_handle_valid() {
    RequestHandle h;
    h.valid_ = true;  // expected compile failure
}
#endif

#if defined(NEG_CONVERT_REQUEST_KEY)
// An internal detail::RequestKey (mintable by including the detail header) has
// NO conversion to RequestHandle. The ADR Decision-2 promise — "cannot turn it
// into a RequestHandle" — holds even for code that can see the internal type.
void neg_convert_request_key() {
    detail::RequestKey key{detail::ContextIdentity{1}, detail::SlotIndex{7},
                           detail::Generation{4}};
    RequestHandle h = key;  // expected compile failure
    (void)h;
}
#endif

#if defined(NEG_CALL_BACKEND_IDENTITY_OF)
// AsyncBackend::identity_of is PRIVATE (friend AsyncIoContext only): a raw
// backend pointer cannot mint a valid handle from a Completion. The only handle
// producer is AsyncIoContext::submit_*_request (ADR Decision 2).
void neg_call_backend_identity_of() {
    AsyncBackend* b = nullptr;
    Completion<std::size_t> c;
    (void)b->identity_of(c);  // expected compile failure
}
#endif

#if defined(NEG_CALL_BACKEND_REQUEST_HANDLE_STATE)
// AsyncBackend::request_handle_state is PRIVATE (friend AsyncIoContext only): a
// raw backend pointer cannot feed a handle into the raw resolver out-of-band.
void neg_call_backend_request_handle_state() {
    AsyncBackend* b = nullptr;
    RequestHandle h;
    (void)b->request_handle_state(h);  // expected compile failure
}
#endif

#if defined(NEG_CALL_CONCRETE_RESOLVE_IDENTITY_STATE)
// The concrete-backend override of the private virtual resolve_identity_state
// is private too: the raw (context, slot, generation) tuple consumer is never
// reachable through a backend pointer. Override access is checked at the call
// site, so leaving the override public would reopen the seam through the
// derived class (ADR Decision 2: no raw identity-tuple consumer).
void neg_call_concrete_resolve_identity_state() {
    FakeAsyncBackend* b = nullptr;
    (void)b->resolve_identity_state(1, 7, 4);  // expected compile failure
}
#endif

int main() {
    positive_control();
    return 0;
}
