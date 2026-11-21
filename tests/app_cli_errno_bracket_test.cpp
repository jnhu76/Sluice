#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr auto kChildDeadline = std::chrono::seconds(20);

int g_failed = 0;

void expect(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failed;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

std::string exe_dir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0)
        return {};
    buf[n] = '\0';
    const std::string full(buf);
    const std::size_t slash = full.rfind('/');
    return slash == std::string::npos ? std::string(".") : full.substr(0, slash);
}

std::string make_temp_dir() {
    std::string tmpl = "/tmp/sluice-errno-bracket-XXXXXX";
    if (::mkdtemp(tmpl.data()) == nullptr)
        return {};
    return tmpl;
}

struct ChildRun {
    bool exited = false;
    int code = -1;
    bool timed_out = false;
    std::string err;
};

ChildRun run_capture_stderr(const std::string& bin, const std::vector<std::string>& args) {
    int err_pipe[2];
    if (::pipe(err_pipe) != 0)
        return {};
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        return {};
    }
    if (pid == 0) {
        ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        ::close(STDOUT_FILENO);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(bin.c_str()));
        for (const std::string& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execv(bin.c_str(), argv.data());
        _exit(127);
    }
    ::close(err_pipe[1]);

    const auto deadline = std::chrono::steady_clock::now() + kChildDeadline;
    ChildRun run;
    const int fd = err_pipe[0];
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    bool eof = false;
    while (!eof && !run.timed_out) {
        char buf[4096];
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) {
            run.err.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            eof = true;
            break;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN) {
            eof = true;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            run.timed_out = true;
            break;
        }
        pollfd pfd{fd, POLLIN, 0};
        ::poll(&pfd, 1, 100);
    }
    while (!run.timed_out) {
        int status = 0;
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            run.exited = WIFEXITED(status) != 0;
            run.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            break;
        }
        if (r < 0 && errno != EINTR)
            break;
        if (std::chrono::steady_clock::now() >= deadline) {
            run.timed_out = true;
            break;
        }
        pollfd pfd{fd, POLLIN, 0};
        ::poll(&pfd, 1, 100);
    }
    if (run.timed_out) {
        ::kill(pid, SIGKILL);
        int status = 0;
        ::waitpid(pid, &status, 0);
    }
    ::close(fd);
    return run;
}

// Failure trigger: pread on /proc/self/mem at offset 0 fails with EIO after a
// successful open, reaching each binary's post-open read-error output path.
void expect_errno_bracket(const std::string& bin, const std::vector<std::string>& args,
                          const std::string& expected_stderr) {
    const ChildRun run = run_capture_stderr(bin, args);
    expect(!run.timed_out, bin + ": child deadline");
    expect(run.exited && run.code == 2, bin + ": exit 2, got " + std::to_string(run.code));
    expect(run.err == expected_stderr, bin + ": stderr mismatch, got: " + run.err);
}

}

int main() {
    const std::string dir = exe_dir();

    {
        const std::string tmp = make_temp_dir();
        expect(!tmp.empty(), "mkdtemp");
        const std::string dst = tmp + "/dst";
        expect_errno_bracket(dir + "/sluice-copy", {"/proc/self/mem", dst},
                             dir + "/sluice-copy: copy failed: backend_error (Input/output error)\n");
        ::unlink(dst.c_str());
        ::rmdir(tmp.c_str());
    }
    expect_errno_bracket(dir + "/sluice-grep", {"hit", "/proc/self/mem"},
                         dir + "/sluice-grep: /proc/self/mem: read error (Input/output error)\n");
    expect_errno_bracket(dir + "/sluice-hash", {"/proc/self/mem"},
                         dir + "/sluice-hash: /proc/self/mem: read error (Input/output error)\n");
    expect_errno_bracket(dir + "/sluice-tail", {"-f", "-n", "0", "/proc/self/mem"},
                         dir + "/sluice-tail: /proc/self/mem: read error (Input/output error)\n");

    if (g_failed != 0)
        return 1;
    std::printf("ok\n");
    return 0;
}
