// completion_authority_negative_compile_probe.cpp
//
// ADR-explicit-io-completion-authority negative-compile probe.
//
// Each NEG_<KIND> macro selects ONE forbidden usage that ordinary application
// code must NOT be able to compile. The verify script compiles this file with
// exactly one NEG_* macro defined at a time and asserts the compile FAILS with
// a specific diagnostic:
//   - access-control cases: a private-access / no-member diagnostic;
//   - NEG_THROWING_COMPLETION_VALUE: a static_assert failure (a value type
//     that violates the Completion<T> noexcept value-type contract).
//
// Without any NEG_* macro, this file compiles cleanly (positive control): it
// exercises only the public caller-facing API (idle/outstanding/ready/result/
// reset), and it also instantiates Completion<NothrowValue> as a positive
// compile-time check that a value type satisfying every Completion<T> trait
// (nothrow default-constructible, copy-constructible, nothrow move-assignable,
// nothrow destructible) is accepted. The full claim -> publish -> result()
// round-trip for a NothrowValue is exercised at runtime in
// async_completion_test.cpp (where a backend is available to drive it).
#include <sluice/async/completion.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <utility>

using namespace sluice::async;
using sluice::Result;

// A value type that satisfies every Completion<T> trait (positive compile
// case). Keep this small: it exists to prove the contract is satisfiable and
// that a conforming type instantiates cleanly. It is deliberately NOT move-
// only: result() returns the stored result by value, so T must be
// copy-constructible (ADR-explicit-io-completion-authority §4).
struct NothrowValue {
    int v = 0;
    NothrowValue() noexcept = default;
    NothrowValue(const NothrowValue&) noexcept = default;
    NothrowValue& operator=(const NothrowValue&) noexcept = default;
    NothrowValue(NothrowValue&&) noexcept = default;
    NothrowValue& operator=(NothrowValue&&) noexcept = default;
    ~NothrowValue() noexcept = default;
};

// Positive control: public API only. Always compiles. Also instantiates
// Completion<NothrowValue> so the value-type contract has a positive compile
// witness (not only negative cases).
void positive_control() {
    Completion<std::size_t> c;
    (void)c.idle();
    (void)c.outstanding();
    (void)c.ready();
    // reset from idle is a no-op (defensive).
    c.reset();

    Completion<NothrowValue> cv;
    (void)cv.idle();
    cv.reset();
}

#if defined(NEG_MARK_OUTSTANDING)
// mark_outstanding() was removed. Ordinary code cannot forge a claim.
void neg_mark_outstanding() {
    Completion<std::size_t> c;
    c.mark_outstanding();  // ERROR: no member named 'mark_outstanding'
}
#endif

#if defined(NEG_COMPLETE_WITH)
// complete_with() was removed. Ordinary code cannot forge a publication.
void neg_complete_with() {
    Completion<std::size_t> c;
    c.complete_with(Result<std::size_t>{42});  // ERROR: no member named 'complete_with'
}
#endif

#if defined(NEG_TRY_CLAIM_PRIVATE)
// try_claim_for_backend() is private (friend AsyncBackend only).
void neg_try_claim_private() {
    Completion<std::size_t> c;
    c.try_claim_for_backend();  // ERROR: 'try_claim_for_backend' is private
}
#endif

#if defined(NEG_PUBLISH_PRIVATE)
// publish_from_reap() is private (friend AsyncBackend only).
void neg_publish_private() {
    Completion<std::size_t> c;
    c.publish_from_reap(Result<std::size_t>{42});  // ERROR: 'publish_from_reap' is private
}
#endif

#if defined(NEG_ROLLBACK_PRIVATE)
// rollback_claim_before_accept() is private (friend AsyncBackend only).
void neg_rollback_private() {
    Completion<std::size_t> c;
    c.rollback_claim_before_accept();  // ERROR: 'rollback_claim_before_accept' is private
}
#endif

#if defined(NEG_REAP_SEQ_PRIVATE)
// reap_seq() is private (friend Batch only).
void neg_reap_seq_private() {
    Completion<std::size_t> c;
    (void)c.reap_seq();  // ERROR: 'reap_seq' is private
}
#endif

#if defined(NEG_THROWING_COMPLETION_VALUE)
// A value type whose move-assignment may throw violates the Completion<T>
// noexcept value-type contract. Instantiating Completion<ThrowingMoveValue>
// must fail to compile via the static_assert traits in completion.hpp. It is
// copy-constructible and nothrow move-constructible so that ONLY the
// move-assignable trait fails (no unrelated deleted-copy-constructor noise),
// and its throwing move-assign operator returns *this so the compile failure
// is attributable to the static_assert, not to -Wreturn-type.
struct ThrowingMoveValue {
    ThrowingMoveValue() noexcept = default;
    ThrowingMoveValue(const ThrowingMoveValue&) noexcept = default;
    ThrowingMoveValue(ThrowingMoveValue&&) noexcept = default;
    ThrowingMoveValue& operator=(ThrowingMoveValue&&) noexcept(false) { return *this; }  // throws
    ~ThrowingMoveValue() noexcept = default;
};
void neg_throwing_completion_value() {
    // ERROR: static_assert in Completion<T> (nothrow move-assignable trait).
    Completion<ThrowingMoveValue> c;
    (void)c;
}
#endif

// --- Phase B binding-protocol negative-compile cases -------------------------
// ADR-explicit-io-request-contract (Accepted) Decision 5 / I2 / I15: the
// binding mutators (begin_binding_for_backend / commit_binding_to_outstanding /
// rollback_binding_before_accept) are PRIVATE to Completion<T>, reachable only
// via AsyncBackend's protected helpers by derived backends (the trusted
// backend-author role). Ordinary application code cannot forge a binding,
// observe a half-installed payload, or roll one back. Each case below must fail
// with a private-access / no-member diagnostic, exactly like the claim/publish
// cases above.

#if defined(NEG_BEGIN_BINDING_PRIVATE)
void neg_begin_binding_private() {
    Completion<std::size_t> c;
    c.begin_binding_for_backend();  // ERROR: private
}
#endif

#if defined(NEG_COMMIT_BINDING_PRIVATE)
void neg_commit_binding_private() {
    Completion<std::size_t> c;
    c.commit_binding_to_outstanding();  // ERROR: private
}
#endif

#if defined(NEG_ROLLBACK_BINDING_PRIVATE)
void neg_rollback_binding_private() {
    Completion<std::size_t> c;
    c.rollback_binding_before_accept();  // ERROR: private
}
#endif

int main() {
    positive_control();
    return 0;
}
