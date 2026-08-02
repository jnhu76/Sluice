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
// compile-time check that a value type satisfying all four traits is accepted.
#include <sluice/async/completion.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <utility>

using namespace sluice::async;
using sluice::Result;

// A value type that satisfies every Completion<T> trait (positive compile
// case). Keep this small: it exists to prove the contract is satisfiable and
// that a conforming type instantiates cleanly.
struct NothrowValue {
    int v = 0;
    NothrowValue() noexcept = default;
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
// must fail to compile via the static_assert traits in completion.hpp.
struct ThrowingMoveValue {
    ThrowingMoveValue() noexcept = default;
    ThrowingMoveValue(ThrowingMoveValue&&) noexcept = default;
    ThrowingMoveValue& operator=(ThrowingMoveValue&&) noexcept(false) {}  // throws
    ~ThrowingMoveValue() noexcept = default;
};
void neg_throwing_completion_value() {
    // ERROR: static_assert in Completion<T> (nothrow move-assignable trait).
    Completion<ThrowingMoveValue> c;
    (void)c;
}
#endif

int main() {
    positive_control();
    return 0;
}
