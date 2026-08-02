// completion_authority_negative_compile_probe.cpp
//
// ADR-explicit-io-completion-authority negative-compile probe.
//
// Each NEG_<KIND> macro selects ONE forbidden usage that ordinary application
// code must NOT be able to compile. The verify script compiles this file with
// exactly one NEG_* macro defined at a time and asserts the compile FAILS with
// a private-access or no-member diagnostic.
//
// Without any NEG_* macro, this file compiles cleanly (positive control): it
// exercises only the public caller-facing API (idle/outstanding/ready/result/
// reset).
#include <sluice/async/completion.hpp>
#include <sluice/result.hpp>

#include <cstddef>

using namespace sluice::async;
using sluice::Result;

// Positive control: public API only. Always compiles.
void positive_control() {
    Completion<std::size_t> c;
    (void)c.idle();
    (void)c.outstanding();
    (void)c.ready();
    // reset from idle is a no-op (defensive).
    c.reset();
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

#if defined(NEG_REAP_SEQ_PRIVATE)
// reap_seq() is private (friend Batch only).
void neg_reap_seq_private() {
    Completion<std::size_t> c;
    (void)c.reap_seq();  // ERROR: 'reap_seq' is private
}
#endif

int main() {
    positive_control();
    return 0;
}
