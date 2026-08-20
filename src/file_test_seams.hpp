// Core-private deterministic test seam for the close(2) indirection in
// src/file.cpp (issue #143 ERR-001 regression).
//
// Compiled ONLY into test targets that define SLUICE_FILE_INTERNAL_TESTING:
// file.cpp includes this header under the same guard, so the production
// sluice_core target never compiles any seam code or state (the persistence
// contract of the src/async *_test_seams.hpp pattern, scoped to the core file
// backend). No linker interposition and no signals: with no script armed,
// close_fd performs the plain ::close syscall, so real-file cases in the same
// test binary are unaffected.
//
// Single-threaded by design: FileReader/FileWriter close on the caller's
// thread and scripts arm/disarm on that same thread, so the active-script
// slot needs no synchronization.
#pragma once

#include <cerrno>
#include <cstddef>
#include <utility>
#include <vector>

namespace sluice::file_testing {

// Scripts close(2) for one test case: construction arms the seam (nested
// arming stacks guard-style), destruction disarms, so a case that bare-returns
// through SLUICE_CHECK still leaves no residue for later cases.
class CloseScript {
  public:
    struct Step {
        int ret;  // close return value for this call
        int err;  // errno to set before returning it
    };

    explicit CloseScript(std::vector<Step> steps)
        : steps_(std::move(steps)), prev_(active()) {
        active() = this;
    }
    ~CloseScript() {
        if (active() == this) {
            active() = prev_;
        }
    }
    CloseScript(const CloseScript&) = delete;
    CloseScript& operator=(const CloseScript&) = delete;

    // The armed script for close_fd's seam branch (null => the real ::close
    // runs). Reference-returning so construction/destruction can re-arm the
    // slot.
    static CloseScript*& active() {
        static CloseScript* armed = nullptr;
        return armed;
    }

    // One scripted call: sets errno from the step, then returns the step's
    // return value. Beyond the scripted sequence the seam returns -1/EBADF —
    // a terminal result — so an unexpected extra close cannot pass silently.
    // (Note: exhaustion-as-EBADF is indistinguishable from a real EBADF close
    // failure; the cases() assertions are what distinguish "the test's extra
    // close" from "the implementation double-closed" — always assert calls().)
    int next(int /*fd*/) {
        ++calls_;
        if (pos_ >= steps_.size()) {
            errno = EBADF;
            return -1;
        }
        Step s = steps_[pos_++];
        errno = s.err;
        return s.ret;
    }

    std::size_t calls() const { return calls_; }

  private:
    std::vector<Step> steps_;
    CloseScript* prev_ = nullptr;
    std::size_t pos_ = 0;
    std::size_t calls_ = 0;
};

}  // namespace sluice::file_testing
