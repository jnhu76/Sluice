// request_arena_negative_compile_probe.cpp
//
// Phase B (ADR-explicit-io-request-contract, Accepted) negative-compile probe for
// the bounded RequestSlot arena. Each NEG_<KIND> macro selects ONE forbidden
// usage that ordinary application code must NOT be able to compile. The verify
// script compiles this file with exactly one NEG_* macro defined at a time and
// asserts the compile FAILS with an access-control / no-member diagnostic.
//
// The authority model (ADR Decision 3 / Decision 5): every slot-lifecycle
// transition (state, generation, enqueue pin, terminal result, registration,
// publication binding, release) is owned by RequestArena under the leaf
// slot-lifecycle mutex. RequestSlot's mutating fields are private (friend
// RequestArena only). Ordinary code — even code that constructs a RequestArena
// — cannot:
//   - clear the enqueue-in-flight pin bit directly (must go through enqueue(),
//     the submit-path authority)
//   - publish backend_ready directly (must go through record_terminal, the
//     single terminal-winner authority)
//   - release a RequestSlot directly (must go through the release authorities:
//     rollback_reserved_or_prepared for pre-commit rollback,
//     release_completed_binding for the caller handshake — both check the pin
//     + registration invariants)
//   - increment the generation directly (must go through release, the slot-free
//     authority)
//   - mutate the slot state directly (must go through the documented stage
//     transitions)
//   - install the Completion publication binding directly (must go through
//     RequestArena::install_publication_binding before commit)
//
// Without any NEG_* macro, this file compiles cleanly (positive control): it
// exercises only the public test-seam introspection (state_of etc.)
// and the documented stage APIs (reserve/prepare/install_publication_binding/
// commit/enqueue/record_terminal/reap/release authorities).
#include <sluice/async/detail/request_arena.hpp>
#include <sluice/result.hpp>

#include <cstddef>

using sluice::async::detail::ContextIdentity;
using sluice::async::detail::OperationKind;
using sluice::async::detail::RequestArena;
using sluice::async::detail::RequestSlot;
using sluice::async::detail::SlotHandle;
using sluice::async::detail::TerminalResult;

// Positive control: the documented stage APIs compile and link. Always compiles.
void positive_control() {
    RequestArena arena{ContextIdentity::for_testing(1), 1};
    auto rh = arena.reserve();
    if (!rh.has_value())
        return;
    SlotHandle h = rh.value();
    // prepare() takes the borrow metadata; the reference backends perform no
    // real I/O so a default BorrowMetadata is a legal (no-op) descriptor here.
    (void)arena.prepare(h, OperationKind::read, {});
    // install_publication_binding rejects a null completion (CodeRabbit finding:
    // the publish thunk dereferences it), so the positive control passes a real
    // (unused) dummy address — the no-op lambda never dereferences it.
    static int dummy_completion = 0;
    (void)arena.install_publication_binding(
        h, &dummy_completion, 0, [](void*, const TerminalResult&) noexcept {});
    (void)arena.commit(h);
    (void)arena.enqueue(h);
    (void)arena.record_terminal(h, TerminalResult::ok_bytes(1));
    // Test-seam introspection (read-only; the design makes this a deliberate
    // test surface so the lifecycle contract is assertable).
    (void)arena.state_of(h.slot);
    (void)arena.enqueue_pin_live(h.slot);
    (void)arena.generation_of(h.slot);
    (void)arena.requested_bytes_of(h.slot);
    struct NoopSink : sluice::async::detail::SynchronousReadySink {
        void on_ready(sluice::async::detail::ReadyEvent) noexcept override {}
    } sink;
    (void)arena.reap(sink);
    arena.release_completed_binding(h);
}

#if defined(NEG_SLOT_STATE_PRIVATE)
// RequestSlot::state_ is private (friend RequestArena only). Ordinary code
// cannot mutate the state word directly — only the arena's stage transitions
// may, under the leaf mutex.
void neg_slot_state_private() {
    RequestSlot s;
    s.state_ = sluice::async::detail::RequestState::backend_ready; // ERROR: private
}
#endif

#if defined(NEG_SLOT_GENERATION_PRIVATE)
// RequestSlot::generation_ is private. Ordinary code cannot bump the ABA guard
// directly — only RequestArena::release may (I6).
void neg_slot_generation_private() {
    RequestSlot s;
    s.generation_ = sluice::async::detail::Generation{999}; // ERROR: private
}
#endif

#if defined(NEG_SLOT_PIN_PRIVATE)
// RequestSlot::enqueue_in_flight_pin_ is private. Ordinary code cannot clear the
// pin directly — only enqueue() (the submit-path authority) may.
void neg_slot_pin_private() {
    RequestSlot s;
    s.enqueue_in_flight_pin_ = false; // ERROR: private
}
#endif

#if defined(NEG_SLOT_BINDING_PRIVATE)
// RequestSlot::publication_binding_ is private. Ordinary code cannot install or
// forge the Completion publication binding directly — only
// RequestArena::install_publication_binding (before commit) may.
void neg_slot_binding_private() {
    RequestSlot s;
    s.publication_binding_.completion = nullptr; // ERROR: private
}
#endif

#if defined(NEG_SLOT_TERMINAL_PRIVATE)
// RequestSlot::terminal_ is private. Ordinary code cannot install a terminal
// result directly — only record_terminal/cancel (the single terminal-winner
// authorities) may.
void neg_slot_terminal_private() {
    RequestSlot s;
    s.terminal_ = TerminalResult::ok_bytes(1); // ERROR: private
}
#endif

#if defined(NEG_SLOT_REGISTRATION_PRIVATE)
// RequestSlot::registration_ is private. Ordinary code cannot close/open the
// waiter registration directly — only reap/register_waiter/cancel_waiter may.
void neg_slot_registration_private() {
    RequestSlot s;
    s.registration_ = sluice::async::detail::WaiterRegistration::closed; // ERROR: private
}
#endif

int main() {
    positive_control();
    return 0;
}
