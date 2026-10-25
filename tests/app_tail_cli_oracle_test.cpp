// TAIL signal-path, pending-I/O settlement and bounded-teardown
// oracles against the REAL sluice-tail binary. The parent harness drives the
// child through stdout pipes, signal delivery and a test-only LD_PRELOAD
// interposer (tests/support/tail_pread_shim.c) whose PRE/ENTERED/EXIT events
// and 'R' release command flow over one fifo pair scoped to the child.
//
// Outcomes are classified, never conflated: EXITED(code), SIGNALED(sig),
// WATCHDOG_KILLED. A held read is released only after the harness observed
// the child still settling; exit deadlines run from release, never from
// signal delivery. Every wait is deadline-bounded; the harness reaps every
// child it spawns (watchdog selftest below proves the kill+reap path).

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <climits>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

using namespace std::chrono_literals;

constexpr auto kEventDeadline = 10s;
constexpr auto kExitDeadline = 10s;
constexpr auto kQuietWindow = 600ms;
// After the stop signal, the child must still be settling the accepted read
// for at least this long: a lost stop or an abandoned read retires early.
constexpr auto kNoRetireWindow = 400ms;
// Post-death stdout evidence collection never waits longer than this: a
// descendant holding the write end must not extend any deadline.
constexpr auto kDeathDrainBound = 400ms;

int g_checks_failed = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_checks_failed;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

void check_msg(bool ok, const std::string& what) {
    check(ok, what);
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

// The dynamic ASan runtime mapped into this test process, if the build linked
// one. sluice-tail is built in the same xmake configuration, so the runtime
// mapped here is the one the child's DT_NEEDED resolves to; preloading it
// ahead of the shim restores the link order libasan's preinit check requires.
// A build with no mapped dynamic runtime (static Clang runtime, non-ASan
// builds) needs no prepend.
std::string mapped_dynamic_asan_runtime() {
    std::FILE* f = std::fopen("/proc/self/maps", "r");
    if (!f)
        return {};
    char line[PATH_MAX];
    std::string found;
    while (found.empty() && std::fgets(line, sizeof line, f)) {
        const char* begin = std::strchr(line, '/');
        if (!begin)
            continue;
        const char* end = begin + std::strlen(begin);
        while (end > begin && (end[-1] == '\n' || end[-1] == '\r'))
            --end;
        const std::string path(begin, static_cast<std::size_t>(end - begin));
        const std::size_t slash = path.rfind('/');
        const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        if (name.rfind("libasan.so", 0) == 0 || name.rfind("libclang_rt.asan", 0) == 0)
            found = path;
    }
    std::fclose(f);
    return found;
}

std::string shim_child_preload() {
    static const std::string entry = [] {
        const std::string asan = mapped_dynamic_asan_runtime();
        return asan.empty() ? exe_dir() + "/libtail_pread_shim.so"
                            : asan + ":" + exe_dir() + "/libtail_pread_shim.so";
    }();
    return entry;
}

struct TempFile {
    std::string path;
    int append_fd = -1;

    explicit TempFile(const std::string& content) {
        char tmpl[] = "/tmp/sluice_tail_cli_XXXXXX";
        const int fd = ::mkstemp(tmpl);
        if (fd < 0) {
            std::fprintf(stderr, "DEADLINE_ABORT: mkstemp failed\n");
            std::fflush(stderr);
            std::_Exit(97);
        }
        path = tmpl;
        if (!content.empty() && ::pwrite(fd, content.data(), content.size(), 0) !=
                                    static_cast<ssize_t>(content.size())) {
            std::fprintf(stderr, "DEADLINE_ABORT: temp seed write failed\n");
            std::fflush(stderr);
            std::_Exit(97);
        }
        append_fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
        if (append_fd < 0) {
            std::fprintf(stderr, "DEADLINE_ABORT: append fd failed\n");
            std::fflush(stderr);
            std::_Exit(97);
        }
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    ~TempFile() {
        if (append_fd >= 0)
            ::close(append_fd);
        ::unlink(path.c_str());
    }

    bool append(const std::string& data) const {
        ssize_t done = 0;
        while (done < static_cast<ssize_t>(data.size())) {
            const ssize_t n = ::write(append_fd, data.data() + done,
                                      data.size() - static_cast<std::size_t>(done));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return false;
            }
            done += n;
        }
        return true;
    }
};

struct ChildOutcome {
    enum class Kind { not_done, exited, signaled, watchdog_killed };
    Kind kind = Kind::not_done;
    int code = -1;

    std::string describe() const {
        switch (kind) {
        case Kind::exited:
            return "exited(" + std::to_string(code) + ")";
        case Kind::signaled:
            return "signaled(" + std::to_string(code) + ")";
        case Kind::watchdog_killed:
            return "watchdog_killed";
        default:
            return "not_done";
        }
    }
};

class TailChild {
  public:
    enum class WaitOutcome { matched, event_timeout, child_exited_before_event, wait_error };

    TailChild() = default;
    TailChild(const TailChild&) = delete;
    TailChild& operator=(const TailChild&) = delete;

    ~TailChild() { cleanup(); }

    bool spawn(const std::string& bin, const std::vector<std::string>& args, dev_t shim_dev = 0,
               ino_t shim_ino = 0) {
        const bool with_shim = shim_dev != 0 || shim_ino != 0;
        char dir_tmpl[] = "/tmp/sluice_tail_child_XXXXXX";
        char* dir = ::mkdtemp(dir_tmpl);
        if (!dir) {
            std::fprintf(stderr, "FAIL: mkdtemp\n");
            return false;
        }
        dir_ = dir;
        evt_path_ = dir_ + "/evt";
        cmd_path_ = dir_ + "/cmd";
        err_path_ = dir_ + "/stderr";
        if (::mkfifo(evt_path_.c_str(), 0600) != 0 || ::mkfifo(cmd_path_.c_str(), 0600) != 0) {
            std::fprintf(stderr, "FAIL: mkfifo\n");
            return false;
        }

        std::vector<std::string> env_store;
        for (char** e = environ; e && *e; ++e) {
            // A shim child's LD_PRELOAD is test-owned: an inherited entry
            // ahead of the ASan runtime would break libasan's link-order
            // check, and a duplicate key would leave the shim entry ignored.
            if (with_shim && std::strncmp(*e, "LD_PRELOAD=", 11) == 0)
                continue;
            env_store.emplace_back(*e);
        }
        env_store.emplace_back("SLUICE_TAIL_TEST_EVT=" + evt_path_);
        if (with_shim) {
            env_store.emplace_back("LD_PRELOAD=" + shim_child_preload());
            env_store.emplace_back("SLUICE_TAIL_SHIM_DEV=" +
                                   std::to_string(static_cast<unsigned long long>(shim_dev)));
            env_store.emplace_back("SLUICE_TAIL_SHIM_INO=" +
                                   std::to_string(static_cast<unsigned long long>(shim_ino)));
            env_store.emplace_back("SLUICE_TAIL_SHIM_EVT=" + evt_path_);
            env_store.emplace_back("SLUICE_TAIL_SHIM_CMD=" + cmd_path_);
            env_store.emplace_back("SLUICE_TAIL_SHIM_HOLD_FROM=1");
        }
        std::vector<char*> envp;
        for (const std::string& e : env_store)
            envp.push_back(const_cast<char*>(e.c_str()));
        envp.push_back(nullptr);

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(bin.c_str()));
        for (const std::string& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        int out_pipe[2];
        if (::pipe(out_pipe) != 0) {
            std::fprintf(stderr, "FAIL: pipe\n");
            return false;
        }
        err_fd_ = ::open(err_path_.c_str(), O_RDONLY | O_CREAT | O_TRUNC, 0644);
        if (err_fd_ < 0) {
            ::close(out_pipe[0]);
            ::close(out_pipe[1]);
            std::fprintf(stderr, "FAIL: stderr capture open: %s\n", std::strerror(errno));
            return false;
        }

        const pid_t pid = ::fork();
        if (pid < 0) {
            ::close(out_pipe[0]);
            ::close(out_pipe[1]);
            ::close(err_fd_);
            err_fd_ = -1;
            std::fprintf(stderr, "FAIL: fork\n");
            return false;
        }
        if (pid == 0) {
            ::close(out_pipe[0]);
            ::dup2(out_pipe[1], STDOUT_FILENO);
            ::close(out_pipe[1]);
            // Own open, not a dup2 of err_fd_: a shared file description
            // would share the offset and make err_text() read from EOF.
            const int efd = ::open(err_path_.c_str(), O_WRONLY, 0600);
            if (efd < 0)
                ::_Exit(126);
            ::dup2(efd, STDERR_FILENO);
            if (efd != STDERR_FILENO)
                ::close(efd);
            ::execve(bin.c_str(), argv.data(), envp.data());
            ::_Exit(127);
        }
        ::close(out_pipe[1]);
        pid_ = pid;
        out_fd_ = out_pipe[0];
        // O_NONBLOCK: the parent holds both fifo ends, so a drained read must
        // return EAGAIN, never block.
        evt_fd_ = ::open(evt_path_.c_str(), O_RDWR | O_NONBLOCK);
        if (evt_fd_ >= 0)
            cmd_fd_ = ::open(cmd_path_.c_str(), O_RDWR);
        if (evt_fd_ < 0 || cmd_fd_ < 0) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
            reaped_ = true;
            return false;
        }
        return true;
    }

    WaitOutcome wait_line(const std::string& want, std::chrono::steady_clock::duration timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (scan_line(want))
                return WaitOutcome::matched;
            const int s = poll_state();
            if (s != 0)
                return terminal_line_outcome(want, s);
            if (!pump_output(deadline)) {
                // The stdout pipe closed before the deadline: a child closes
                // its fds during exit before the zombie is reapable, so the
                // reap state, not the pump stop, classifies the wait.
                const int s2 = poll_state();
                if (s2 != 0)
                    return terminal_line_outcome(want, s2);
                if (std::chrono::steady_clock::now() >= deadline)
                    return WaitOutcome::event_timeout;
                continue;
            }
        }
    }

    WaitOutcome wait_event(const std::string& prefix, std::chrono::steady_clock::duration timeout,
                           std::string* line_out = nullptr) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (consume_matching_event(prefix, line_out))
                return WaitOutcome::matched;
            const int s = poll_state();
            if (s != 0)
                return terminal_event_outcome(prefix, line_out, s);
            if (!pump_fd(evt_fd_, evt_buf_, deadline, evt_eof_)) {
                const int s2 = poll_state();
                if (s2 != 0)
                    return terminal_event_outcome(prefix, line_out, s2);
                if (std::chrono::steady_clock::now() >= deadline)
                    return WaitOutcome::event_timeout;
                continue;
            }
        }
    }

    void send_signal(int sig) {
        if (pid_ > 0)
            ::kill(pid_, sig);
    }

    void release() {
        if (cmd_fd_ < 0)
            return;
        ssize_t r;
        do {
            r = ::write(cmd_fd_, "R", 1);
        } while (r < 0 && errno == EINTR);
    }

    // Non-consuming liveness probe; a death observed here is recorded in the
    // shared reap state so wait_exit can still classify the outcome.
    bool alive() {
        const int s = poll_state();
        if (s == 1)
            drain_output_bounded();
        return s == 0;
    }

    // Bounded death confirmation that consumes neither stream: a wait issued
    // afterwards must recover data the child already wrote.
    bool join_without_consuming(std::chrono::steady_clock::duration timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const int s = poll_state();
            if (s != 0)
                return s == 1;
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            ::usleep(10000);
        }
    }

    bool stayed_alive_for(std::chrono::steady_clock::duration window) {
        const auto deadline = std::chrono::steady_clock::now() + window;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!alive())
                return false;
            pump_output(deadline);
            ::usleep(20000);
        }
        return alive();
    }

    ChildOutcome wait_exit(std::chrono::steady_clock::duration timeout) {
        if (reaped_) {
            drain_output_bounded();
            return outcome_of_termination();
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const int s = poll_state();
            if (s == 1) {
                drain_output_bounded();
                return outcome_of_termination();
            }
            if (s < 0) {
                ChildOutcome o;
                o.kind = ChildOutcome::Kind::not_done;
                return o;
            }
            pump_output(deadline);
            if (std::chrono::steady_clock::now() >= deadline) {
                watchdog_fired_ = true;
                ::kill(pid_, SIGKILL);
                int status = 0;
                while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
                saved_status_ = status;
                reaped_ = true;
                drain_output_bounded();
                return outcome_of_termination();
            }
        }
    }

    const std::string& out() const { return out_; }

    std::string wait_failure_detail(WaitOutcome o, const std::string& what) const {
        std::string s = what + ": ";
        switch (o) {
        case WaitOutcome::matched:
            return what + ": matched";
        case WaitOutcome::event_timeout:
            s += "EVENT_TIMEOUT; child ";
            s += reaped_ ? outcome_of_termination().describe() : std::string("still alive");
            break;
        case WaitOutcome::child_exited_before_event:
            s += "CHILD_EXITED_BEFORE_EVENT; child ";
            s += reaped_ ? outcome_of_termination().describe() : std::string("status unknown");
            break;
        case WaitOutcome::wait_error:
            s += "EVENT_WAIT_ERROR; no waitable status for pid " + std::to_string(pid_);
            break;
        }
        const std::string err = err_text_capped(4096);
        s += "; stderr[" + std::to_string(err.size()) + "B]=" + err;
        const std::size_t tail = out_.size() > 256 ? 256 : out_.size();
        s += "; stdout-tail=\"" + out_.substr(out_.size() - tail) + "\"";
        if (!evt_buf_.empty())
            s += "; pending-events=\"" + evt_buf_ + "\"";
        return s;
    }

    std::string err_text() const {
        std::string s;
        if (err_fd_ < 0)
            return "<stderr-capture-unavailable>";
        if (::lseek(err_fd_, 0, SEEK_SET) < 0)
            return "<stderr-capture-unavailable>";
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(err_fd_, buf, sizeof buf);
            if (n <= 0)
                break;
            s.append(buf, static_cast<std::size_t>(n));
        }
        return s;
    }

    bool reaped() const { return reaped_; }

    pid_t pid() const { return pid_; }

  private:
    // Single reap authority: 1 = terminated (saved_status_ valid, recorded
    // exactly once), 0 = alive, -1 = status unobtainable for pid_.
    int poll_state() {
        if (reaped_)
            return 1;
        if (pid_ <= 0)
            return -1;
        int status = 0;
        pid_t r;
        do {
            r = ::waitpid(pid_, &status, WNOHANG);
        } while (r < 0 && errno == EINTR);
        if (r == pid_) {
            saved_status_ = status;
            reaped_ = true;
            return 1;
        }
        if (r == 0)
            return 0;
        reap_lost_ = true;
        return -1;
    }

    ChildOutcome outcome_of_termination() const {
        ChildOutcome o = classify(saved_status_);
        if (watchdog_fired_ && o.kind == ChildOutcome::Kind::signaled && o.code == SIGKILL)
            o.kind = ChildOutcome::Kind::watchdog_killed;
        return o;
    }

    WaitOutcome terminal_line_outcome(const std::string& want, int s) {
        drain_output_bounded();
        if (scan_line(want))
            return WaitOutcome::matched;
        return s < 0 ? WaitOutcome::wait_error : WaitOutcome::child_exited_before_event;
    }

    WaitOutcome terminal_event_outcome(const std::string& prefix, std::string* line_out, int s) {
        consume_evt_available();
        if (consume_matching_event(prefix, line_out))
            return WaitOutcome::matched;
        return s < 0 ? WaitOutcome::wait_error : WaitOutcome::child_exited_before_event;
    }

    bool consume_matching_event(const std::string& prefix, std::string* line_out) {
        std::size_t nl;
        while ((nl = evt_buf_.find('\n')) != std::string::npos) {
            const std::string line = evt_buf_.substr(0, nl);
            evt_buf_.erase(0, nl + 1);
            if (line.rfind(prefix, 0) == 0) {
                if (line_out)
                    *line_out = line;
                return true;
            }
        }
        return false;
    }

    void consume_evt_available() {
        if (evt_fd_ < 0)
            return;
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(evt_fd_, buf, sizeof buf);
            if (n > 0) {
                evt_buf_.append(buf, static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
    }

    static ChildOutcome classify(int status) {
        ChildOutcome o;
        if (WIFEXITED(status)) {
            o.kind = ChildOutcome::Kind::exited;
            o.code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            o.kind = ChildOutcome::Kind::signaled;
            o.code = WTERMSIG(status);
        }
        return o;
    }

    static bool pump_fd(int fd, std::string& into, std::chrono::steady_clock::time_point deadline,
                        bool& eof) {
        if (fd < 0 || eof)
            return false;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;
        int ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (ms > 50)
            ms = 50;
        pollfd p{};
        p.fd = fd;
        p.events = POLLIN;
        int r;
        do {
            r = ::poll(&p, 1, ms);
        } while (r < 0 && errno == EINTR);
        if (r <= 0)
            return true;
        if (!(p.revents & (POLLIN | POLLHUP | POLLERR)))
            return true;
        char buf[4096];
        ssize_t n;
        do {
            n = ::read(fd, buf, sizeof buf);
        } while (n < 0 && errno == EINTR);
        if (n > 0) {
            into.append(buf, static_cast<std::size_t>(n));
            return true;
        }
        if (n == 0) {
            eof = true;
            return true;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return true;
        eof = true;
        return true;
    }

    bool pump_output(std::chrono::steady_clock::time_point deadline) {
        return pump_fd(out_fd_, out_, deadline, out_eof_);
    }

    bool scan_line(const std::string& want) {
        std::size_t begin = scanned_;
        for (;;) {
            const std::size_t nl = out_.find('\n', begin);
            if (nl == std::string::npos)
                break;
            if (out_.compare(begin, nl - begin, want) == 0) {
                scanned_ = nl + 1;
                return true;
            }
            begin = nl + 1;
        }
        // scanned_ stays at the fragment start: the line may only complete
        // after the next read appends the rest.
        scanned_ = begin;
        return false;
    }

    // Post-death evidence collection only: stops at EOF, at an idle pipe (a
    // descendant may still hold the write end), or at the bound.
    void drain_output_bounded() {
        if (out_fd_ < 0 || out_eof_)
            return;
        const auto bound = std::chrono::steady_clock::now() + kDeathDrainBound;
        char buf[4096];
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= bound)
                break;
            pollfd p{};
            p.fd = out_fd_;
            p.events = POLLIN;
            int ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(bound - now).count());
            if (ms > 50)
                ms = 50;
            const int r = ::poll(&p, 1, ms);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (r == 0)
                break;
            const ssize_t n = ::read(out_fd_, buf, sizeof buf);
            if (n > 0) {
                out_.append(buf, static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) {
                out_eof_ = true;
                break;
            }
            if (errno != EINTR)
                break;
        }
    }

    std::string err_text_capped(std::size_t cap) const {
        if (err_fd_ < 0)
            return "<stderr-capture-unavailable>";
        if (::lseek(err_fd_, 0, SEEK_SET) < 0)
            return "<stderr-capture-unavailable>";
        std::string s;
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(err_fd_, buf, sizeof buf);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            s.append(buf, static_cast<std::size_t>(n));
            if (s.size() > cap) {
                s.resize(cap);
                s += "...[truncated]";
                break;
            }
        }
        return s;
    }

    void cleanup() {
        if (pid_ > 0 && !reaped_ && !reap_lost_) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
            reaped_ = true;
        }
        if (out_fd_ >= 0)
            ::close(out_fd_);
        if (evt_fd_ >= 0)
            ::close(evt_fd_);
        if (cmd_fd_ >= 0)
            ::close(cmd_fd_);
        if (err_fd_ >= 0)
            ::close(err_fd_);
        if (!dir_.empty()) {
            ::unlink(evt_path_.c_str());
            ::unlink(cmd_path_.c_str());
            ::unlink(err_path_.c_str());
            ::rmdir(dir_.c_str());
        }
    }

    pid_t pid_ = -1;
    int out_fd_ = -1;
    int evt_fd_ = -1;
    int cmd_fd_ = -1;
    int err_fd_ = -1;
    std::string dir_;
    std::string evt_path_;
    std::string cmd_path_;
    std::string err_path_;
    std::string out_;
    std::string evt_buf_;
    std::size_t scanned_ = 0;
    int saved_status_ = 0;
    bool reaped_ = false;
    bool reap_lost_ = false;
    bool watchdog_fired_ = false;
    bool out_eof_ = false;
    bool evt_eof_ = false;
};

// ---------------------------------------------------------------------------

const char* wait_outcome_name(TailChild::WaitOutcome o) {
    switch (o) {
    case TailChild::WaitOutcome::matched:
        return "matched";
    case TailChild::WaitOutcome::event_timeout:
        return "event_timeout";
    case TailChild::WaitOutcome::child_exited_before_event:
        return "child_exited_before_event";
    case TailChild::WaitOutcome::wait_error:
        return "wait_error";
    }
    return "unknown";
}

bool expect_event(TailChild& child, const std::string& prefix,
                  std::chrono::steady_clock::duration timeout, std::string* line,
                  const std::string& what) {
    const TailChild::WaitOutcome o = child.wait_event(prefix, timeout, line);
    if (o == TailChild::WaitOutcome::matched)
        return true;
    check_msg(false, child.wait_failure_detail(o, what));
    return false;
}

bool expect_line(TailChild& child, const std::string& want,
                 std::chrono::steady_clock::duration timeout, const std::string& what) {
    const TailChild::WaitOutcome o = child.wait_line(want, timeout);
    if (o == TailChild::WaitOutcome::matched)
        return true;
    check_msg(false, child.wait_failure_detail(o, what));
    return false;
}

bool file_status_of(const std::string& path, dev_t* dev, ino_t* ino) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0)
        return false;
    *dev = st.st_dev;
    *ino = st.st_ino;
    return true;
}

// Parses "PRE <seq> <off> <len>" / "ENTERED <seq> <off> <len>" (3 fields) or
// "EXIT <seq> <ret>" (2 fields). Returns the field count, -1 on prefix miss.
int parse_event(const std::string& line, const char* prefix, long fields[3]) {
    if (line.rfind(prefix, 0) != 0)
        return -1;
    return std::sscanf(line.c_str() + std::strlen(prefix), "%ld %ld %ld", &fields[0], &fields[1],
                       &fields[2]);
}

bool finite_tail_and_cli_exit_codes() {
    const int before = g_checks_failed;
    {
        TempFile f("a\nb\nc\nd\ne\n");
        TailChild child;
        const std::string bin = exe_dir() + "/sluice-tail";
        check_msg(child.spawn(bin, {"-n", "2", f.path}), "cli: spawn finite");
        const ChildOutcome o = child.wait_exit(kExitDeadline);
        check_msg(o.kind == ChildOutcome::Kind::exited && o.code == 0,
                  "cli: finite exit 0, got " + o.describe());
        const std::string out = child.out();
        check_msg(out.find("d\n") != std::string::npos && out.find("e\n") != std::string::npos,
                  "cli: finite stdout has d/e");
    }
    {
        TailChild child;
        const std::string bin = exe_dir() + "/sluice-tail";
        check_msg(child.spawn(bin, {"/no/such/file_e4"}), "cli: spawn missing");
        const ChildOutcome o = child.wait_exit(kExitDeadline);
        check_msg(o.kind == ChildOutcome::Kind::exited && o.code == 2,
                  "cli: missing file exit 2, got " + o.describe());
    }
    {
        TailChild child;
        const std::string bin = exe_dir() + "/sluice-tail";
        check_msg(child.spawn(bin, {"--bogus-flag"}), "cli: spawn usage");
        const ChildOutcome o = child.wait_exit(kExitDeadline);
        check_msg(o.kind == ChildOutcome::Kind::exited && o.code == 1,
                  "cli: usage exit 1, got " + o.describe());
    }
    return g_checks_failed == before;
}

bool cli_removed_workers_and_max_poll_stop() {
    const int before = g_checks_failed;
    TempFile f("seed\n");
    const std::string bin = exe_dir() + "/sluice-tail";
    for (const auto* value : {"1", "4", "64", "0", "65", "bad", "184467440737095516160"}) {
        TailChild child;
        check_msg(child.spawn(bin, {"--workers", value, f.path}), "workers: removed option");
        const auto outcome = child.wait_exit(kExitDeadline);
        check_msg(outcome.kind == ChildOutcome::Kind::exited && outcome.code == 1 &&
                      child.out().empty() && child.err_text().find("--workers has been removed") != std::string::npos,
                  "workers: explicit rejection");
    }
    {
        TailChild child;
        check_msg(child.spawn(bin, {"-f", "-n", "1", "--poll-interval", "5000", f.path}),
                  "max poll: spawn");
        expect_line(child, "seed", kEventDeadline, "max poll: initial output");
        child.send_signal(SIGTERM);
        const auto outcome = child.wait_exit(1s);
        check_msg(outcome.kind == ChildOutcome::Kind::exited && outcome.code == 0 &&
                      child.out() == "seed\n" && child.err_text().empty(),
                  "max poll: signal stop bypasses 5000 ms deadline");
    }
    return g_checks_failed == before;
}

bool follow_clean_signal_exit(int sig, const std::string& tag) {
    TempFile f("seed-1\nseed-2\n");
    TailChild child;
    const std::string bin = exe_dir() + "/sluice-tail";
    if (!child.spawn(bin, {"-f", "-n", "1", "--poll-interval", "50", f.path})) {
        check_msg(false, tag + ": spawn");
        return false;
    }
    expect_line(child, "seed-2", kEventDeadline, tag + ": seed line delivered");
    check_msg(f.append("live-1\n"), tag + ": append");
    expect_line(child, "live-1", kEventDeadline, tag + ": live line delivered");

    child.send_signal(sig);
    const ChildOutcome o = child.wait_exit(kExitDeadline);
    check_msg(o.kind == ChildOutcome::Kind::exited && o.code == 0,
              tag + ": clean exit 0, got " + o.describe());
    check_msg(child.out().find("seed-2\n") != std::string::npos &&
                  child.out().find("live-1\n") != std::string::npos,
              tag + ": delivered lines intact");
    check_msg(child.err_text().empty(), tag + ": stderr empty");
    return o.kind == ChildOutcome::Kind::exited && o.code == 0;
}

bool cli_sigint_clean_exit() {
    return follow_clean_signal_exit(SIGINT, "sigint");
}

bool cli_sigterm_clean_exit() {
    return follow_clean_signal_exit(SIGTERM, "sigterm");
}

bool cli_double_signal_clean_exit() {
    TempFile f("seed-1\nseed-2\n");
    TailChild child;
    const std::string bin = exe_dir() + "/sluice-tail";
    if (!child.spawn(bin, {"-f", "-n", "1", "--poll-interval", "50", f.path})) {
        check_msg(false, "double: spawn");
        return false;
    }
    expect_line(child, "seed-2", kEventDeadline, "double: seed line delivered");
    child.send_signal(SIGINT);
    child.send_signal(SIGTERM);
    const ChildOutcome o = child.wait_exit(kExitDeadline);
    check_msg(o.kind == ChildOutcome::Kind::exited && o.code == 0,
              "double: clean exit 0, got " + o.describe());
    return o.kind == ChildOutcome::Kind::exited && o.code == 0;
}

bool pending_read_settlement_case(int sig, const std::string& tag) {
    TempFile f("seed\n");
    dev_t dev = 0;
    ino_t ino = 0;
    if (!file_status_of(f.path, &dev, &ino)) {
        check_msg(false, tag + ": stat target");
        return false;
    }
    TailChild child;
    const std::string bin = exe_dir() + "/sluice-tail";
    if (!child.spawn(bin, {"-f", "-n", "0", "--poll-interval", "50", f.path}, dev, ino)) {
        check_msg(false, tag + ": spawn");
        return false;
    }

    std::string line;
    long fields[3] = {0, 0, 0};
    // Round 1: the child's first follow read is held before any append can
    // land, so it is necessarily the empty-tail EOF probe at offset 5.
    if (!expect_event(child, "ENTERED ", kEventDeadline, &line,
                      tag + ": first held read ENTERED event"))
        return false;
    check_msg(parse_event(line, "ENTERED ", fields) == 3 && fields[0] == 1 && fields[1] == 5,
              tag + ": first held read is the follow read at offset 5, got: " + line);
    child.release();
    if (!expect_event(child, "EXIT ", kEventDeadline, &line,
                      tag + ": EXIT event for the first held read"))
        return false;
    check_msg(parse_event(line, "EXIT ", fields) == 2 && fields[0] == 1 && fields[1] == 0,
              tag + ": first held read was the EOF probe, got: " + line);

    // The child now sleeps one 50 ms slice before re-reading; the append lands
    // inside that window, so the next held read is the data read.
    check_msg(f.append("held\n"), tag + ": append");

    if (!expect_event(child, "ENTERED ", kEventDeadline, &line,
                      tag + ": data read ENTERED event"))
        return false;
    check_msg(parse_event(line, "ENTERED ", fields) == 3 && fields[0] == 2 && fields[1] == 5,
              tag + ": ENTERED 2 is the data read at offset 5, got: " + line);

    child.send_signal(sig);

    check_msg(child.stayed_alive_for(kNoRetireWindow),
              tag + ": child must keep settling the accepted read (no premature retirement)");

    child.release();

    if (!expect_event(child, "EXIT ", kEventDeadline, &line,
                      tag + ": EXIT event after release"))
        return false;
    check_msg(parse_event(line, "EXIT ", fields) == 2 && fields[0] == 2 && fields[1] == 5,
              tag + ": EXIT 2 returned 5 bytes, got: " + line);

    const ChildOutcome o = child.wait_exit(kExitDeadline);
    check_msg(o.kind == ChildOutcome::Kind::exited && o.code == 0,
              tag + ": clean exit 0 after settlement, got " + o.describe());
    check_msg(child.out().find("held\n") != std::string::npos,
              tag + ": held line delivered after settlement");
    std::string out = child.out();
    std::size_t pos = 0;
    int count = 0;
    while ((pos = out.find("held\n", pos)) != std::string::npos) {
        ++count;
        pos += 5;
    }
    check_msg(count == 1, tag + ": held line exactly once");
    check_msg(child.err_text().empty(), tag + ": stderr empty");
    return o.kind == ChildOutcome::Kind::exited && o.code == 0 && count == 1;
}

bool cli_pending_sigint() {
    return pending_read_settlement_case(SIGINT, "pending-sigint");
}

bool cli_pending_sigterm() {
    return pending_read_settlement_case(SIGTERM, "pending-sigterm");
}

bool cli_descriptor_follow_rotation() {
    TempFile f("seed\n");
    const std::string rotated = f.path + ".rotated";
    TailChild child;
    const std::string bin = exe_dir() + "/sluice-tail";
    if (!child.spawn(bin, {"-f", "-n", "1", "--poll-interval", "50", f.path})) {
        check_msg(false, "rotation: spawn");
        return false;
    }
    expect_line(child, "seed", kEventDeadline, "rotation: seed delivered");

    if (::rename(f.path.c_str(), rotated.c_str()) != 0) {
        std::fprintf(stderr, "DEADLINE_ABORT: rename failed\n");
        std::fflush(stderr);
        std::_Exit(97);
    }
    const int nfd = ::open(f.path.c_str(), O_WRONLY | O_CREAT, 0644);
    if (nfd < 0) {
        std::fprintf(stderr, "DEADLINE_ABORT: new path create failed\n");
        std::fflush(stderr);
        std::_Exit(97);
    }
    ::close(nfd);

    {
        const int af = ::open(rotated.c_str(), O_WRONLY | O_APPEND);
        const std::string data = "rot-b\n";
        const ssize_t w = af >= 0 ? ::write(af, data.data(), data.size()) : -1;
        if (af >= 0)
            ::close(af);
        check_msg(w == static_cast<ssize_t>(data.size()), "rotation: append to original inode");
    }
    expect_line(child, "rot-b", kEventDeadline, "rotation: original-inode append delivered");

    {
        const int af = ::open(f.path.c_str(), O_WRONLY | O_APPEND);
        const std::string data = "newp\n";
        const ssize_t w = af >= 0 ? ::write(af, data.data(), data.size()) : -1;
        if (af >= 0)
            ::close(af);
        check_msg(w == static_cast<ssize_t>(data.size()), "rotation: append to new path");
    }
    check_msg(child.stayed_alive_for(kQuietWindow), "rotation: child alive, no new-path follow");
    check_msg(child.out().find("newp") == std::string::npos,
              "rotation: new-path content never delivered");
    check_msg(child.err_text().find("truncated") == std::string::npos,
              "rotation: no truncation diag");

    child.send_signal(SIGINT);
    const ChildOutcome o = child.wait_exit(kExitDeadline);
    ::unlink(rotated.c_str());
    check_msg(o.kind == ChildOutcome::Kind::exited && o.code == 0,
              "rotation: clean exit 0, got " + o.describe());
    return o.kind == ChildOutcome::Kind::exited && o.code == 0 &&
           child.out().find("newp") == std::string::npos;
}

bool harness_watchdog_selftest() {
    TailChild child;
    if (!child.spawn("/bin/sleep", {"60"})) {
        check_msg(false, "selftest: spawn sleep");
        return false;
    }
    const ChildOutcome o = child.wait_exit(400ms);
    check_msg(o.kind == ChildOutcome::Kind::watchdog_killed,
              "selftest: watchdog classification, got " + o.describe());
    check_msg(child.reaped(), "selftest: child reaped");
    int status = 0;
    const pid_t r = ::waitpid(child.pid(), &status, WNOHANG);
    check_msg(r < 0 && errno == ECHILD, "selftest: no zombie remains");
    return o.kind == ChildOutcome::Kind::watchdog_killed && r < 0 && errno == ECHILD;
}

// Negative control for the empty-stderr assertions: a child that exits 0
// while writing stderr must be observable through err_text().
bool harness_stderr_capture_selftest() {
    TailChild child;
    if (!child.spawn("/bin/sh", {"-c", "echo tail-stderr-marker >&2; exit 0"})) {
        check_msg(false, "selftest-stderr: spawn sh");
        return false;
    }
    const ChildOutcome o = child.wait_exit(kExitDeadline);
    check_msg(o.kind == ChildOutcome::Kind::exited && o.code == 0,
              "selftest-stderr: exit 0, got " + o.describe());
    check_msg(child.err_text().find("tail-stderr-marker") != std::string::npos,
              "selftest-stderr: stderr captured, got: [" + child.err_text() + "]");
    return o.kind == ChildOutcome::Kind::exited && o.code == 0 &&
           child.err_text().find("tail-stderr-marker") != std::string::npos;
}

bool harness_split_line_selftest() {
    TailChild child;
    // The mid-line sleep forces two pipe writes, so the line is only
    // recognized if the scanner retains the fragment between reads.
    if (!child.spawn("/bin/sh", {"-c", "printf split-hal; sleep 0.4; echo f-line"})) {
        check_msg(false, "selftest-split: spawn sh");
        return false;
    }
    const TailChild::WaitOutcome got = child.wait_line("split-half-line", kEventDeadline);
    check_msg(got == TailChild::WaitOutcome::matched,
              "selftest-split: line assembled across read chunks, got " +
                  std::string(wait_outcome_name(got)));
    const ChildOutcome o = child.wait_exit(kExitDeadline);
    check_msg(o.kind == ChildOutcome::Kind::exited && o.code == 0,
              "selftest-split: sh exit 0, got " + o.describe());
    return got == TailChild::WaitOutcome::matched && o.kind == ChildOutcome::Kind::exited &&
           o.code == 0;
}

bool harness_death_aware_event_wait_reports_exit_and_stderr() {
    TailChild child;
    if (!child.spawn("/bin/sh", {"-c", "echo HARNESS-DEATH-MARKER >&2; exit 23"})) {
        check_msg(false, "death-aware: spawn sh");
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    const TailChild::WaitOutcome o = child.wait_event("ENTERED ", kEventDeadline);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const std::string detail =
        child.wait_failure_detail(o, "first held read ENTERED event");
    check_msg(o == TailChild::WaitOutcome::child_exited_before_event,
              std::string("death-aware: outcome is child_exited_before_event, got ") +
                  wait_outcome_name(o));
    check_msg(elapsed < 5s, "death-aware: fails long before the 10s event deadline");
    check_msg(detail.find("CHILD_EXITED_BEFORE_EVENT") != std::string::npos &&
                  detail.find("exited(23)") != std::string::npos &&
                  detail.find("HARNESS-DEATH-MARKER") != std::string::npos,
              "death-aware: failure detail carries classification and stderr, got: " + detail);
    const ChildOutcome e = child.wait_exit(kExitDeadline);
    check_msg(e.kind == ChildOutcome::Kind::exited && e.code == 23,
              "death-aware: cached status is exited(23), got " + e.describe());
    check_msg(child.err_text().find("HARNESS-DEATH-MARKER") != std::string::npos,
              "death-aware: stderr death marker captured");
    int status = 0;
    const pid_t r = ::waitpid(child.pid(), &status, WNOHANG);
    check_msg(r < 0 && errno == ECHILD, "death-aware: child reaped, no zombie remains");
    return o == TailChild::WaitOutcome::child_exited_before_event && elapsed < 5s &&
           detail.find("CHILD_EXITED_BEFORE_EVENT") != std::string::npos &&
           detail.find("exited(23)") != std::string::npos &&
           detail.find("HARNESS-DEATH-MARKER") != std::string::npos &&
           e.kind == ChildOutcome::Kind::exited && e.code == 23 &&
           child.err_text().find("HARNESS-DEATH-MARKER") != std::string::npos && r < 0 &&
           errno == ECHILD;
}

bool harness_exec_failure_is_detected_and_cached() {
    TailChild child;
    const std::string missing = exe_dir() + "/no-such-harness-exec-binary";
    if (!child.spawn(missing, {"arg"})) {
        check_msg(false, "exec-fail: spawn reports the forked pid");
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    const TailChild::WaitOutcome o = child.wait_event("ENTERED ", kEventDeadline);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    check_msg(o == TailChild::WaitOutcome::child_exited_before_event && elapsed < 5s,
              std::string("exec-fail: fast child_exited_before_event, got ") +
                  wait_outcome_name(o));
    const ChildOutcome e = child.wait_exit(kExitDeadline);
    check_msg(e.kind == ChildOutcome::Kind::exited && e.code == 127,
              "exec-fail: cached status is exited(127), got " + e.describe());
    int status = 0;
    const pid_t r = ::waitpid(child.pid(), &status, WNOHANG);
    check_msg(r < 0 && errno == ECHILD, "exec-fail: cleanup reaped, no zombie remains");
    return o == TailChild::WaitOutcome::child_exited_before_event && elapsed < 5s &&
           e.kind == ChildOutcome::Kind::exited && e.code == 127 && r < 0 && errno == ECHILD;
}

bool harness_event_fully_written_before_exit_is_still_delivered() {
    TailChild child;
    // The child writes the complete event, then exits; the fifo data outlives
    // the writer because the parent holds the read end (O_RDWR).
    if (!child.spawn("/bin/sh",
                     {"-c", "printf 'ENTERED 7 5 0\\n' > \"$SLUICE_TAIL_TEST_EVT\"; exit 0"})) {
        check_msg(false, "event-then-exit: spawn sh");
        return false;
    }
    std::string line;
    const TailChild::WaitOutcome o = child.wait_event("ENTERED ", kEventDeadline, &line);
    check_msg(o == TailChild::WaitOutcome::matched,
              std::string("event-then-exit: event survives the death race, got ") +
                  wait_outcome_name(o));
    check_msg(line == "ENTERED 7 5 0", "event-then-exit: exact payload, got: " + line);
    const ChildOutcome e = child.wait_exit(kExitDeadline);
    check_msg(e.kind == ChildOutcome::Kind::exited && e.code == 0,
              "event-then-exit: sh exit 0, got " + e.describe());
    if (!(o == TailChild::WaitOutcome::matched && line == "ENTERED 7 5 0" &&
          e.kind == ChildOutcome::Kind::exited && e.code == 0))
        return false;
    {
        TailChild child;
        // Death is confirmed before any consumption: the event was fully
        // written before exit, so the wait must still recover it from the fifo.
        if (!child.spawn("/bin/sh",
                         {"-c", "printf 'ENTERED 9 5 0\\n' > \"$SLUICE_TAIL_TEST_EVT\"; exit 0"})) {
            check_msg(false, "event-then-exit: spawn sh for ordered arm");
            return false;
        }
        if (!child.join_without_consuming(kEventDeadline)) {
            check_msg(false, "event-then-exit: deterministic death confirmation");
            return false;
        }
        const TailChild::WaitOutcome oo = child.wait_event("ENTERED ", kEventDeadline, &line);
        check_msg(oo == TailChild::WaitOutcome::matched,
                  std::string("event-then-exit: event written before death still recovered, got ") +
                      wait_outcome_name(oo));
        check_msg(line == "ENTERED 9 5 0",
                  "event-then-exit: ordered-arm payload, got: " + line);
        const ChildOutcome e2 = child.wait_exit(kExitDeadline);
        check_msg(e2.kind == ChildOutcome::Kind::exited && e2.code == 0,
                  "event-then-exit: ordered-arm sh exit 0, got " + e2.describe());
        return oo == TailChild::WaitOutcome::matched && line == "ENTERED 9 5 0" &&
               e2.kind == ChildOutcome::Kind::exited && e2.code == 0;
    }
}

bool harness_non_target_event_before_death_is_not_matched() {
    TailChild child;
    if (!child.spawn("/bin/sh",
                     {"-c", "printf 'PRE 1 0 0\\n' > \"$SLUICE_TAIL_TEST_EVT\"; exit 0"})) {
        check_msg(false, "pre-then-exit: spawn sh");
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    const TailChild::WaitOutcome o = child.wait_event("ENTERED ", kEventDeadline);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    check_msg(o == TailChild::WaitOutcome::child_exited_before_event,
              std::string("pre-then-exit: PRE never counts as ENTERED, got ") +
                  wait_outcome_name(o));
    check_msg(elapsed < 5s, "pre-then-exit: fast failure after death");
    return o == TailChild::WaitOutcome::child_exited_before_event && elapsed < 5s;
}

bool harness_last_line_before_death_matched_and_partial_rejected() {
    {
        TailChild child;
        if (!child.spawn("/bin/sh", {"-c", "printf 'T5-early\\nT5-FINAL-LINE\\n'"})) {
            check_msg(false, "last-line: spawn sh");
            return false;
        }
        const TailChild::WaitOutcome o = child.wait_line("T5-FINAL-LINE", kEventDeadline);
        check_msg(o == TailChild::WaitOutcome::matched,
                  std::string("last-line: complete final line delivered despite death, got ") +
                      wait_outcome_name(o));
        const ChildOutcome e = child.wait_exit(kExitDeadline);
        check_msg(e.kind == ChildOutcome::Kind::exited && e.code == 0,
                  "last-line: sh exit 0, got " + e.describe());
        if (!(o == TailChild::WaitOutcome::matched && e.kind == ChildOutcome::Kind::exited &&
              e.code == 0))
            return false;
    }
    {
        TailChild child;
        // Death is confirmed before any consumption: the complete line was
        // fully written before exit, so wait_line must still recover it.
        if (!child.spawn("/bin/sh", {"-c", "printf 'T5-JOIN\\nT5-FINAL-LINE\\n'"})) {
            check_msg(false, "last-line: spawn sh for ordered arm");
            return false;
        }
        if (!child.join_without_consuming(kEventDeadline)) {
            check_msg(false, "last-line: deterministic death confirmation");
            return false;
        }
        const TailChild::WaitOutcome o = child.wait_line("T5-FINAL-LINE", kEventDeadline);
        check_msg(o == TailChild::WaitOutcome::matched,
                  std::string("last-line: line written before death still recovered, got ") +
                      wait_outcome_name(o));
        if (o != TailChild::WaitOutcome::matched)
            return false;
    }
    {
        TailChild child;
        if (!child.spawn("/bin/sh", {"-c", "printf 'T5-partial-no-newline'"})) {
            check_msg(false, "last-line: spawn partial sh");
            return false;
        }
        const auto start = std::chrono::steady_clock::now();
        const TailChild::WaitOutcome o =
            child.wait_line("T5-partial-no-newline-COMPLETE", kEventDeadline);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        check_msg(o == TailChild::WaitOutcome::child_exited_before_event,
                  std::string("last-line: unterminated fragment never matches, got ") +
                      wait_outcome_name(o));
        check_msg(elapsed < 5s, "last-line: partial-line negative control fails fast");
        return o == TailChild::WaitOutcome::child_exited_before_event && elapsed < 5s;
    }
}

bool harness_live_child_keeps_full_event_deadline() {
    TailChild child;
    if (!child.spawn("/bin/sleep", {"60"})) {
        check_msg(false, "live-timeout: spawn sleep");
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    const TailChild::WaitOutcome o = child.wait_event("ENTERED ", 600ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    check_msg(o == TailChild::WaitOutcome::event_timeout,
              std::string("live-timeout: outcome is event_timeout, got ") +
                  wait_outcome_name(o));
    check_msg(elapsed >= 550ms,
              "live-timeout: a live child keeps the requested wait window");
    check_msg(elapsed < 5s, "live-timeout: timeout is bounded");
    check_msg(child.alive(), "live-timeout: poll silence never classifies as death");
    return o == TailChild::WaitOutcome::event_timeout && elapsed >= 550ms && elapsed < 5s &&
           child.alive();
}

bool harness_watchdog_classification_survives_reentry_and_external_signals() {
    {
        TailChild child;
        if (!child.spawn("/bin/sleep", {"60"})) {
            check_msg(false, "watchdog-reentry: spawn sleep");
            return false;
        }
        const ChildOutcome o1 = child.wait_exit(400ms);
        check_msg(o1.kind == ChildOutcome::Kind::watchdog_killed,
                  "watchdog-reentry: watchdog classification, got " + o1.describe());
        const ChildOutcome o2 = child.wait_exit(kExitDeadline);
        check_msg(o2.kind == ChildOutcome::Kind::watchdog_killed,
                  "watchdog-reentry: re-entry returns the cached classification, got " +
                      o2.describe());
        check_msg(child.reaped(), "watchdog-reentry: child reaped");
        int status = 0;
        const pid_t r = ::waitpid(child.pid(), &status, WNOHANG);
        check_msg(r < 0 && errno == ECHILD, "watchdog-reentry: no leftover child, no double reap");
        if (!(o1.kind == ChildOutcome::Kind::watchdog_killed &&
              o2.kind == ChildOutcome::Kind::watchdog_killed && child.reaped() && r < 0 &&
              errno == ECHILD))
            return false;
    }
    {
        TailChild child;
        if (!child.spawn("/bin/sleep", {"60"})) {
            check_msg(false, "watchdog-reentry: spawn sleep for external kill");
            return false;
        }
        ::kill(child.pid(), SIGKILL);
        const ChildOutcome o = child.wait_exit(kExitDeadline);
        check_msg(o.kind == ChildOutcome::Kind::signaled && o.code == SIGKILL,
                  "watchdog-reentry: external SIGKILL is signaled(9), not watchdog_killed, got " +
                      o.describe());
        return o.kind == ChildOutcome::Kind::signaled && o.code == SIGKILL;
    }
}

bool harness_descendant_holding_stdout_keeps_death_paths_bounded() {
    {
        TailChild child;
        // The backgrounded sleep inherits stdout and holds the pipe write end
        // for ~2s after sh exits; it self-terminates, so nothing leaks.
        if (!child.spawn("/bin/sh", {"-c", "sleep 2 & exit 0"})) {
            check_msg(false, "stdout-holder: spawn sh");
            return false;
        }
        const auto start = std::chrono::steady_clock::now();
        const ChildOutcome e = child.wait_exit(kExitDeadline);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        check_msg(e.kind == ChildOutcome::Kind::exited && e.code == 0,
                  "stdout-holder: sh exit 0 while a descendant holds stdout, got " +
                      e.describe());
        check_msg(elapsed < 1500ms,
                  "stdout-holder: reap+drain bounded while the 2s descendant still lives");
        if (!(e.kind == ChildOutcome::Kind::exited && e.code == 0 && elapsed < 1500ms))
            return false;
    }
    {
        TailChild child;
        if (!child.spawn("/bin/sh", {"-c", "sleep 2 & exit 0"})) {
            check_msg(false, "stdout-holder: spawn sh for probe path");
            return false;
        }
        const auto start = std::chrono::steady_clock::now();
        while (child.alive() && std::chrono::steady_clock::now() - start < 1s)
            ::usleep(10000);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        check_msg(elapsed < 1500ms,
                  "stdout-holder: liveness-probe death path stays bounded");
        check_msg(!child.alive(),
                  "stdout-holder: liveness probe must observe the child's death");
        return elapsed < 1500ms && !child.alive();
    }
}

} // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"harness_watchdog_selftest", harness_watchdog_selftest},
        {"harness_stderr_capture_selftest", harness_stderr_capture_selftest},
        {"harness_split_line_selftest", harness_split_line_selftest},
        {"harness_death_aware_event_wait_reports_exit_and_stderr",
         harness_death_aware_event_wait_reports_exit_and_stderr},
        {"harness_exec_failure_is_detected_and_cached",
         harness_exec_failure_is_detected_and_cached},
        {"harness_event_fully_written_before_exit_is_still_delivered",
         harness_event_fully_written_before_exit_is_still_delivered},
        {"harness_non_target_event_before_death_is_not_matched",
         harness_non_target_event_before_death_is_not_matched},
        {"harness_last_line_before_death_matched_and_partial_rejected",
         harness_last_line_before_death_matched_and_partial_rejected},
        {"harness_live_child_keeps_full_event_deadline",
         harness_live_child_keeps_full_event_deadline},
        {"harness_watchdog_classification_survives_reentry_and_external_signals",
         harness_watchdog_classification_survives_reentry_and_external_signals},
        {"harness_descendant_holding_stdout_keeps_death_paths_bounded",
         harness_descendant_holding_stdout_keeps_death_paths_bounded},
        {"finite_tail_and_cli_exit_codes", finite_tail_and_cli_exit_codes},
        {"cli_removed_workers_and_max_poll_stop", cli_removed_workers_and_max_poll_stop},
        {"cli_sigint_clean_exit", cli_sigint_clean_exit},
        {"cli_sigterm_clean_exit", cli_sigterm_clean_exit},
        {"cli_double_signal_clean_exit", cli_double_signal_clean_exit},
        {"cli_pending_sigint", cli_pending_sigint},
        {"cli_pending_sigterm", cli_pending_sigterm},
        {"cli_descriptor_follow_rotation", cli_descriptor_follow_rotation},
    };

    const std::string bin = exe_dir() + "/sluice-tail";
    struct stat st{};
    if (::stat(bin.c_str(), &st) != 0) {
        std::fprintf(stderr, "FAIL: sluice-tail binary not found next to the test binary (%s)\n",
                     bin.c_str());
        return 1;
    }

    int failed_cases = 0;
    for (const NamedTest& t : tests) {
        const int before = g_checks_failed;
        const bool ok = t.fn();
        const bool clean = ok && g_checks_failed == before;
        if (!clean) {
            ++failed_cases;
            std::fprintf(stderr, "CASE FAIL: %s\n", t.name);
        } else {
            std::fprintf(stderr, "CASE PASS: %s\n", t.name);
        }
    }
    std::fflush(stderr);
    if (failed_cases > 0) {
        std::fprintf(stderr, "FAIL: %d of %zu tail cli oracle cases failed\n", failed_cases,
                     sizeof(tests) / sizeof(tests[0]));
        return 1;
    }
    std::printf("all %zu tail cli oracle cases passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
