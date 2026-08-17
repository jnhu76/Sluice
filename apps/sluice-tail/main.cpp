// sluice-tail — bounded last-N + follow-mode tail.
//
// CLI:
//   sluice-tail [options] <file>
// Options:
//   -n <count>              last N lines (default 10; 0 = none)
//   -f                      follow (descriptor semantics: the fd is followed,
//                           not the path — rotation-by-rename is NOT tracked)
//   --poll-interval <ms>    follow poll cadence (50..5000, default 200)
//   --buffer-size <bytes>   scan/read buffer (default 64 KiB)
//   --max-line-bytes <n>    retained-line cap (default 1 MiB)
//   --workers <count>       Runtime worker count (default 1)
//   --help                  show usage
//
// Backend: ThreadPoolBackend. Last-N is a bounded BACKWARD scan (memory ~
// buffer + one line carry, independent of file size); follow adds one stat
// per poll interval — no busy spin, idle CPU ~ 0.
//
// Cancellation (follow): SIGINT/SIGTERM are blocked before any thread is
// created and consumed by a dedicated sigwait thread which calls
// TailEngine::request_stop() from a REAL thread context (request_stop takes
// locks — a signal-handler context is not safe for it). The follow task
// observes the stop request within one poll slice, drains its outstanding
// Completion, and exits; main then completes the Runtime lifecycle
// (drain + join) and exits 0 — a signal-ended follow is the documented
// NORMAL end of tail -f. No std::exit() bypass of the Runtime lifecycle.
//
// Exit codes: 0 = success (incl. signal-ended follow), 1 = usage error,
// 2 = I/O error.
#include "cli_parse.hpp"
#include "tail_task.hpp"

#include <sluice/error.hpp>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using sluice_tail::TailEngine;
using sluice_tail::TailOptions;
using sluice_tail::cli::CliArgs;
using sluice_tail::cli::parse_args;

// Blocks SIGINT/SIGTERM in the calling thread (and, because threads inherit
// the mask, in every thread created afterwards — call before spawning).
bool block_signals() {
    sigset_t set;
    ::sigemptyset(&set);
    ::sigaddset(&set, SIGINT);
    ::sigaddset(&set, SIGTERM);
    return ::pthread_sigmask(SIG_BLOCK, &set, nullptr) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    // Block the follow-terminating signals FIRST so the sigwait thread (and
    // no other thread) can consume them deterministically.
    if (!block_signals()) {
        std::fprintf(stderr, "%s: cannot block signals: %s\n", argv[0],
                     std::strerror(errno));
        return 2;
    }

    CliArgs args;
    int rc = parse_args(argc, argv, args);
    if (rc != 0) return rc;
    if (args.help) {
        sluice_tail::cli::usage(argv[0]);
        return 0;
    }

    int fd = ::open(args.file.c_str(), O_RDONLY);
    if (fd < 0) {
        std::fprintf(stderr, "%s: cannot open '%s': %s\n", argv[0],
                     args.file.c_str(), std::strerror(errno));
        return 2;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        std::fprintf(stderr, "%s: cannot stat '%s': %s\n", argv[0],
                     args.file.c_str(), std::strerror(errno));
        ::close(fd);
        return 2;
    }
    if (!S_ISREG(st.st_mode)) {
        std::fprintf(stderr, "%s: %s: not a regular file\n", argv[0],
                     args.file.c_str());
        ::close(fd);
        return 2;
    }

    TailOptions options;
    options.lines = args.lines;
    options.follow = args.follow;
    options.poll_interval_ms = args.poll_interval_ms;
    options.buffer_size = args.buffer_size;
    options.max_line_bytes = args.max_line_bytes;
    options.workers = args.workers;

    TailEngine engine(
        fd, options,
        /*sink=*/[](std::string_view line) {
            std::fwrite(line.data(), 1, line.size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);  // line-visible to pipe consumers
        },
        /*diag=*/[](std::string_view msg) {
            std::fwrite(msg.data(), 1, msg.size(), stderr);
        });

    auto start_r = engine.start();
    if (!start_r.has_value()) {
        std::fprintf(stderr, "%s: cannot start tail engine\n", argv[0]);
        ::close(fd);
        return 2;
    }

    // Follow mode: a dedicated thread consumes the blocked signals and stops
    // the engine from a real thread context. The Runtime lifecycle is then
    // closed by wait() (drain + join) — never bypassed with std::exit.
    std::atomic<bool> signal_seen{false};
    pthread_t sig_thread{};
    bool sig_thread_spawned = false;
    // The trampoline context must outlive the if-block: the waiter thread
    // dereferences it when the signal arrives, which is AFTER the block
    // closes (ASan-confirmed stack-use-after-scope in the first version).
    // It lives until main returns; the thread is joined before that.
    struct SigCtx {
        TailEngine* engine;
        std::atomic<bool>* seen;
    };
    SigCtx sig_ctx{&engine, &signal_seen};
    if (args.follow) {
        auto trampoline = [](void* p) -> void* {
            SigCtx* c = static_cast<SigCtx*>(p);
            sigset_t set;
            ::sigemptyset(&set);
            ::sigaddset(&set, SIGINT);
            ::sigaddset(&set, SIGTERM);
            int sig = 0;
            if (::sigwait(&set, &sig) == 0) {
                c->seen->store(true, std::memory_order_relaxed);
                c->engine->request_stop();
            }
            return nullptr;
        };
        if (::pthread_create(&sig_thread, nullptr, trampoline, &sig_ctx) != 0) {
            std::fprintf(stderr, "%s: cannot spawn signal waiter\n", argv[0]);
            engine.request_stop();
            (void)engine.wait();
            ::close(fd);
            return 2;
        }
        sig_thread_spawned = true;
    }

    auto result = engine.wait();

    if (sig_thread_spawned) {
        // If the task ended WITHOUT a signal (error/EOF-only follow cannot
        // happen: follow only ends by stop or error; an error end leaves the
        // waiter blocked), deliver a no-op signal to wake it for the join.
        if (!signal_seen.load(std::memory_order_relaxed))
            ::pthread_kill(sig_thread, SIGINT);
        ::pthread_join(sig_thread, nullptr);
    }

    ::close(fd);

    if (!result.has_value()) {
        std::fprintf(stderr, "%s: tail failed: %d\n", argv[0],
                     static_cast<int>(result.error().code));
        return 2;
    }
    const auto& r = result.value();
    if (r.error.has_value()) {
        std::fprintf(stderr, "%s: %s: %s%s%s\n", argv[0], args.file.c_str(),
                     r.error->code == sluice::IoError::Code::canceled
                         ? "canceled"
                         : "read error",
                     r.error->os_errno ? " (" : "",
                     r.error->os_errno ? std::strerror(r.error->os_errno) : "");
        return 2;
    }
    // Signal-ended follow (stopped_by_cancel) is success — GNU-tail behavior.
    return 0;
}
