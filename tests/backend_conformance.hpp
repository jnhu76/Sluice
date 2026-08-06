// Shared AsyncBackend conformance suite (sluice-CORE-024, B1).
//
// One parameterized harness exercising every genuinely-shared backend semantic
// against every backend (Fake, ThreadPool, Uring-real, Uring-stub). Backend-
// specific mechanism (io_uring SQE pressure, ThreadPool worker count) stays in
// backend-specific tests. Per task §6: assert shared SEMANTIC outcomes, never
// require backend-specific PHYSICAL behavior to be identical.
//
// Usage: a backend test instantiates this with a factory + a temp-fd provider
// and calls run_conformance(). Cases that need a real fd (read/write content
// verification) are skipped when the backend is not "real" (Fake has no kernel;
// Uring stub returns backend_error). The skip is queried via `real_mode`.
#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/op_helpers.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

namespace sluice_test::conformance {

// A factory that produces a fresh backend, plus a way to open a temp fd the
// backend can do real I/O on (only used when real_mode). real_mode=false means
// the backend cannot do real I/O (Fake, or Uring stub without liburing) — the
// fd-backed cases are skipped cleanly.
//
// Phase C1 classification fields (profile + mode): these are the stable,
// machine-readable identity the aggregate gate reads via [conformance-meta]
// lines. They are HIGHER-LEVEL than real_mode (which still controls per-case
// skip logic inside the suite) and exist so the gate does not have to infer a
// backend's family/mode from display names or skip text. Values:
//   profile: one of ReferenceProfile / BlockingIoProfile / KernelIoProfile.
//   mode:    deterministic | real | stub.
//
// Phase C2a capacity seam: the OPTIONAL make_backend_with_capacity constructs a
// backend at a CHOSEN request_capacity. It is the authoritative way the shared
// capacity cases (run_capacity_cases) build a small bounded backend. A backend
// that has not migrated onto the bounded RequestArena (Uring before Phase D)
// leaves this null; the capacity cases then do not execute for that backend's
// driver, and the gap is recorded authoritatively by the manifest's
// not_implemented record + applicable_evidence_for_backend() — there is NO
// driver-side "skip-as-pass" and NO separate machine-readable INCOMPLETE marker
// protocol. The default zero-arg make_backend is preserved so the existing 8
// shared cases are unchanged.
struct BackendFactory {
    using MakeBackend =
        std::function<std::unique_ptr<sluice::async::AsyncBackend>()>;
    using MakeBackendWithCapacity =
        std::function<std::unique_ptr<sluice::async::AsyncBackend>(std::size_t)>;
    using MakeTempFd = std::function<int()>;  // returns an open rw fd; -1 if unsupported

    const char* name;
    MakeBackend make_backend;                       // default-capacity path
    MakeBackendWithCapacity make_backend_with_capacity = nullptr;  // optional
    MakeTempFd make_temp_fd;  // may be nullptr when not real_mode
    bool real_mode;           // can do real kernel I/O
    const char* profile;      // Phase C1 closed-profile classification
    const char* mode;         // Phase C1 execution mode classification
};

// Whether a factory can construct a backend at a chosen capacity. A factory
// that cannot is NOT a silent skip for the capacity cases: the SOLE
// authoritative gap record is the manifest entry
// (uring_capacity_not_implemented), carried into the verdict via
// applicable_evidence_for_backend(). The capacity runner observes a false
// result and does not register the capacity cases for that backend's driver.
inline bool factory_supports_capacity(const BackendFactory& f) {
    return f.make_backend_with_capacity != nullptr;
}

// Records a conformance skip (not a failure). Printed for visibility.
inline void note_skip(const char* backend, const char* case_name, const char* reason) {
    std::printf("[conformance] skip %s :: %s (%s)\n", backend, case_name, reason);
}

// Phase C1: emit a stable machine-readable identity line for a backend BEFORE
// its shared-suite case runs. The aggregate gate parses ONLY these
// [conformance-meta] lines to classify backends. Must be printed exactly once
// per registered backend, before any [conformance] skip/FAIL line for it.
inline void emit_meta(const BackendFactory& factory) {
    std::printf("[conformance-meta] backend=%s profile=%s mode=%s\n",
                factory.name, factory.profile, factory.mode);
}

// The shared suite. Returns 0 on full pass, 1 on any failure. Skips are not
// failures. Each case is self-contained: it builds its own context + buffers.
// Implemented out-of-line in backend_conformance_test.cpp so adding a case is
// one vertical slice (RED on Fake -> GREEN -> parameterize to other backends).
int run_conformance(const BackendFactory& factory);

// Phase C2a — shared capacity/admission/accounting cases. Drives ONLY the
// capacity cases against a backend built at a chosen small request_capacity via
// factory.make_backend_with_capacity. Returns:
//   * the empty string  — all capacity cases passed;
//   * a non-empty string — the stable name of the FIRST failing capacity case
//     (diagnosable; never just a bool or a whole-suite exit code, so a validity
//     fixture can assert the SPECIFIC case that caught a defect).
//
// Precondition: factory_supports_capacity(factory). A factory without the seam
// (Uring before Phase D) MUST NOT call this; the manifest's
// uring_capacity_not_implemented record is the authoritative gap surface.
// Implemented out-of-line in backend_conformance_test.cpp.
std::string run_capacity_cases(const BackendFactory& factory);

}  // namespace sluice_test::conformance
