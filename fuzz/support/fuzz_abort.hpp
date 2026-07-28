// Fuzz-harness failure primitive.
//
// One narrow, sanitizer-visible abort for invariant violations inside
// LLVMFuzzerTestOneInput. Using __builtin_trap() (rather than abort()) keeps
// the fault site precise for libFuzzer crash reporting and avoids pulling in
// signal-handling paths. Never use the project unit-test harness inside a fuzz
// target — this is the only assertion primitive they get.
#pragma once

namespace fuzz {

[[noreturn]] inline void fuzz_fail() noexcept { __builtin_trap(); }

} // namespace fuzz

// Convenience macro so call sites read as assertions but resolve to a trap.
// Prints the failing condition and source location to stderr before trapping so
// a crash can be triaged even when the libFuzzer signal handler does not print a
// full sanitizer stack trace (e.g. a non-ASan build of the target).
#include <cstdio>
#define FUZZ_ASSERT(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FUZZ_ASSERT FAILED: %s @ %s:%d\n", #cond, __FILE__, __LINE__);   \
            std::fflush(stderr);                                                                   \
            ::fuzz::fuzz_fail();                                                                   \
        }                                                                                          \
    } while (false)
