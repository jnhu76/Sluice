// e4 (#474): TAIL stop / settlement / teardown contract oracles over the
// public TailEngine API. Event anchors come from an in-process pread
// interposer compiled into this target: ELF symbol interposition routes the
// ThreadPoolBackend worker's ::pread through tail_probe, which gates on the
// target file's dev/ino and can hold one accepted read until the test
// releases it. No production seam, no new Runtime API.
//
// Bounded-failure discipline: every wait has a deadline; a deadline that
// invalidates the remaining invariants dumps diagnostics and _Exit()s with a
// distinct code instead of hanging the suite or tearing a live runtime down.

#include "tail_task.hpp"

#include <sluice/file_resource.hpp>

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using sluice_tail::TailEngine;
using sluice_tail::TailOptions;
using sluice_tail::TailResult;

constexpr auto kEventDeadline = 10s;
constexpr auto kSettleDeadline = 10s;
// Negative windows must outlast several 50 ms poll cycles; a wrong emission
// happens synchronously in the read iteration, never delayed past them.
constexpr auto kQuietWindow = 600ms;
// While a held read is unsettled, wait() must not return within this window
// (task may not retire; publish happens strictly after the task returns).
constexpr auto kNoRetireWindow = 300ms;

int g_checks_failed = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_checks_failed;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

void check_msg(bool ok, const std::string& what) { check(ok, what); }

[[noreturn]] void deadline_abort(const std::string& why) {
    std::fprintf(stderr, "DEADLINE_ABORT: %s\n", why.c_str());
    std::fflush(stderr);
    std::_Exit(97);
}

// ---------------------------------------------------------------------------
// In-process pread probe: ELF interposition of the backend worker's ::pread.
// The mutex is never held across the blocking syscall or the hold wait.
namespace tail_probe {

struct Record {
    enum class Kind { pre, entered, exit };
    Kind kind;
    long seq = 0;
    std::uint64_t offset = 0;
    std::size_t len = 0;
    long ret = 0;
};

namespace {

std::mutex g_mtx;
std::condition_variable g_cv;
bool g_armed = false;
long g_release_count = 0;
long g_holds_served = 0;
long g_hold_from = -1;
long g_seq = 0;
dev_t g_dev = 0;
ino_t g_ino = 0;
std::vector<Record> g_log;

bool matches(const Record& r, Record::Kind kind, long seq) {
    return r.kind == kind && (seq < 0 || r.seq == seq);
}

}  // namespace

void arm(dev_t dev, ino_t ino, long hold_from_seq) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_dev = dev;
    g_ino = ino;
    g_hold_from = hold_from_seq;
    g_armed = true;
    g_release_count = 0;
    g_holds_served = 0;
    g_seq = 0;
    g_log.clear();
}

// Grants exactly one pending hold; held reads wake in arrival order.
void release() {
    std::lock_guard<std::mutex> lk(g_mtx);
    ++g_release_count;
    g_cv.notify_all();
}

void disarm() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_armed = false;
    g_release_count = g_holds_served + 1000;
    g_cv.notify_all();
}

std::optional<Record> wait_for(Record::Kind kind, long seq,
                               const std::function<bool(const Record&)>& extra,
                               std::chrono::steady_clock::duration timeout,
                               std::size_t& cursor) {
    std::unique_lock<std::mutex> lk(g_mtx);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto take = [&](std::size_t at) {
        const Record out = g_log[at];
        cursor = at + 1;
        return out;
    };
    const auto hit = [&](std::size_t at) {
        return matches(g_log[at], kind, seq) && (!extra || extra(g_log[at]));
    };
    for (;;) {
        for (; cursor < g_log.size(); ++cursor) {
            if (hit(cursor))
                return take(cursor);
        }
        if (g_cv.wait_until(lk, deadline) == std::cv_status::timeout) {
            for (; cursor < g_log.size(); ++cursor) {
                if (hit(cursor))
                    return take(cursor);
            }
            return std::nullopt;
        }
    }
}

std::vector<Record> log_snapshot() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_log;
}

void dump_log(const char* tag) {
    std::vector<Record> snap = log_snapshot();
    static const char* kNames[] = {"pre", "entered", "exit"};
    for (const Record& r : snap) {
        std::fprintf(stderr, "  [%s] %s seq=%ld off=%llu len=%zu ret=%ld\n", tag,
                     kNames[static_cast<int>(r.kind)], r.seq,
                     static_cast<unsigned long long>(r.offset), r.len, r.ret);
    }
}

}  // namespace tail_probe

extern "C" ssize_t pread(int fd, void* buf, std::size_t count, off_t offset) {
    bool armed = false;
    dev_t dev = 0;
    ino_t ino = 0;
    {
        std::lock_guard<std::mutex> lk(tail_probe::g_mtx);
        armed = tail_probe::g_armed;
        dev = tail_probe::g_dev;
        ino = tail_probe::g_ino;
    }
    struct stat st {};
    if (!armed || ::fstat(fd, &st) != 0 || st.st_dev != dev || st.st_ino != ino)
        return static_cast<ssize_t>(::syscall(SYS_pread64, fd, buf, count, offset));

    long seq = 0;
    long hold_ticket = -1;
    {
        std::lock_guard<std::mutex> lk(tail_probe::g_mtx);
        seq = ++tail_probe::g_seq;
        tail_probe::g_log.push_back(
            {tail_probe::Record::Kind::pre, seq, static_cast<std::uint64_t>(offset), count, 0});
        if (tail_probe::g_armed && tail_probe::g_hold_from >= 0 && seq >= tail_probe::g_hold_from) {
            hold_ticket = tail_probe::g_holds_served;
            tail_probe::g_log.push_back(
                {tail_probe::Record::Kind::entered, seq, static_cast<std::uint64_t>(offset),
                 count, 0});
        }
        tail_probe::g_cv.notify_all();
    }
    if (hold_ticket >= 0) {
        std::unique_lock<std::mutex> lk(tail_probe::g_mtx);
        tail_probe::g_cv.wait(lk, [&] {
            return tail_probe::g_release_count > hold_ticket;
        });
        ++tail_probe::g_holds_served;
    }
    const ssize_t r = static_cast<ssize_t>(::syscall(SYS_pread64, fd, buf, count, offset));
    {
        std::lock_guard<std::mutex> lk(tail_probe::g_mtx);
        tail_probe::g_log.push_back({tail_probe::Record::Kind::exit, seq,
                                     static_cast<std::uint64_t>(offset), count,
                                     static_cast<long>(r)});
        tail_probe::g_cv.notify_all();
    }
    return r;
}

// ---------------------------------------------------------------------------
struct TempFile {
    std::string path;
    int append_fd = -1;

    explicit TempFile(const std::string& content) {
        char tmpl[] = "/tmp/sluice_tail_e4_XXXXXX";
        const int fd = ::mkstemp(tmpl);
        if (fd < 0)
            deadline_abort("mkstemp failed");
        path = tmpl;
        if (!content.empty()) {
            std::size_t done = 0;
            while (done < content.size()) {
                const ssize_t n = ::pwrite(fd, content.data() + done, content.size() - done,
                                           static_cast<off_t>(done));
                if (n <= 0)
                    deadline_abort("temp seed write failed");
                done += static_cast<std::size_t>(n);
            }
        }
        append_fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
        if (append_fd < 0)
            deadline_abort("temp append fd open failed");
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
            const ssize_t n = ::write(append_fd, data.data() + done, data.size() - static_cast<std::size_t>(done));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return false;
            }
            done += n;
        }
        return true;
    }

    bool truncate_to(std::uint64_t size) const {
        struct stat st {};
        if (::stat(path.c_str(), &st) != 0)
            return false;
        return ::truncate(path.c_str(), static_cast<off_t>(size)) == 0;
    }
};

struct SinkCapture {
    mutable std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::string> lines;
    std::string diag;

    void on_line(std::string_view l) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            lines.emplace_back(l);
        }
        cv.notify_all();
    }

    void on_diag(std::string_view m) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            diag.append(m);
        }
        cv.notify_all();
    }

    bool wait_line(const std::string& want, std::chrono::steady_clock::duration timeout) {
        std::unique_lock<std::mutex> lk(mtx);
        const auto has = [&] {
            for (const std::string& l : lines)
                if (l == want)
                    return true;
            return false;
        };
        return cv.wait_for(lk, timeout, has);
    }

    bool wait_diag(const std::string& fragment, std::chrono::steady_clock::duration timeout) {
        std::unique_lock<std::mutex> lk(mtx);
        return cv.wait_for(lk, timeout, [&] { return diag.find(fragment) != std::string::npos; });
    }

    bool quiet_for(std::chrono::steady_clock::duration timeout) {
        std::unique_lock<std::mutex> lk(mtx);
        const std::size_t lines0 = lines.size();
        const std::size_t diag0 = diag.size();
        const auto grew = [&] { return lines.size() != lines0 || diag.size() != diag0; };
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!grew()) {
            if (cv.wait_until(lk, deadline) == std::cv_status::timeout)
                break;
        }
        return !grew();
    }

    std::vector<std::string> lines_copy() const {
        std::lock_guard<std::mutex> lk(mtx);
        return lines;
    }

    std::string diag_copy() const {
        std::lock_guard<std::mutex> lk(mtx);
        return diag;
    }
};

struct EngineRun {
    TempFile& file;
    SinkCapture sink;
    std::unique_ptr<TailEngine> engine;

    EngineRun(TempFile& f, const TailOptions& options) : file(f) {
        auto opened = sluice::File::open(f.path);
        if (!opened.has_value())
            deadline_abort("engine file open failed");
        engine = std::make_unique<TailEngine>(
            std::move(opened).value(), options,
            [this](std::string_view l) { sink.on_line(l); },
            [this](std::string_view m) { sink.on_diag(m); });
    }
};

TailOptions follow_options(std::size_t lines) {
    TailOptions o;
    o.lines = lines;
    o.follow = true;
    o.poll_interval_ms = 50;
    return o;
}

bool wait_for_record(const char* what, tail_probe::Record::Kind kind, long seq,
                     const std::function<bool(const tail_probe::Record&)>& extra,
                     std::chrono::steady_clock::duration timeout, std::size_t& cursor,
                     tail_probe::Record& out) {
    auto r = tail_probe::wait_for(kind, seq, extra, timeout, cursor);
    if (!r.has_value()) {
        std::fprintf(stderr, "probe log at failure:\n");
        tail_probe::dump_log("probe");
        check_msg(false, std::string(what) + " (deadline)");
        return false;
    }
    out = *r;
    return true;
}

// Runs engine->wait() on a helper thread; on deadline dumps state and exits
// the process: the runtime underneath is wedged, and tearing the engine down
// from here would race its fibers.
bool bounded_stop_and_wait(EngineRun& run, std::chrono::steady_clock::duration timeout,
                           std::optional<TailResult>& out, long& latency_ms) {
    std::promise<sluice::Result<TailResult>> done;
    auto fut = done.get_future();
    const auto t0 = std::chrono::steady_clock::now();
    std::thread waiter([&run, &done] {
        run.engine->request_stop();
        run.engine->request_stop();
        done.set_value(run.engine->wait());
    });
    if (fut.wait_for(timeout) != std::future_status::ready) {
        std::fprintf(stderr, "probe log at hang:\n");
        tail_probe::dump_log("probe");
        std::fprintf(stderr, "sink lines at hang:\n");
        for (const std::string& l : run.sink.lines_copy())
            std::fprintf(stderr, "  line: %s\n", l.c_str());
        std::fprintf(stderr, "diag at hang: %s\n", run.sink.diag_copy().c_str());
        std::fflush(stderr);
        std::_Exit(97);
    }
    waiter.join();
    latency_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    sluice::Result<TailResult> res = fut.get();
    if (res.has_value())
        out = std::move(res.value());
    return res.has_value();
}

// ---------------------------------------------------------------------------

bool idle_follow_stops_bounded() {
    TempFile f("");
    struct stat st {};
    if (::stat(f.path.c_str(), &st) != 0)
        deadline_abort("stat failed");
    tail_probe::arm(st.st_dev, st.st_ino, -1);

    EngineRun run(f, follow_options(0));
    if (!run.engine->start().has_value()) {
        check_msg(false, "idle: engine start");
        tail_probe::disarm();
        return false;
    }

    std::size_t cursor = 0;
    tail_probe::Record rec;
    // PRE seq 1 = the follow loop's first EOF probe at offset 0 on the empty
    // file: deterministic proof the task reached its idle poll state.
    if (!wait_for_record("idle: first follow probe", tail_probe::Record::Kind::pre, 1, nullptr,
                         kEventDeadline, cursor, rec)) {
        tail_probe::disarm();
        return false;
    }
    check_msg(rec.offset == 0 && rec.len > 0, "idle: probe1 is the follow EOF read");

    std::optional<TailResult> result;
    long latency_ms = 0;
    const bool waited = bounded_stop_and_wait(run, kSettleDeadline, result, latency_ms);
    check_msg(waited, "idle: bounded stop+wait");
    tail_probe::disarm();
    if (!waited || !result.has_value())
        return false;
    check_msg(result->stopped_by_cancel, "idle: stopped_by_cancel");
    check_msg(!result->error.has_value(), "idle: no error");
    check_msg(result->lines_emitted == 0, "idle: no lines");
    check_msg(run.sink.lines_copy().empty(), "idle: sink empty");
    std::fprintf(stderr, "idle stop latency: %ld ms\n", latency_ms);
    return result->stopped_by_cancel && !result->error.has_value() && result->lines_emitted == 0;
}

bool lifecycle_api_states() {
    {
        TempFile f("a\nb\nc\n");
        TailOptions o;
        o.lines = 2;
        EngineRun run(f, o);
        auto early = run.engine->wait();
        check_msg(!early.has_value() && early.error().code == sluice::IoError::Code::invalid_state,
                  "lifecycle: wait before start is invalid_state");
    }
    {
        TempFile f("a\nb\nc\nd\n");
        TailOptions o;
        o.lines = 2;
        EngineRun run(f, o);
        run.engine->request_stop();
        check_msg(run.engine->start().has_value(), "lifecycle: start after pre-start stop no-op");
        auto first = run.engine->wait();
        check_msg(first.has_value(), "lifecycle: first wait succeeds");
        check_msg(first.has_value() && !first.value().stopped_by_cancel,
                  "lifecycle: pre-start stop did not reach the task");
        check_msg(first.has_value() && first.value().lines_emitted == 2,
                  "lifecycle: finite tail unaffected by pre-start stop");
        auto second = run.engine->wait();
        check_msg(!second.has_value() &&
                      second.error().code == sluice::IoError::Code::invalid_state,
                  "lifecycle: second wait is invalid_state");
        run.engine->request_stop();
    }
    {
        TempFile f("x\n");
        TailOptions o;
        o.poll_interval_ms = 49;
        o.follow = true;
        EngineRun run(f, o);
        check_msg(!run.engine->start().has_value(), "lifecycle: poll 49 < min refused");
        auto w = run.engine->wait();
        check_msg(!w.has_value() && w.error().code == sluice::IoError::Code::invalid_state,
                  "lifecycle: wait after refused start is invalid_state");
    }
    return g_checks_failed == 0;
}

bool pending_read_settles_before_retire() {
    TempFile f("");
    struct stat st {};
    if (::stat(f.path.c_str(), &st) != 0)
        deadline_abort("stat failed");
    tail_probe::arm(st.st_dev, st.st_ino, 1);

    EngineRun run(f, follow_options(0));
    if (!run.engine->start().has_value()) {
        check_msg(false, "pending: engine start");
        tail_probe::disarm();
        return false;
    }

    std::size_t cursor = 0;
    tail_probe::Record rec;
    // Round 1: the task's first follow read is held before any append can
    // land, so it is necessarily the empty-file EOF probe.
    if (!wait_for_record("pending: first follow read held", tail_probe::Record::Kind::entered, 1,
                         nullptr, kEventDeadline, cursor, rec)) {
        tail_probe::disarm();
        return false;
    }
    tail_probe::release();
    tail_probe::Record probe_exit;
    if (!wait_for_record("pending: first read completed", tail_probe::Record::Kind::exit, 1,
                         nullptr, kEventDeadline, cursor, probe_exit)) {
        tail_probe::disarm();
        return false;
    }
    check_msg(probe_exit.ret == 0, "pending: first read was the empty-file probe");

    // The task now sleeps one 50 ms slice before re-reading; the append lands
    // inside that window, so the next held read is the data read.
    check_msg(f.append("held\n"), "pending: append");

    tail_probe::Record entered;
    if (!wait_for_record("pending: worker entered data read", tail_probe::Record::Kind::entered,
                         2, nullptr, kEventDeadline, cursor, entered)) {
        tail_probe::disarm();
        return false;
    }
    check_msg(entered.offset == 0, "pending: data read at offset 0");

    std::promise<sluice::Result<TailResult>> done;
    auto fut = done.get_future();
    std::thread waiter([&run, &done] {
        run.engine->request_stop();
        done.set_value(run.engine->wait());
    });

    // While the accepted read is unsettled, the task must not retire and
    // wait() must not return.
    const bool stayed = run.sink.quiet_for(kNoRetireWindow);
    const bool retired_early = fut.wait_for(0s) == std::future_status::ready;
    check_msg(!retired_early, "pending: wait() must not return before settlement");
    check_msg(stayed, "pending: no emission while read unsettled");

    tail_probe::release();
    if (fut.wait_for(kSettleDeadline) != std::future_status::ready) {
        check_msg(false, "pending: wait() after release (deadline)");
        std::fprintf(stderr, "probe log at hang:\n");
        tail_probe::dump_log("probe");
        std::fflush(stderr);
        std::_Exit(97);
    }
    waiter.join();
    tail_probe::disarm();

    sluice::Result<TailResult> res = fut.get();
    check_msg(res.has_value(), "pending: wait() success");
    if (!res.has_value())
        return false;
    std::optional<TailResult> result = std::move(res.value());
    check_msg(result->stopped_by_cancel, "pending: stopped_by_cancel");
    check_msg(!result->error.has_value(), "pending: no error");
    check_msg(result->lines_emitted == 1, "pending: exactly the held line counted");

    tail_probe::Record exit_rec;
    if (!wait_for_record("pending: physical exit of data read", tail_probe::Record::Kind::exit, 2,
                         nullptr, kEventDeadline, cursor, exit_rec))
        return false;
    check_msg(exit_rec.ret == 5, "pending: data read returned 5 bytes");

    const auto lines = run.sink.lines_copy();
    check_msg(lines.size() == 1 && lines[0] == "held",
              "pending: held line delivered exactly once");
    return result->stopped_by_cancel && !result->error.has_value() && lines.size() == 1 &&
           lines[0] == "held";
}

bool append_delivery_and_partial_withholding() {
    TempFile f("");
    struct stat st {};
    if (::stat(f.path.c_str(), &st) != 0)
        deadline_abort("stat failed");
    tail_probe::arm(st.st_dev, st.st_ino, -1);

    EngineRun run(f, follow_options(0));
    if (!run.engine->start().has_value()) {
        check_msg(false, "partial: engine start");
        tail_probe::disarm();
        return false;
    }

    std::size_t cursor = 0;
    tail_probe::Record rec;
    if (!wait_for_record("partial: initial EOF probe", tail_probe::Record::Kind::pre, 1, nullptr,
                         kEventDeadline, cursor, rec)) {
        tail_probe::disarm();
        return false;
    }

    check_msg(f.append("ap-one\n"), "partial: append one");
    check_msg(run.sink.wait_line("ap-one", kEventDeadline), "partial: ap-one delivered");
    // Idle cycles also probe (ret 0 at the new EOF), so each content read is
    // anchored by its unique nonzero return, never by log position.
    tail_probe::Record one_exit;
    if (!wait_for_record("partial: ap-one read completed", tail_probe::Record::Kind::exit, -1,
                         [](const tail_probe::Record& r) { return r.ret == 7; }, kEventDeadline,
                         cursor, one_exit)) {
        tail_probe::disarm();
        return false;
    }

    check_msg(f.append("par"), "partial: append fragment");
    tail_probe::Record par_exit;
    if (!wait_for_record("partial: fragment physically read", tail_probe::Record::Kind::exit, -1,
                         [](const tail_probe::Record& r) { return r.ret == 3; }, kEventDeadline,
                         cursor, par_exit)) {
        tail_probe::disarm();
        return false;
    }

    check_msg(run.sink.quiet_for(kQuietWindow), "partial: fragment withheld before newline");

    check_msg(f.append("\n"), "partial: append newline");
    check_msg(run.sink.wait_line("par", kEventDeadline), "partial: par delivered after newline");

    std::optional<TailResult> result;
    long latency_ms = 0;
    const bool waited = bounded_stop_and_wait(run, kSettleDeadline, result, latency_ms);
    tail_probe::disarm();
    check_msg(waited && result.has_value() && result->stopped_by_cancel && !result->error.has_value(),
              "partial: clean stop after deliveries");
    if (!waited || !result.has_value())
        return false;
    const auto lines = run.sink.lines_copy();
    check_msg(lines.size() == 2 && lines[0] == "ap-one" && lines[1] == "par",
              "partial: exactly-once ordered delivery");
    return lines.size() == 2 && lines[0] == "ap-one" && lines[1] == "par";
}

bool truncate_resets_partial_carry() {
    TempFile f("");
    struct stat st {};
    if (::stat(f.path.c_str(), &st) != 0)
        deadline_abort("stat failed");
    tail_probe::arm(st.st_dev, st.st_ino, -1);

    EngineRun run(f, follow_options(0));
    if (!run.engine->start().has_value()) {
        check_msg(false, "truncate: engine start");
        tail_probe::disarm();
        return false;
    }

    std::size_t cursor = 0;
    tail_probe::Record rec;
    if (!wait_for_record("truncate: initial EOF probe", tail_probe::Record::Kind::pre, 1, nullptr,
                         kEventDeadline, cursor, rec)) {
        tail_probe::disarm();
        return false;
    }

    check_msg(f.append("carry"), "truncate: append carry fragment");
    // Idle cycles probe with ret 0; only the carry read completes with 5.
    tail_probe::Record exit_rec;
    if (!wait_for_record("truncate: carry read completed", tail_probe::Record::Kind::exit, -1,
                         [](const tail_probe::Record& r) { return r.ret == 5; }, kEventDeadline,
                         cursor, exit_rec)) {
        tail_probe::disarm();
        return false;
    }
    check_msg(exit_rec.ret == 5, "truncate: carry fragment fully read (5 bytes)");

    check_msg(f.truncate_to(0), "truncate: ftruncate to 0");
    check_msg(run.sink.wait_diag("file truncated", kEventDeadline),
              "truncate: truncation processed (diag anchor)");

    check_msg(f.append("after\n"), "truncate: append after");
    check_msg(run.sink.wait_line("after", kEventDeadline), "truncate: after delivered");

    std::optional<TailResult> result;
    long latency_ms = 0;
    const bool waited = bounded_stop_and_wait(run, kSettleDeadline, result, latency_ms);
    tail_probe::disarm();
    check_msg(waited && result.has_value() && result->stopped_by_cancel && !result->error.has_value(),
              "truncate: clean stop");
    if (!waited || !result.has_value())
        return false;
    check_msg(result->truncation_detected, "truncate: truncation_detected in result");

    const auto lines = run.sink.lines_copy();
    check_msg(lines.size() == 1 && lines[0] == "after",
              "truncate: carry discarded (no carry-after concatenation)");
    return result->truncation_detected && lines.size() == 1 && lines[0] == "after";
}

bool descriptor_follow_across_rename() {
    TempFile f("");
    const std::string rotated = f.path + ".rotated";
    struct stat st {};
    if (::stat(f.path.c_str(), &st) != 0)
        deadline_abort("stat failed");
    tail_probe::arm(st.st_dev, st.st_ino, -1);

    EngineRun run(f, follow_options(0));
    if (!run.engine->start().has_value()) {
        check_msg(false, "rotation: engine start");
        tail_probe::disarm();
        return false;
    }

    std::size_t cursor = 0;
    tail_probe::Record rec;
    if (!wait_for_record("rotation: initial EOF probe", tail_probe::Record::Kind::pre, 1, nullptr,
                         kEventDeadline, cursor, rec)) {
        tail_probe::disarm();
        return false;
    }

    check_msg(f.append("rot-a\n"), "rotation: append to original");
    check_msg(run.sink.wait_line("rot-a", kEventDeadline), "rotation: rot-a delivered");

    if (::rename(f.path.c_str(), rotated.c_str()) != 0)
        deadline_abort("rename failed");
    const int nfd = ::open(f.path.c_str(), O_WRONLY | O_CREAT, 0644);
    if (nfd < 0)
        deadline_abort("new path file create failed");
    ::close(nfd);

    {
        const int af = ::open(rotated.c_str(), O_WRONLY | O_APPEND);
        if (af < 0)
            deadline_abort("rotated append open failed");
        const std::string data = "rot-b\n";
        const ssize_t w = ::write(af, data.data(), data.size());
        ::close(af);
        check_msg(w == static_cast<ssize_t>(data.size()), "rotation: append to original inode");
    }
    check_msg(run.sink.wait_line("rot-b", kEventDeadline),
              "rotation: original-inode append delivered");

    {
        const int af = ::open(f.path.c_str(), O_WRONLY | O_APPEND);
        if (af < 0)
            deadline_abort("new path append open failed");
        const std::string data = "newp\n";
        const ssize_t w = ::write(af, data.data(), data.size());
        ::close(af);
        check_msg(w == static_cast<ssize_t>(data.size()), "rotation: append to new path");
    }
    check_msg(run.sink.quiet_for(kQuietWindow), "rotation: new-path content never delivered");
    check_msg(run.sink.diag_copy().empty(), "rotation: no truncation diag from new path");

    std::optional<TailResult> result;
    long latency_ms = 0;
    const bool waited = bounded_stop_and_wait(run, kSettleDeadline, result, latency_ms);
    tail_probe::disarm();
    ::unlink(rotated.c_str());
    check_msg(waited && result.has_value() && result->stopped_by_cancel && !result->error.has_value(),
              "rotation: clean stop");
    if (!waited || !result.has_value())
        return false;
    check_msg(!result->truncation_detected, "rotation: no truncation flag");
    const auto lines = run.sink.lines_copy();
    check_msg(lines.size() == 2 && lines[0] == "rot-a" && lines[1] == "rot-b",
              "rotation: only original-inode lines, exactly once");
    return lines.size() == 2 && lines[0] == "rot-a" && lines[1] == "rot-b";
}

}  // namespace

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"idle_follow_stops_bounded", idle_follow_stops_bounded},
        {"lifecycle_api_states", lifecycle_api_states},
        {"pending_read_settles_before_retire", pending_read_settles_before_retire},
        {"append_delivery_and_partial_withholding", append_delivery_and_partial_withholding},
        {"truncate_resets_partial_carry", truncate_resets_partial_carry},
        {"descriptor_follow_across_rename", descriptor_follow_across_rename},
    };

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
        std::fprintf(stderr, "FAIL: %d of %zu tail lifecycle oracle cases failed\n", failed_cases,
                     sizeof(tests) / sizeof(tests[0]));
        return 1;
    }
    std::printf("all %zu tail lifecycle oracle cases passed\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
