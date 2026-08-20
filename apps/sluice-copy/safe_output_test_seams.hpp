// App-private deterministic test seam for the directory fsync in
// commit_atomic_copy (issue #142 EINTR-001 regression).
//
// Compiled ONLY into test targets that define SLUICE_COPY_INTERNAL_TESTING:
// safe_output.cpp includes this header under the same guard, so the
// production sluice-copy target never compiles any seam code or state (the
// persistence contract of the src/async *_test_seams.hpp pattern, scoped
// app-locally). No linker interposition and no signals: with no script
// armed, directory_fsync performs the plain ::fsync syscall, so the real
// filesystem cases in the same test binary are unaffected.
//
// Single-threaded by design: commit_atomic_copy runs on the caller's thread
// and scripts arm/disarm on that same thread, so the active-script slot
// needs no synchronization.
#pragma once

#include <cerrno>
#include <cstddef>
#include <utility>
#include <vector>

namespace sluice_copy::testing {

// Scripts the directory fsync for one test case: construction arms the seam
// (nested arming stacks guard-style), destruction disarms, so a case that
// bare-returns through SLUICE_CHECK still leaves no residue for later cases.
class DirFsyncScript {
  public:
    struct Step {
        int ret;  // fsync return value for this call
        int err;  // errno to set before returning it
    };

    explicit DirFsyncScript(std::vector<Step> steps)
        : steps_(std::move(steps)), prev_(active()) {
        active() = this;
    }
    ~DirFsyncScript() {
        if (active() == this) {
            active() = prev_;
        }
    }
    DirFsyncScript(const DirFsyncScript&) = delete;
    DirFsyncScript& operator=(const DirFsyncScript&) = delete;

    // The armed script for directory_fsync's seam branch (null => the real
    // ::fsync runs). Reference-returning so construction/destruction can
    // re-arm the slot.
    static DirFsyncScript*& active() {
        static DirFsyncScript* armed = nullptr;
        return armed;
    }

    // One scripted call: sets errno from the step, then returns the step's
    // return value. Beyond the scripted sequence the seam returns -1/EBADF —
    // a terminal non-EINTR result — so an unexpected extra call can neither
    // spin an EINTR retry nor pass silently.
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
    DirFsyncScript* prev_ = nullptr;
    std::size_t pos_ = 0;
    std::size_t calls_ = 0;
};

}  // namespace sluice_copy::testing
