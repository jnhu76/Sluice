// sluice-tail engine tests: last-N backward scan + follow mode + truncation
// through the SAME tail_task.cpp the CLI uses, against real files and the
// real ThreadPoolBackend. Follow cases use a writer thread + condition
// variables (bounded waits, never sleep-as-proof) and end via
// request_stop() — the documented follow-termination path.
#include "harness.hpp"

#include "tail_task.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

#include <string>
#include <vector>

using namespace sluice_tail;

namespace {

struct TempPath {
    int fd;
    std::string path;
    TempPath(const char* tag) {
        std::string tmpl = std::string("/tmp/sluice_tail_t_") + tag + "_XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        fd = ::mkstemp(buf.data());
        SLUICE_CHECK(fd >= 0);
        path = buf.data();
    }
    ~TempPath() {
        if (fd >= 0) ::close(fd);
        if (!path.empty()) ::unlink(path.c_str());
    }
    void write_all(const std::string& s) {
        SLUICE_CHECK(::pwrite(fd, s.data(), s.size(), 0) ==
                     static_cast<ssize_t>(s.size()));
    }
    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;
};

TailOptions default_opts() {
    TailOptions o;
    o.follow = false;
    o.buffer_size = 4096;
    return o;
}

struct TailRunOut {
    std::vector<std::string> lines;
    TailResult result;
};

// SLUICE_CHECK bare-returns on failure; this variant returns the (empty)
// result object so a failed precondition still produces a value.
#define TAIL_CHECK(cond)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ::sluice_test::record_failure(__FILE__, __LINE__, #cond);          \
            return {};                                                         \
        }                                                                      \
    } while (0)

TailRunOut run_tail_once(int fd, TailOptions o, std::string* diag_out = nullptr) {
    TailRunOut out;
    TailEngine engine(
        fd, o,
        [&](std::string_view l) { out.lines.emplace_back(l); },
        [&](std::string_view m) {
            if (diag_out) diag_out->append(m);
        });
    TAIL_CHECK(engine.start().has_value());
    auto r = engine.wait();
    TAIL_CHECK(r.has_value());
    out.result = r.value();
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// last-N
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(tail_last_zero_emits_nothing) {
    TempPath f("n0");
    f.write_all("a\nb\nc\n");
    TailOptions o = default_opts();
    o.lines = 0;
    auto out = run_tail_once(f.fd, o);
    SLUICE_CHECK(out.lines.empty());
}

SLUICE_TEST_CASE(tail_last_one_and_n) {
    TempPath f("n1");
    f.write_all("l1\nl2\nl3\nl4\nl5\n");
    {
        TailOptions o = default_opts();
        o.lines = 1;
        auto out = run_tail_once(f.fd, o);
        SLUICE_CHECK(out.lines.size() == 1 && out.lines[0] == "l5");
    }
    {
        TailOptions o = default_opts();
        o.lines = 3;
        auto out = run_tail_once(f.fd, o);
        SLUICE_CHECK(out.lines.size() == 3);
        SLUICE_CHECK(out.lines[0] == "l3" && out.lines[1] == "l4" &&
                     out.lines[2] == "l5");
    }
}

SLUICE_TEST_CASE(tail_n_greater_than_total_emits_whole_file) {
    TempPath f("nbig");
    f.write_all("only\ntwo\n");
    TailOptions o = default_opts();
    o.lines = 100;
    auto out = run_tail_once(f.fd, o);
    SLUICE_CHECK(out.lines.size() == 2);
    SLUICE_CHECK(out.lines[0] == "only" && out.lines[1] == "two");
}

SLUICE_TEST_CASE(tail_final_line_without_newline_counted) {
    TempPath f("nonl");
    f.write_all("a\nb\nc-no-newline");
    TailOptions o = default_opts();
    o.lines = 2;
    auto out = run_tail_once(f.fd, o);
    SLUICE_CHECK(out.lines.size() == 2);
    SLUICE_CHECK(out.lines[0] == "b" && out.lines[1] == "c-no-newline");
}

SLUICE_TEST_CASE(tail_empty_file) {
    TempPath f("empty");
    TailOptions o = default_opts();
    auto out = run_tail_once(f.fd, o);
    SLUICE_CHECK(out.lines.empty());
}

SLUICE_TEST_CASE(tail_large_file_bounded_backward_scan) {
    // 200 KiB / 8 KiB buffer: the backward scan must find the last 10 of
    // 3400 lines without forward-scanning the whole file (correctness is
    // observable; the bounded window is by construction).
    TempPath f("big");
    std::string data;
    for (int i = 1; i <= 3400; ++i)
        data += "line-" + std::to_string(i) + "\n";
    f.write_all(data);

    TailOptions o = default_opts();
    o.lines = 10;
    auto out = run_tail_once(f.fd, o);
    SLUICE_CHECK(out.lines.size() == 10);
    SLUICE_CHECK(out.lines.front() == "line-3391");
    SLUICE_CHECK(out.lines.back() == "line-3400");
}

SLUICE_TEST_CASE(tail_long_line_skipped_with_diag) {
    TempPath f("long");
    std::string data = "keep\n" + std::string(5000, 'x') + "\nalso keep\n";
    f.write_all(data);
    TailOptions o = default_opts();
    o.lines = 10;
    o.max_line_bytes = 100;
    std::string diag;
    auto out = run_tail_once(f.fd, o, &diag);
    SLUICE_CHECK(out.lines.size() == 2);
    SLUICE_CHECK(out.lines[0] == "keep" && out.lines[1] == "also keep");
    SLUICE_CHECK(out.result.dropped_long_lines);
    SLUICE_CHECK(diag.find("max-line-bytes") != std::string::npos);
}

// ---------------------------------------------------------------------------
// follow
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(tail_follow_appends_and_cancels_cleanly) {
    TempPath f("follow");
    f.write_all("start1\nstart2\n");

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::string> lines;
    std::size_t seen = 0;

    TailOptions o = default_opts();
    o.follow = true;
    o.lines = 1;
    o.poll_interval_ms = kMinPollMs;

    TailEngine engine(
        f.fd, o,
        [&](std::string_view l) {
            std::lock_guard<std::mutex> lk(mtx);
            lines.emplace_back(l);
            ++seen;
            cv.notify_all();
        },
        nullptr);
    SLUICE_CHECK(engine.start().has_value());

    // RAII: see the truncation case — always leave a quiescent engine.
    struct EngineGuard {
        TailEngine& e;
        ~EngineGuard() {
            e.request_stop();
            (void)e.wait();
        }
    } guard{engine};

    // Wait for the initial tail line (bounded; a hang fails the gate's
    // watchdog rather than sleeping forever).
    {
        std::unique_lock<std::mutex> lk(mtx);
        SLUICE_CHECK(cv.wait_for(lk, std::chrono::seconds(5),
                                 [&] { return seen >= 1; }));
    }
    {
        std::lock_guard<std::mutex> lk(mtx);
        SLUICE_CHECK(lines.size() == 1 && lines[0] == "start2");
    }

    // Append two lines from outside; follow must deliver them. The initial
    // file is exactly 14 bytes ("start1\nstart2\n"); appends land at EOF.
    std::string append1 = "appended-A\n";  // 11 bytes -> EOF at 14
    SLUICE_CHECK(::pwrite(f.fd, append1.data(), append1.size(), 14) ==
                 static_cast<ssize_t>(append1.size()));
    {
        std::unique_lock<std::mutex> lk(mtx);
        SLUICE_CHECK(cv.wait_for(lk, std::chrono::seconds(5),
                                 [&] { return seen >= 2; }));
    }
    std::string append2 = "appended-B\n";  // -> EOF at 25
    SLUICE_CHECK(::pwrite(f.fd, append2.data(), append2.size(), 25) ==
                 static_cast<ssize_t>(append2.size()));
    {
        std::unique_lock<std::mutex> lk(mtx);
        SLUICE_CHECK(cv.wait_for(lk, std::chrono::seconds(5),
                                 [&] { return seen >= 3; }));
    }
    {
        std::lock_guard<std::mutex> lk(mtx);
        SLUICE_CHECK(lines[1] == "appended-A");
        SLUICE_CHECK(lines[2] == "appended-B");
    }

    // Documented follow termination: request_stop -> clean result.
    engine.request_stop();
    auto r = engine.wait();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(!r.value().error.has_value());
    SLUICE_CHECK(r.value().stopped_by_cancel);
    SLUICE_CHECK(r.value().lines_emitted == 3);
}

SLUICE_TEST_CASE(tail_follow_truncation_detected) {
    TempPath f("trunc");
    f.write_all("one\ntwo\n");

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::string> lines;
    std::size_t seen = 0;
    std::string diag;

    TailOptions o = default_opts();
    o.follow = true;
    // Initial tail = ORDERING ANCHOR: delivering "two" proves the task
    // already snapshotted the file size, so the truncation below can only be
    // observed as a shrink (the follow contract: changes AFTER start).
    o.lines = 1;
    o.poll_interval_ms = kMinPollMs;

    TailEngine engine(
        f.fd, o,
        [&](std::string_view l) {
            std::lock_guard<std::mutex> lk(mtx);
            lines.emplace_back(l);
            ++seen;
            cv.notify_all();
        },
        [&](std::string_view m) {
            std::lock_guard<std::mutex> lk(mtx);
            diag.append(m);
            cv.notify_all();
        });
    SLUICE_CHECK(engine.start().has_value());

    // RAII: whatever fails below, stop + wait the engine so its destruction
    // sees a quiescent Runtime (destruction with a running follow task is a
    // contract violation and fail-fasts by design).
    struct EngineGuard {
        TailEngine& e;
        ~EngineGuard() {
            e.request_stop();
            (void)e.wait();
        }
    } guard{engine};

    // Wait for the initial tail line (the ordering anchor).
    {
        std::unique_lock<std::mutex> lk(mtx);
        SLUICE_CHECK(cv.wait_for(lk, std::chrono::seconds(5),
                                 [&] { return seen >= 1; }));
        SLUICE_CHECK(lines.size() == 1 && lines[0] == "two");
    }

    // Truncate to zero and write a fresh line: follow must report the
    // truncation and deliver the new content from offset 0.
    SLUICE_CHECK(::ftruncate(f.fd, 0) == 0);
    std::string fresh = "fresh\n";
    SLUICE_CHECK(::pwrite(f.fd, fresh.data(), fresh.size(), 0) ==
                 static_cast<ssize_t>(fresh.size()));

    {
        std::unique_lock<std::mutex> lk(mtx);
        SLUICE_CHECK(cv.wait_for(lk, std::chrono::seconds(5), [&] {
            return seen >= 2 && diag.find("truncated") != std::string::npos;
        }));
        SLUICE_CHECK(lines.size() == 2 && lines[1] == "fresh");
    }

    engine.request_stop();
    auto r = engine.wait();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(!r.value().error.has_value());
    SLUICE_CHECK(r.value().truncation_detected);
}

// ---------------------------------------------------------------------------
// Review-required regressions (PR #122 review)
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(tail_follow_long_line_reports_diagnostic) {
    // Review finding #1: an over-cap line appended during FOLLOW used to be
    // silently dropped; the documented policy (plan §3.4 / README) is
    // "reported to stderr and skipped" in BOTH phases.
    TempPath f("followlong");
    f.write_all("seed\n");

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::string> lines;
    std::size_t seen = 0;
    std::string diag;

    TailOptions o = default_opts();
    o.follow = true;
    // Initial tail = ORDERING ANCHOR: delivering "seed" proves the task
    // already snapshotted the size, so the append below is follow data.
    o.lines = 1;
    o.max_line_bytes = 100;
    o.poll_interval_ms = kMinPollMs;

    TailEngine engine(
        f.fd, o,
        [&](std::string_view l) {
            std::lock_guard<std::mutex> lk(mtx);
            lines.emplace_back(l);
            ++seen;
            cv.notify_all();
        },
        [&](std::string_view m) {
            std::lock_guard<std::mutex> lk(mtx);
            diag.append(m);
            cv.notify_all();
        });
    SLUICE_CHECK(engine.start().has_value());
    struct EngineGuard {
        TailEngine& e;
        ~EngineGuard() {
            e.request_stop();
            (void)e.wait();
        }
    } guard{engine};

    {
        std::unique_lock<std::mutex> lk(mtx);
        SLUICE_CHECK(cv.wait_for(lk, std::chrono::seconds(5),
                                 [&] { return seen >= 1; }));
        SLUICE_CHECK(lines.size() == 1 && lines[0] == "seed");
    }

    // A normal short line plus an over-cap line: the short one is delivered,
    // the long one is reported and skipped.
    std::string append = "short\n" + std::string(400, 'x') + "\n";
    SLUICE_CHECK(::pwrite(f.fd, append.data(), append.size(), 5) ==
                 static_cast<ssize_t>(append.size()));

    {
        std::unique_lock<std::mutex> lk(mtx);
        SLUICE_CHECK(cv.wait_for(lk, std::chrono::seconds(5), [&] {
            return seen >= 2 &&
                   diag.find("max-line-bytes") != std::string::npos;
        }));
        SLUICE_CHECK(lines.size() == 2 && lines[1] == "short");
    }

    engine.request_stop();
    auto r = engine.wait();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(!r.value().error.has_value());
    SLUICE_CHECK(r.value().dropped_long_lines);
    SLUICE_CHECK(r.value().lines_emitted == 2);
}

SLUICE_TEST_CASE(tail_follow_no_duplicate_when_file_grows_during_tail) {
    // Review finding #2: the initial-tail forward pass can advance PAST the
    // size snapshot when the file grows mid-scan; follow must continue from
    // the pass's final offset, not the stale snapshot, or bytes already
    // emitted by the tail are re-emitted (duplicates).
    TempPath f("growrace");
    // 3000 lines (~230 KB): with a 4096-byte buffer the forward pass over
    // the last 2000 lines takes ~55 reads, leaving a wide window for the
    // append below to land mid-pass.
    std::string seed;
    for (int i = 1; i <= 3000; ++i)
        seed += "grow-line-" + std::to_string(i) + "\n";
    f.write_all(seed);
    const std::uint64_t seed_size = seed.size();

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::string> lines;
    std::size_t seen = 0;

    TailOptions o = default_opts();  // buffer 4096
    o.follow = true;
    o.lines = 2000;
    o.poll_interval_ms = kMinPollMs;

    TailEngine engine(
        f.fd, o,
        [&](std::string_view l) {
            std::lock_guard<std::mutex> lk(mtx);
            lines.emplace_back(l);
            ++seen;
            cv.notify_all();
        },
        nullptr);
    SLUICE_CHECK(engine.start().has_value());
    struct EngineGuard {
        TailEngine& e;
        ~EngineGuard() {
            e.request_stop();
            (void)e.wait();
        }
    } guard{engine};

    // Wait until the forward pass is partway through the initial tail...
    {
        std::unique_lock<std::mutex> lk(mtx);
        SLUICE_CHECK(cv.wait_for(lk, std::chrono::seconds(5),
                                 [&] { return seen >= 1; }));
    }
    // ...then append a unique line mid-pass (races the pass's final reads).
    std::string extra = "TAILRACE-UNIQUE\n";
    SLUICE_CHECK(::pwrite(f.fd, extra.data(), extra.size(),
                          static_cast<off_t>(seed_size)) ==
                 static_cast<ssize_t>(extra.size()));
    // Wait for the append to be delivered by follow.
    {
        std::unique_lock<std::mutex> lk(mtx);
        SLUICE_CHECK(cv.wait_for(lk, std::chrono::seconds(5), [&] {
            for (auto& l : lines)
                if (l == "TAILRACE-UNIQUE") return true;
            return false;
        }));
    }

    engine.request_stop();
    auto r = engine.wait();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(!r.value().error.has_value());

    // Exactly-once: 2000 initial-tail lines + 1 appended line (whichever
    // path delivered the append — forward overrun or follow — it must appear
    // once, never twice).
    std::size_t hits = 0;
    for (auto& l : lines)
        if (l == "TAILRACE-UNIQUE") ++hits;
    SLUICE_CHECK(hits == 1);
    SLUICE_CHECK(r.value().lines_emitted == lines.size());
    SLUICE_CHECK(lines.size() == 2001);
}

SLUICE_MAIN()
