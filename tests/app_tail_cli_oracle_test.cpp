// e4 (#474): TAIL signal-path, pending-I/O settlement and bounded-teardown
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

int g_checks_failed = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_checks_failed;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

void check_msg(bool ok, const std::string& what) { check(ok, what); }

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

struct TempFile {
    std::string path;
    int append_fd = -1;

    explicit TempFile(const std::string& content) {
        char tmpl[] = "/tmp/sluice_tail_e4_cli_XXXXXX";
        const int fd = ::mkstemp(tmpl);
        if (fd < 0) {
            std::fprintf(stderr, "DEADLINE_ABORT: mkstemp failed\n");
            std::fflush(stderr);
            std::_Exit(97);
        }
        path = tmpl;
        if (!content.empty() &&
            ::pwrite(fd, content.data(), content.size(), 0) !=
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
    TailChild() = default;
    TailChild(const TailChild&) = delete;
    TailChild& operator=(const TailChild&) = delete;

    ~TailChild() { cleanup(); }

    bool spawn(const std::string& bin, const std::vector<std::string>& args, dev_t shim_dev = 0,
               ino_t shim_ino = 0) {
        const bool with_shim = shim_dev != 0 || shim_ino != 0;
        char dir_tmpl[] = "/tmp/sluice_tail_e4_child_XXXXXX";
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
        for (char** e = environ; e && *e; ++e)
            env_store.emplace_back(*e);
        if (with_shim) {
            env_store.emplace_back("LD_PRELOAD=" + exe_dir() + "/libtail_pread_shim.so");
            env_store.emplace_back("SLUICE_TAIL_SHIM_DEV=" + std::to_string(
                                       static_cast<unsigned long long>(shim_dev)));
            env_store.emplace_back("SLUICE_TAIL_SHIM_INO=" + std::to_string(
                                       static_cast<unsigned long long>(shim_ino)));
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

        const pid_t pid = ::fork();
        if (pid < 0) {
            ::close(out_pipe[0]);
            ::close(out_pipe[1]);
            std::fprintf(stderr, "FAIL: fork\n");
            return false;
        }
        if (pid == 0) {
            ::close(out_pipe[0]);
            ::dup2(out_pipe[1], STDOUT_FILENO);
            ::close(out_pipe[1]);
            const int efd = ::open(err_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
        evt_fd_ = ::open(evt_path_.c_str(), O_RDWR);
        cmd_fd_ = ::open(cmd_path_.c_str(), O_RDWR);
        err_fd_ = ::open(err_path_.c_str(), O_RDONLY);
        return pid_ > 0;
    }

    bool wait_line(const std::string& want, std::chrono::steady_clock::duration timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (scan_line(want))
                return true;
            if (!pump_output(deadline))
                return false;
            if (exited_ && !scan_line(want))
                return false;
        }
    }

    bool wait_event(const std::string& prefix, std::chrono::steady_clock::duration timeout,
                    std::string* line_out = nullptr) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
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
            if (!pump_fd(evt_fd_, evt_buf_, deadline))
                return false;
        }
    }

    void send_signal(int sig) { ::kill(pid_, sig); }

    void release() {
        if (cmd_fd_ < 0)
            return;
        ssize_t r;
        do {
            r = ::write(cmd_fd_, "R", 1);
        } while (r < 0 && errno == EINTR);
    }

    bool alive() { return ::waitpid(pid_, nullptr, WNOHANG) == 0; }

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
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            int status = 0;
            const pid_t r = ::waitpid(pid_, &status, WNOHANG);
            if (r == pid_) {
                reaped_ = true;
                drain_output_eof();
                ChildOutcome o;
                if (WIFEXITED(status)) {
                    o.kind = ChildOutcome::Kind::exited;
                    o.code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    o.kind = ChildOutcome::Kind::signaled;
                    o.code = WTERMSIG(status);
                }
                exited_ = true;
                return o;
            }
            if (r < 0 && errno != EINTR) {
                ChildOutcome o;
                o.kind = ChildOutcome::Kind::not_done;
                return o;
            }
            pump_output(deadline);
            if (std::chrono::steady_clock::now() >= deadline) {
                ::kill(pid_, SIGKILL);
                int status2 = 0;
                while (::waitpid(pid_, &status2, 0) < 0 && errno == EINTR) {
                }
                reaped_ = true;
                exited_ = true;
                drain_output_eof();
                ChildOutcome o;
                o.kind = ChildOutcome::Kind::watchdog_killed;
                return o;
            }
        }
    }

    const std::string& out() const { return out_; }

    std::string err_text() const {
        std::string s;
        if (err_fd_ < 0)
            return s;
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
    static bool pump_fd(int fd, std::string& into, std::chrono::steady_clock::time_point deadline) {
        if (fd < 0)
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
        const int r = ::poll(&p, 1, ms);
        if (r <= 0)
            return true;
        char buf[4096];
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0)
            into.append(buf, static_cast<std::size_t>(n));
        return true;
    }

    bool pump_output(std::chrono::steady_clock::time_point deadline) {
        return pump_fd(out_fd_, out_, deadline);
    }

    bool scan_line(const std::string& want) {
        std::size_t nl;
        while ((nl = out_.find('\n', scanned_)) != std::string::npos) {
            const std::size_t begin = scanned_;
            scanned_ = nl + 1;
            if (out_.compare(begin, nl - begin, want) == 0)
                return true;
        }
        scanned_ = out_.size();
        return false;
    }

    void drain_output_eof() {
        if (out_fd_ < 0)
            return;
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(out_fd_, buf, sizeof buf);
            if (n <= 0)
                break;
            out_.append(buf, static_cast<std::size_t>(n));
        }
    }

    void cleanup() {
        if (pid_ > 0 && !reaped_) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
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
    bool reaped_ = false;
    bool exited_ = false;
};

// ---------------------------------------------------------------------------

bool file_status_of(const std::string& path, dev_t* dev, ino_t* ino) {
    struct stat st {};
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
    return g_checks_failed == 0;
}

bool follow_clean_signal_exit(int sig, const std::string& tag) {
    TempFile f("seed-1\nseed-2\n");
    TailChild child;
    const std::string bin = exe_dir() + "/sluice-tail";
    if (!child.spawn(bin, {"-f", "-n", "1", "--poll-interval", "50", f.path})) {
        check_msg(false, tag + ": spawn");
        return false;
    }
    check_msg(child.wait_line("seed-2", kEventDeadline), tag + ": seed line delivered");
    check_msg(f.append("live-1\n"), tag + ": append");
    check_msg(child.wait_line("live-1", kEventDeadline), tag + ": live line delivered");

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

bool cli_sigint_clean_exit() { return follow_clean_signal_exit(SIGINT, "sigint"); }

bool cli_sigterm_clean_exit() { return follow_clean_signal_exit(SIGTERM, "sigterm"); }

bool cli_double_signal_clean_exit() {
    TempFile f("seed-1\nseed-2\n");
    TailChild child;
    const std::string bin = exe_dir() + "/sluice-tail";
    if (!child.spawn(bin, {"-f", "-n", "1", "--poll-interval", "50", f.path})) {
        check_msg(false, "double: spawn");
        return false;
    }
    check_msg(child.wait_line("seed-2", kEventDeadline), "double: seed line delivered");
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
    if (!child.wait_event("ENTERED ", kEventDeadline, &line)) {
        check_msg(false, tag + ": no ENTERED event (LD_PRELOAD intercept failed?)");
        return false;
    }
    check_msg(parse_event(line, "ENTERED ", fields) == 3 && fields[0] == 1 && fields[1] == 5,
              tag + ": first held read is the follow read at offset 5, got: " + line);
    child.release();
    if (!child.wait_event("EXIT ", kEventDeadline, &line)) {
        check_msg(false, tag + ": no EXIT event for the first held read");
        return false;
    }
    check_msg(parse_event(line, "EXIT ", fields) == 2 && fields[0] == 1 && fields[1] == 0,
              tag + ": first held read was the EOF probe, got: " + line);

    // The child now sleeps one 50 ms slice before re-reading; the append lands
    // inside that window, so the next held read is the data read.
    check_msg(f.append("held\n"), tag + ": append");

    if (!child.wait_event("ENTERED ", kEventDeadline, &line)) {
        check_msg(false, tag + ": no ENTERED event for the data read");
        return false;
    }
    check_msg(parse_event(line, "ENTERED ", fields) == 3 && fields[0] == 2 && fields[1] == 5,
              tag + ": ENTERED 2 is the data read at offset 5, got: " + line);

    child.send_signal(sig);

    check_msg(child.stayed_alive_for(kNoRetireWindow),
              tag + ": child must keep settling the accepted read (no premature retirement)");

    child.release();

    if (!child.wait_event("EXIT ", kEventDeadline, &line)) {
        check_msg(false, tag + ": no EXIT event after release");
        return false;
    }
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

bool cli_pending_sigint() { return pending_read_settlement_case(SIGINT, "pending-sigint"); }

bool cli_pending_sigterm() { return pending_read_settlement_case(SIGTERM, "pending-sigterm"); }

bool cli_descriptor_follow_rotation() {
    TempFile f("seed\n");
    const std::string rotated = f.path + ".rotated";
    TailChild child;
    const std::string bin = exe_dir() + "/sluice-tail";
    if (!child.spawn(bin, {"-f", "-n", "1", "--poll-interval", "50", f.path})) {
        check_msg(false, "rotation: spawn");
        return false;
    }
    check_msg(child.wait_line("seed", kEventDeadline), "rotation: seed delivered");

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
    check_msg(child.wait_line("rot-b", kEventDeadline),
              "rotation: original-inode append delivered");

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

}  // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"harness_watchdog_selftest", harness_watchdog_selftest},
        {"finite_tail_and_cli_exit_codes", finite_tail_and_cli_exit_codes},
        {"cli_sigint_clean_exit", cli_sigint_clean_exit},
        {"cli_sigterm_clean_exit", cli_sigterm_clean_exit},
        {"cli_double_signal_clean_exit", cli_double_signal_clean_exit},
        {"cli_pending_sigint", cli_pending_sigint},
        {"cli_pending_sigterm", cli_pending_sigterm},
        {"cli_descriptor_follow_rotation", cli_descriptor_follow_rotation},
    };

    const std::string bin = exe_dir() + "/sluice-tail";
    struct stat st {};
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
