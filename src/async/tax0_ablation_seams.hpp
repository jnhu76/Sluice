// tax0_ablation_seams.hpp - NON-INSTALLED internal-testing seam header for
// the TAX-0D control-plane causal ablations (#250 / #259 / PR #260).
//
// Registers the R1 research variants of the two suspected seams frozen in
// research/tax0/TAX0-A2-CONTROL-PLANE-SEMANTIC-FLOOR.md:
//
//   F01  AsyncIoContext::submit_* evaluated backend_->outstanding() (arena
//        leaf lock) unconditionally even when stats are disabled. R1 gates
//        that evaluation on stats presence (identical semantics either way:
//        stats enabled -> identical accounting; stats disabled -> nothing
//        observable changes). R1 became the PRODUCTION behavior in #261;
//        this seam keeps the R0 (unconditional) arm reproducible for the
//        TAX-0D A/B measurement.
//   F02  Completion::publish_from_reap() stamps the process-global reap
//        sequence (seq_cst RMW) on EVERY publication although only
//        Batch::next() consumes it. R1 skips the stamp for ordinary
//        publications. RESEARCH-ONLY LIMITATION: with the flag set the seam
//        build is not Batch-safe (Batch::next() requires stamps); it exists
//        for the causal A/B measurement only and is never a production
//        candidate as-is.
//   F07  UringAsyncBackend per-op router extent probes: every per-op probe
//        site recomputes router_.size() from the vector header although the
//        router is construction-fixed at request_capacity and never
//        resized. R1 reads a construction-cached extent instead
//        (RE-H0-ATTR-B-PREREGISTRATION.md A4). Semantics-identical by the
//        invariance fact (the cached value equals router_.size() on every
//        path); the mode flag is checked per call (conservative — the R1
//        arm pays the branch). R0 remains the production behavior.
//
// The production targets never define SLUICE_ASYNC_INTERNAL_TESTING and
// compile none of this. Default mode in every build is R0; the ablation
// bench installs R1 through tax0_ablation_modes() before driving I/O.
// For F01, production has been R1-shaped since #261 (stats-gated
// evaluation), so this seam's R0 arm exists to reproduce the pre-#261
// baseline; for F02, R0 remains the production behavior.
//
// Definitions are C++17 inline (vague linkage): every internal-testing TU
// that compiles the guarded production sources sees the one shared storage
// without needing a dedicated seam TU to link against — dedicated
// uring/async test targets compile src/async/*.cpp subsets directly and
// must keep linking standalone.
#pragma once

#include <cstdint>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

namespace sluice::async::detail {

struct Tax0AblationModes {
    // F01 R1: submit paths skip backend_->outstanding() when stats == nullptr.
    bool f01_gate_outstanding_eval = false;
    // F02 R1: ordinary (non-Batch) publications skip the reap-seq stamp.
    bool f02_skip_reap_seq = false;
    // F07 R1 (RE-H0 ATTR-B): per-op router extent probes read the
    // construction-cached extent instead of recomputing router_.size().
    bool f07_skip_extent_reprobes = false;
};

inline Tax0AblationModes g_tax0_ablation_modes{};

inline Tax0AblationModes& tax0_ablation_modes() noexcept {
    return g_tax0_ablation_modes;
}
inline bool tax0_f01_gate_outstanding_eval() noexcept {
    return g_tax0_ablation_modes.f01_gate_outstanding_eval;
}
inline bool tax0_f02_skip_reap_seq() noexcept {
    return g_tax0_ablation_modes.f02_skip_reap_seq;
}
inline bool tax0_f07_skip_extent_reprobes() noexcept {
    return g_tax0_ablation_modes.f07_skip_extent_reprobes;
}

}  // namespace sluice::async::detail

#endif  // SLUICE_ASYNC_INTERNAL_TESTING
