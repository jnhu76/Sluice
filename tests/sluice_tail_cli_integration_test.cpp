// sluice-tail CLI end-to-end integration tests: fork/exec the REAL
// sluice-tail binary (built as a target dependency, run from the binary
// directory like the sluice-copy CLI tests) and drive it against real files:
//   - finite tail: correct stdout, exit 0;
//   - follow: appends from the parent are delivered to the child's stdout,
//     then SIGINT ends it CLEANLY (exit 0) through the sigwait ->
//     request_stop -> drain/join path — the brief's cancellation acceptance
//     requirement at the process level;
//   - usage/open errors: exit 1 / exit 2.
#include "harness.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kBin = "./sluice-tail";

struct TempLog {
    int fd;
    std::string path;
    TempLog(const char* tag) {
        std::string tmpl = std::string("/tmp/sluice_tail_cli_") + tag +
                           "_XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        fd = ::mkstemp(buf.data());
        SLUICE_CHECK(fd >= 0);
        path = buf.data();
    }
    ~TempLog() {
        if (fd >= 0) ::close(fd);
        ::unlink(path.c_str());
    }
    void write_all(const std::string& s) {
        SLUICE_CHECK(::pwrite(fd, s.data(), s.size(), 0) ==
                     static_cast<ssize_t>(s.size()));
    }
    TempLog(const TempLog&) = delete;
    TempLog& operator=(const TempLog&) = delete;
};

// Spawn sluice-tail with argv; pipe the child's stdout; parent reads until
// the child exits (or an expected line arrives).
struct Child {
    pid_t pid;
    int out_fd;  // read end of the child's stdout pipe
};

bool spawn_tail(Child& c, std::vector<std::string> args) {
    int p[2];
    if (::pipe(p) != 0) return false;
    c.pid = ::fork();
    if (c.pid < 0) {
        ::close(p[0]);
        ::close(p[1]);
        return false;
    }
    if (c.pid == 0) {
        ::close(p[0]);
        ::dup2(p[1], STDOUT_FILENO);
        ::close(p[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(kBin));
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execv(kBin, argv.data());
        ::_Exit(127);
    }
    ::close(p[1]);
    c.out_fd = p[0];
    return true;
}

// Bounded read until EOF or `budget_ms` elapsed. Returns bytes read.
std::string drain_stdout(int fd, int budget_ms) {
    std::string out;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(budget_ms);
    for (;;) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        timeval tv{0, 100 * 1000};  // 100ms slices; bounded, not busy spin
        int r = ::select(fd + 1, &rf, nullptr, nullptr, &tv);
        if (r > 0) {
            char buf[4096];
            ssize_t n = ::read(fd, buf, sizeof buf);
            if (n <= 0) break;
            out.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (r < 0 && errno != EINTR) break;
    }
    return out;
}

int wait_child(pid_t pid, int budget_ms) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(budget_ms);
    int status = -1;
    for (;;) {
        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            (void)::waitpid(pid, &status, 0);
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}  // namespace

SLUICE_TEST_CASE(tail_cli_finite_last_n) {
    TempLog f("finite");
    f.write_all("a\nb\nc\nd\ne\n");
    Child c;
    SLUICE_CHECK(spawn_tail(c, {"-n", "2", f.path}));
    std::string out = drain_stdout(c.out_fd, 5000);
    ::close(c.out_fd);
    int code = wait_child(c.pid, 5000);
    SLUICE_CHECK(code == 0);
    SLUICE_CHECK(out == "d\ne\n");
}

SLUICE_TEST_CASE(tail_cli_missing_file_exit_two) {
    Child c;
    SLUICE_CHECK(spawn_tail(c, {"/no/such/file"}));
    std::string out = drain_stdout(c.out_fd, 5000);
    ::close(c.out_fd);
    int code = wait_child(c.pid, 5000);
    SLUICE_CHECK(code == 2);
    SLUICE_CHECK(out.empty());
}

SLUICE_TEST_CASE(tail_cli_usage_error_exit_one) {
    Child c;
    SLUICE_CHECK(spawn_tail(c, {}));
    std::string out = drain_stdout(c.out_fd, 5000);
    ::close(c.out_fd);
    int code = wait_child(c.pid, 5000);
    SLUICE_CHECK(code == 1);
    SLUICE_CHECK(out.empty());
}

SLUICE_TEST_CASE(tail_cli_follow_append_then_sigint_exits_zero) {
    TempLog f("follow");
    f.write_all("seed-1\nseed-2\n");

    Child c;
    SLUICE_CHECK(spawn_tail(c, {"-f", "-n", "1", "--poll-interval", "50",
                                f.path}));

    // Phase 1: the initial tail line arrives.
    {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(c.out_fd, &rf);
        timeval tv{5, 0};
        int r = ::select(c.out_fd + 1, &rf, nullptr, nullptr, &tv);
        SLUICE_CHECK(r > 0);
        char buf[256];
        ssize_t n = ::read(c.out_fd, buf, sizeof buf);
        SLUICE_CHECK(n > 0);
        SLUICE_CHECK(std::string(buf, static_cast<std::size_t>(n)) ==
                     "seed-2\n");
    }

    // Phase 2: an append is delivered (bounded wait for exactly one line).
    {
        std::string app = "live-line\n";
        SLUICE_CHECK(::pwrite(f.fd, app.data(), app.size(), 14) ==
                     static_cast<ssize_t>(app.size()));
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(c.out_fd, &rf);
        timeval tv{5, 0};
        int r = ::select(c.out_fd + 1, &rf, nullptr, nullptr, &tv);
        SLUICE_CHECK(r > 0);
        char buf[256];
        ssize_t n = ::read(c.out_fd, buf, sizeof buf);
        SLUICE_CHECK(n > 0);
        SLUICE_CHECK(std::string(buf, static_cast<std::size_t>(n)) ==
                     "live-line\n");
    }

    // Phase 3: SIGINT ends the follow cleanly through the sigwait ->
    // request_stop -> drain/join path. Exit code 0 (the documented normal
    // end of tail -f); the pipe then reaches EOF.
    SLUICE_CHECK(::kill(c.pid, SIGINT) == 0);
    int code = wait_child(c.pid, 5000);
    ::close(c.out_fd);
    SLUICE_CHECK(code == 0);
}

SLUICE_MAIN()
