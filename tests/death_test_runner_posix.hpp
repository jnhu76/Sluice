// death_test_runner_posix.hpp — minimal POSIX child-process death-test harness.
//
// Built for ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1 §F. The existing
// tests/harness.hpp test framework is cooperative (SLUICE_CHECK returns from
// the case on failure) and therefore cannot survive a std::terminate in the
// test process. These tests instead fork/exec a child that exercises the real
// production Mutex entry; the child installs a deterministic terminate handler
// (so the exit code is fixed, not a SIGABRT that varies by stdlib / sanitizer
// / user handler); the parent waitpid()s and asserts the exact exit code.
//
// Self-exec discipline (§F1): the test binary re-execs itself with
// `--death-child=<case>` to run the child body. This keeps the child linked
// against exactly the same (internal-testing) library as the parent, and the
// production Mutex entry is the unit under test (no fake class, §N).
//
// PORTABILITY (§F3): POSIX only. fork/exec/waitpid are available on Linux and
// macOS. Windows is NOT RUN in this task (the harness is not implemented
// there); the death-test xmake target is gated to is_plat("linux","macosx").
//
// Exit-code protocol (deliberately small, fixed, sentinel-free):
//
//   kExpectedTerminateExit (86)
//       child reached std::terminate via the Mutex fail-fast boundary.
//   kUnexpectedReturnExit  (87)
//       child returned past a call that MUST terminate — the fail-fast
//       boundary did NOT fire. This is the failure we are testing against.
//   kChildTestFailExit      (88)
//       child internal assertion failed (e.g. control-path lock did not
//       succeed). Distinct from kUnexpectedReturnExit so logs name the cause.
//   other exit code / signal
//       unexpected crash or exec failure; reported verbatim by the parent.
#pragma once

#if defined(__unix__)

#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <iostream>
#include <string>
#include <vector>

namespace sluice_death_test {

inline constexpr int kExpectedTerminateExit = 86;
inline constexpr int kUnexpectedReturnExit  = 87;
inline constexpr int kChildTestFailExit      = 88;

// Marker argv[1] prefix that selects child-mode dispatch.
inline constexpr const char* kChildArgPrefix = "--death-child=";

// Install the deterministic terminate handler in the child. The handler MUST
// NOT return (§F3): std::_Exit never returns. Using std::_Exit (not exit)
// avoids running atexit/global destructors, which keeps the exit code stable
// and independent of global-state teardown ordering.
inline void install_deterministic_terminate_handler() noexcept {
    std::set_terminate([]() noexcept {
        std::_Exit(kExpectedTerminateExit);
    });
}

// Return the argv token for a child dispatching `case_name`, e.g.
// "--death-child=T1".
inline std::string child_arg(const std::string& case_name) {
    return std::string(kChildArgPrefix) + case_name;
}

// Extract the case name from argv[1] if it matches the child marker, else
// return the empty string (meaning: run as parent).
inline std::string parse_child_case(int argc, char** argv) {
    if (argc < 2) return {};
    std::string a(argv[1]);
    const std::string p(kChildArgPrefix);
    if (a.compare(0, p.size(), p) != 0) return {};
    return a.substr(p.size());
}

// Fork a child that re-execs this binary with `--death-child=<case_name>` and
// wait for it. Uses a non-blocking poll with a 60-second watchdog so that a
// child stuck in a hang (e.g. a destructor that should have fail-fasted but
// instead joined a paused worker) does not block the parent and drag the CI
// target forever. The watchdog sends SIGKILL on timeout.
inline int fork_exec_child(const std::string& case_name, bool& timed_out) {
    timed_out = false;
    pid_t pid = fork();
    if (pid < 0) {
        std::perror("death_test: fork");
        return -1;
    }
    if (pid == 0) {
        // Child: re-exec self so the child process is the same binary, linked
        // against the same library, with the production Mutex entry under test.
        std::vector<char*> argv;
        std::string self_buf;
        {
            char buf[4096];
            ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
            if (n <= 0) {
                std::perror("death_test: readlink /proc/self/exe");
                std::_Exit(kChildTestFailExit);
            }
            buf[n] = '\0';
            self_buf = buf;
        }
        std::string arg1 = child_arg(case_name);
        argv.push_back(const_cast<char*>(self_buf.c_str()));
        argv.push_back(const_cast<char*>(arg1.c_str()));
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        std::perror("death_test: execv");
        std::_Exit(kChildTestFailExit);
    }
    // Parent: bounded wait with a 60-second watchdog.
    const auto start = std::chrono::steady_clock::now();
    constexpr auto kTimeout = std::chrono::seconds(60);
    for (;;) {
        int status = 0;
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) return status;
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) {
            std::perror("death_test: waitpid");
            return -1;
        }
        // w == 0: child still running; check the timeout.
        if (std::chrono::steady_clock::now() - start >= kTimeout) {
            timed_out = true;
            std::cerr << "[death] child " << case_name << " (pid " << pid
                      << ") exceeded 60s timeout; sending SIGKILL\n";
            if (::kill(pid, SIGKILL) != 0 && errno != ESRCH) {
                std::perror("death_test: kill");
            }
            // Reap the killed child (should return almost immediately).
            for (;;) {
                w = waitpid(pid, &status, 0);
                if (w == pid) return status;
                if (w < 0 && errno == EINTR) continue;
                if (w < 0) {
                    std::perror("death_test: waitpid after SIGKILL");
                    return -1;
                }
            }
        }
        usleep(10000);  // 10ms poll interval while child is alive
    }
}

// Result of a single death-case run, for reporting.
struct DeathResult {
    std::string case_name;
    bool        spawned       = false;  // fork/exec/waitpid succeeded
    bool        signaled      = false;  // WIFSIGNALED
    int         signal_number = 0;      // valid if signaled
    int         exit_code     = -1;     // valid if !signaled && spawned
    bool        timed_out     = false;  // parent killed child after timeout
};

inline DeathResult run_death_case(const std::string& case_name) {
    DeathResult r;
    r.case_name = case_name;
    bool timed_out = false;
    int status = fork_exec_child(case_name, timed_out);
    r.timed_out = timed_out;
    if (status < 0) {
        if (!timed_out) {
            // fork/exec/waitpid error (not timeout) — r.spawned stays false.
        }
        return r;
    }
    r.spawned = true;
    if (WIFSIGNALED(status)) {
        r.signaled = true;
        r.signal_number = WTERMSIG(status);
    } else if (WIFEXITED(status)) {
        r.exit_code = WEXITSTATUS(status);
    }
    return r;
}

// Parent-side assertion for a case that MUST terminate via the Mutex fail-fast
// boundary. Returns true iff the child exited normally with the expected
// terminate code (86).
inline bool expect_terminated_via_fail_fast(const DeathResult& r) {
    if (r.timed_out) {
        std::cerr << "[death] " << r.case_name
                  << ": FAIL (child exceeded watchdog timeout and was killed; "
                     "the destructor likely hung instead of fail-fasting)\n";
        return false;
    }
    if (!r.spawned) {
        std::cerr << "[death] " << r.case_name << ": FAIL (fork/exec/waitpid error)\n";
        return false;
    }
    if (r.signaled) {
        std::cerr << "[death] " << r.case_name << ": FAIL (signaled by "
                  << strsignal(r.signal_number) << "; expected terminate exit "
                  << kExpectedTerminateExit << ")\n";
        return false;
    }
    if (r.exit_code != kExpectedTerminateExit) {
        std::cerr << "[death] " << r.case_name << ": FAIL (exit=" << r.exit_code
                  << "; expected terminate exit " << kExpectedTerminateExit
                  << ". exit=" << kUnexpectedReturnExit
                  << " means the call returned instead of terminating)\n";
        return false;
    }
    std::cerr << "[death] " << r.case_name
              << ": PASS (terminated via Mutex fail-fast boundary, exit="
              << kExpectedTerminateExit << ")\n";
    return true;
}

// Parent-side assertion for the CONTROL case (T4): the child must exit 0
// (everything succeeded with no fault armed).
inline bool expect_normal_exit_zero(const DeathResult& r) {
    if (r.timed_out) {
        std::cerr << "[death] " << r.case_name
                  << ": FAIL (child exceeded watchdog timeout and was killed; "
                     "the control case should exit promptly)\n";
        return false;
    }
    if (!r.spawned) {
        std::cerr << "[death] " << r.case_name << ": FAIL (fork/exec/waitpid error)\n";
        return false;
    }
    if (r.signaled) {
        std::cerr << "[death] " << r.case_name
                  << ": FAIL (signaled by " << strsignal(r.signal_number)
                  << "; control case must not crash)\n";
        return false;
    }
    if (r.exit_code != 0) {
        std::cerr << "[death] " << r.case_name << ": FAIL (exit=" << r.exit_code
                  << "; control case must exit 0)\n";
        return false;
    }
    std::cerr << "[death] " << r.case_name
              << ": PASS (control: lock/try_lock/cv-wait all succeeded)\n";
    return true;
}

}  // namespace sluice_death_test

#endif  // defined(__unix__)
