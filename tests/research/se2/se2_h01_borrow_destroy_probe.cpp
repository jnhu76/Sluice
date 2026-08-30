// SE-2 probe P-S01 — SE1-CA-H01-1 borrow-destroy experiment (task §18 REQUIRED).
//
// ONE hazard: the caller destroys the borrowed buffer while an async read
// that borrowed it is still outstanding. ONE semantic violation: the borrow
// contract (buffer must stay alive and address-stable for the request
// lifetime). ONE expected observation per mode: Sluice does NOT detect the
// violation — the accepted request completes and the Completion publishes a
// success. Detection, where it exists, comes from ASan observing the real
// write into freed memory.
//
// Modes (exactly one borrow-destroy scenario per run):
//   --mode=fake        FakeAsyncBackend: submit -> delete[] borrow ->
//                      complete_oldest_with_bytes -> reap. The fake backend
//                      never dereferences the borrow, so this run answers the
//                      SLUICE-LAYER question only (no fail-fast?) with zero UB
//                      executed: expected exit 0 under plain AND ASan.
//   --mode=threadpool  ThreadPoolBackend + pipe: submit -> delete[] borrow ->
//                      write 13 bytes -> the worker's real read(2) writes into
//                      freed memory -> Completion publishes success. Plain:
//                      exit 0, silent UAF. ASan: heap-use-after-free abort.
//
// Expected outcomes (pre-registered in SE-2 audit artifact, task §45):
//   fake        plain exit 0, value published; ASan exit 0, clean.
//   threadpool  plain exit 0 (20 repeats), value published; ASan: non-zero
//               exit with heap-use-after-free report.
//
// RESEARCH PROBE — deliberately NOT add_tests-registered (xmake/research.lua);
// it must never run in default test groups. It is executed explicitly per the
// SE-2 execution plan, and the threadpool mode is ASan-only evidence for the
// ASan layer cell.
//
// Build: xmake -m debug/x -m asanubsan; run: se2_h01_borrow_destroy_probe [--mode=...][--repeats=N]

#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/threadpool_backend.hpp>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

using namespace sluice::async;

namespace {

int run_fake(std::size_t /*repeats*/) {
    FakeAsyncBackend backend;
    int fds[2]{};
    if (::pipe(fds) != 0) {
        std::fprintf(stderr, "SE2-H01: pipe failed\n");
        return 2;
    }

    Completion<std::size_t> c;
    std::byte* buf = new std::byte[64];
    std::memset(buf, 0xAB, 64);

    auto sub = backend.submit_read(ReadOp{fds[0], buf, 64, 0}, c);
    if (!sub.has_value()) {
        std::fprintf(stderr, "SE2-H01(fake): submit rejected (code=%d) — would contradict expected MISSES\n",
                     static_cast<int>(sub.error().code));
        ::close(fds[0]);
        ::close(fds[1]);
        delete[] buf;
        return 2;
    }

    // THE VIOLATION: borrower destroys the borrow while the request is
    // outstanding. No Sluice API is consulted; the contract is caller-side.
    delete[] buf;

    // Complete and reap. The fake backend never touches the borrow: no UB is
    // executed in this mode, so ASan is expected clean here.
    backend.complete_oldest_with_bytes(64);
    auto wr = backend.wait_one();

    const bool ready = c.ready();
    const bool ok = ready && c.result().has_value() && wr.has_value();
    std::printf(
        "SE2-H01(fake): submit=accepted violation=executed reap=%s completion_ready=%s "
        "result=%s — SLUICE LAYER: no fail-fast, success published (MISSES)\n",
        wr.has_value() ? "ok" : "error", ready ? "yes" : "no",
        ok ? "ok(64 bytes reported for a freed borrow)" : "n/a");

    ::close(fds[0]);
    ::close(fds[1]);
    if (c.ready()) {
        c.reset();
    }
    return ok ? 0 : 2;
}

int run_threadpool(std::size_t repeats) {
    // A regular temp file (not a pipe): the backend reads with a positional
    // read, and pipes do not support positional reads.
    char path[128];
    std::snprintf(path, sizeof(path), "/tmp/se2_h01_borrow_%d.bin", static_cast<int>(::getpid()));
    {
        FILE* f = std::fopen(path, "wb");
        if (!f) {
            std::fprintf(stderr, "SE2-H01: temp file open failed\n");
            return 2;
        }
        static const char payload[65] = "SE2H01-SEED-SE2H01-SEED-SE2H01-SEED-SE2H01-SEED-SE2H01-SEED-XXXX";
        std::fwrite(payload, 1, sizeof(payload), f);
        std::fclose(f);
    }

    for (std::size_t i = 0; i < repeats; ++i) {
        int fd = ::open(path, O_RDONLY);
        if (fd < 0) {
            std::fprintf(stderr, "SE2-H01: temp file open failed\n");
            return 2;
        }

        {
            ThreadPoolBackend backend;
            Completion<std::size_t> c;
            std::byte* buf = new std::byte[64];
            std::memset(buf, 0xAB, 64);

            auto sub = backend.submit_read(ReadOp{fd, buf, 64, 0}, c);
            if (!sub.has_value()) {
                std::fprintf(stderr, "SE2-H01(threadpool): submit rejected — would contradict expected MISSES\n");
                delete[] buf;
                ::close(fd);
                return 2;
            }

            // THE VIOLATION: destroy the borrow before completion. The
            // worker's real positional read(2) then writes into freed memory.
            delete[] buf;

            // Reap: reap publishes the Completion. No Sluice layer validates
            // the borrow lifetime.
            std::size_t events = 0;
            bool ready = false;
            while (events < 8 && !ready) {  // bounded reap loop
                auto wr = backend.wait_one();
                if (!wr.has_value()) {
                    break;
                }
                events += wr.value();
                ready = c.ready();
            }

            const bool ok = ready && c.result().has_value();
            std::printf(
                "SE2-H01(threadpool): iter=%zu submit=accepted violation=executed completion_ready=%s "
                "result=%s — SLUICE LAYER: no fail-fast, success published (MISSES)\n",
                i, ready ? "yes" : "no", ok ? "ok(64 bytes delivered into freed borrow)" : "n/a");

            if (c.ready()) {
                c.reset();
            }
            if (!ok) {
                ::close(fd);
                ::unlink(path);
                return 2;
            }
        }
        ::close(fd);
    }
    ::unlink(path);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode = "fake";
    std::size_t repeats = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--mode=", 0) == 0) {
            mode = a.substr(7);
        } else if (a.rfind("--repeats=", 0) == 0) {
            repeats = static_cast<std::size_t>(std::atoi(a.c_str() + 10));
        }
    }
    if (mode == "fake") {
        return run_fake(repeats);
    }
    if (mode == "threadpool") {
        return run_threadpool(repeats);
    }
    std::fprintf(stderr, "SE2-H01: unknown mode '%s' (fake|threadpool)\n", mode.c_str());
    return 2;
}
