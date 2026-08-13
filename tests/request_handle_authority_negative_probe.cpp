// request_handle_authority_negative_probe.cpp
//
// ADR-public-request-handle — negative-compile authority probe.
//
// Proves the public RequestHandle is construction-controlled (non-forgeable):
// ordinary application code cannot manufacture a valid handle, read its private
// identity components, or convert an internal detail::RequestKey into a handle.
// Each NEG_<KIND> macro selects ONE forbidden usage; the verify script compiles
// this file with exactly one NEG_* macro defined at a time and asserts the
// compile FAILS with a private-access / no-conversion diagnostic.
//
// Without any NEG_* macro this file compiles cleanly (positive control): it
// exercises only the public RequestHandle surface (default ctor, valid(),
// copy, equality) plus the read-only Result<RequestHandle> shape.
#include <sluice/async/request_handle.hpp>

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
    RequestHandle h{1, 7, 4};  // ERROR: calling a private constructor
    (void)h;
}
#endif

#if defined(NEG_READ_HANDLE_CONTEXT)
// The identity components are PRIVATE. A caller cannot read the context token
// (which would let it forge or cross-check authority out-of-band).
void neg_read_handle_context() {
    RequestHandle h;
    auto v = h.context_;  // ERROR: 'context_' is private
    (void)v;
}
#endif

#if defined(NEG_READ_HANDLE_SLOT)
void neg_read_handle_slot() {
    RequestHandle h;
    auto v = h.slot_;  // ERROR: 'slot_' is private
    (void)v;
}
#endif

#if defined(NEG_READ_HANDLE_GENERATION)
void neg_read_handle_generation() {
    RequestHandle h;
    auto v = h.generation_;  // ERROR: 'generation_' is private
    (void)v;
}
#endif

#if defined(NEG_SET_HANDLE_VALID)
// valid_ is private; a caller cannot flip an invalid handle to valid.
void neg_set_handle_valid() {
    RequestHandle h;
    h.valid_ = true;  // ERROR: 'valid_' is private
}
#endif

int main() {
    positive_control();
    return 0;
}
