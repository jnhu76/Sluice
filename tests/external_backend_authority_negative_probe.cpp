// external_backend_authority_negative_probe.cpp
//
// Phase C1 negative-compile probe — AsyncBackend protected-helper authority.
//
// SCOPE (narrow, per review): the existing completion-authority and
// request-arena negative-compile gates already prove that ordinary code
// cannot publish/claim via Completion<T> privates and cannot mutate
// RequestSlot private fields. This probe closes ONLY the narrower gap that
// remains: the AsyncBackend PROTECTED publication helpers (try_claim /
// publish / rollback_claim_before_accept / begin_binding / ...) are
// inaccessible to NON-DERIVED ordinary code. A class that does not inherit
// from AsyncBackend must not be able to call them.
//
// Each NEG_<KIND> macro selects ONE forbidden usage. The verify script
// compiles this file with exactly one NEG_* macro defined at a time and
// asserts the compile FAILS with a private-access / inaccessible /
// protected diagnostic. With NO NEG_* macro this file compiles cleanly as a
// positive control (it only exercises what a legitimate DERIVED backend may
// do, plus the public caller-facing Completion API).
//
// Header policy: public installed headers only, like its positive twin
// (external_backend_admission_test.cpp).
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <utility>

using namespace sluice::async;
using sluice::Result;

// A legitimate DERIVED backend — the authorized role. Used by the positive
// control and as the reference for what IS permitted.
class LegitimateBackend : public AsyncBackend {
  public:
    Result<void> submit_read(ReadOp, Completion<std::size_t>&) override {
        return make_unexpected<void>(sluice::IoError{sluice::IoError::Code::invalid_state});
    }
    Result<void> submit_write(WriteOp, Completion<std::size_t>&) override {
        return make_unexpected<void>(sluice::IoError{sluice::IoError::Code::invalid_state});
    }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&) override {
        return make_unexpected<void>(sluice::IoError{sluice::IoError::Code::invalid_state});
    }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&) override {
        return make_unexpected<void>(sluice::IoError{sluice::IoError::Code::invalid_state});
    }
    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override { return Result<std::size_t>{0}; }
    std::size_t outstanding() const noexcept override { return 0; }
};

// Positive control: a derived backend reaches the helpers through inheritance
// (the sanctioned backend-author capability), and ordinary code uses the public
// Completion API. Always compiles with no NEG_* macro.
void positive_control() {
    LegitimateBackend b;
    AsyncIoContext ctx(std::make_unique<LegitimateBackend>());
    (void)ctx.outstanding();
    Completion<std::size_t> c;
    (void)c.idle();
    c.reset();
    (void)b.outstanding();
}

// --- Negative cases: a NON-derived class attempts the protected helpers -----
// AsyncBackend::try_claim / publish are `protected static` reachable only from
// a derived class's member function. An ordinary non-backend class cannot name
// them. Each case below must fail to compile with a private / protected /
// inaccessible diagnostic.

#if defined(NEG_TRY_CLAIM_AS_NON_BACKEND)
// An ordinary class (NOT derived from AsyncBackend) tries to call the
// protected static try_claim helper directly.
struct OrdinaryClient {
    void try_claim_from_ordinary_code(Completion<std::size_t>& c) {
        // ERROR: 'try_claim' is a protected member of AsyncBackend and
        // OrdinaryClient does not derive from it.
        AsyncBackend::try_claim(c);
    }
};
void neg_try_claim_as_non_backend() {
    OrdinaryClient oc;
    Completion<std::size_t> c;
    oc.try_claim_from_ordinary_code(c);
}
#endif

#if defined(NEG_PUBLISH_AS_NON_BACKEND)
// An ordinary class (NOT derived from AsyncBackend) tries to call the
// protected static publish helper directly.
struct OrdinaryPublisher {
    void publish_from_ordinary_code(Completion<std::size_t>& c) {
        // ERROR: 'publish' is a protected member of AsyncBackend and
        // OrdinaryPublisher does not derive from it.
        AsyncBackend::publish(c, Result<std::size_t>{1});
    }
};
void neg_publish_as_non_backend() {
    OrdinaryPublisher op;
    Completion<std::size_t> c;
    op.publish_from_ordinary_code(c);
}
#endif

int main() {
    positive_control();
    return 0;
}
