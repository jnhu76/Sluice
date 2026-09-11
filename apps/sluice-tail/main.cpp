#include "cli_parse.hpp"
#include "tail_task.hpp"

#include <sluice/error.hpp>
#include <sluice/file_resource.hpp>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sys/stat.h>

namespace {

using sluice_tail::TailEngine;
using sluice_tail::TailOptions;
using sluice_tail::cli::CliArgs;
using sluice_tail::cli::parse_args;

bool block_signals() {
    sigset_t set;
    ::sigemptyset(&set);
    ::sigaddset(&set, SIGINT);
    ::sigaddset(&set, SIGTERM);
    return ::pthread_sigmask(SIG_BLOCK, &set, nullptr) == 0;
}

} // namespace

int main(int argc, char** argv) {
    if (!block_signals()) {
        std::fprintf(stderr, "%s: cannot block signals: %s\n", argv[0], std::strerror(errno));
        return 2;
    }

    CliArgs args;
    int rc = parse_args(argc, argv, args);
    if (rc != 0)
        return rc;
    if (args.help) {
        sluice_tail::cli::usage(argv[0]);
        return 0;
    }

    auto opened = sluice::File::open(args.file);
    if (!opened.has_value()) {
        std::fprintf(stderr, "%s: cannot open '%s': %s\n", argv[0], args.file.c_str(),
                     std::strerror(opened.error().os_errno));
        return 2;
    }
    struct stat st{};
    if (::fstat(opened.value().native_handle(), &st) != 0) {
        std::fprintf(stderr, "%s: cannot stat '%s': %s\n", argv[0], args.file.c_str(),
                     std::strerror(errno));
        return 2;
    }
    if (!S_ISREG(st.st_mode)) {
        std::fprintf(stderr, "%s: %s: not a regular file\n", argv[0], args.file.c_str());
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
        std::move(opened).value(), options,
        [](std::string_view line) {
            std::fwrite(line.data(), 1, line.size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        },
        [](std::string_view msg) { std::fwrite(msg.data(), 1, msg.size(), stderr); });

    auto start_r = engine.start();
    if (!start_r.has_value()) {
        std::fprintf(stderr, "%s: cannot start tail engine\n", argv[0]);
        return 2;
    }

    std::atomic<bool> signal_seen{false};
    pthread_t sig_thread{};
    bool sig_thread_spawned = false;

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
            return 2;
        }
        sig_thread_spawned = true;
    }

    auto result = engine.wait();

    if (sig_thread_spawned) {
        if (!signal_seen.load(std::memory_order_relaxed))
            ::pthread_kill(sig_thread, SIGINT);
        ::pthread_join(sig_thread, nullptr);
    }

    if (!result.has_value()) {
        std::fprintf(stderr, "%s: tail failed: %d\n", argv[0],
                     static_cast<int>(result.error().code));
        return 2;
    }
    const auto& r = result.value();
    if (r.error.has_value()) {
        std::fprintf(stderr, "%s: %s: %s%s%s\n", argv[0], args.file.c_str(),
                     r.error->code == sluice::IoError::Code::canceled ? "canceled" : "read error",
                     r.error->os_errno ? " (" : "",
                     r.error->os_errno ? std::strerror(r.error->os_errno) : "");
        return 2;
    }

    return 0;
}
