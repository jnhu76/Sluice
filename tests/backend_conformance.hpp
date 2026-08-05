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
struct BackendFactory {
    using MakeBackend = std::function<std::unique_ptr<sluice::async::AsyncBackend>()>;
    using MakeTempFd = std::function<int()>;  // returns an open rw fd; -1 if unsupported

    const char* name;
    MakeBackend make_backend;
    MakeTempFd make_temp_fd;  // may be nullptr when not real_mode
    bool real_mode;           // can do real kernel I/O
    const char* profile;      // Phase C1 closed-profile classification
    const char* mode;         // Phase C1 execution mode classification
};

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

}  // namespace sluice_test::conformance
