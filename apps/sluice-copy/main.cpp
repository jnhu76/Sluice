// sluice-copy — reference async file copy application.
//
// CLI:
//   sluice-copy [options] <source> <destination>
// Options:
//   --buffer-size <bytes>   per-chunk read/write buffer (default 1 MiB)
//   --pipeline-depth <n>    read-ahead slots (default 1 = Version A sequential;
//                           >1 enables the bounded reusable-buffer pipeline,
//                           Version B, with up to n outstanding reads and an
//                           ordered single writer)
//   --workers <count>       Runtime worker count (default 1)
//   --sync none|data|all    durability policy after copy (default none)
//   --help                  show usage
//
// Backend: ThreadPoolBackend (real file I/O). No FakeAsyncBackend in app code.
//
// Memory upper bound is approximately buffer_size * pipeline_depth, capped by
// the app-level limits in copy_task.hpp (kMaxBufferSize/kMaxPipelineDepth/
// kMaxPipelineBytes/kMaxWorkers). The outer call is still blocking (the copy
// completes before the CLI returns); the pipeline is internal. Not zero-copy;
// writes are not parallel.
//
// Input domain: both source and destination must be REGULAR files (the
// pipeline requires a seekable, finite-length source and a truncatable,
// positional destination). The source is validated before the destination is
// created or touched; on a mid-copy failure the destination may be left
// partial. Atomic temp-file + rename is a Version C feature.
//
// Exit codes: 0 = success, 1 = usage error, 2 = I/O error, 3 = canceled.
#include "cli_parse.hpp"
#include "copy_task.hpp"
#include "file_domain.hpp"

#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {

using sluice::IoError;
using sluice_copy::CopyStats;
using sluice_copy::SyncPolicy;
using sluice_copy::cli::CliArgs;
using sluice_copy::cli::parse_args;

// App-local RAII file descriptor (brief §21: do NOT promote to core).
struct ScopedFd {
    int fd = -1;
    explicit ScopedFd(int f) : fd(f) {}
    ~ScopedFd() { if (fd >= 0) ::close(fd); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
};

}  // namespace

int main(int argc, char** argv) {
    CliArgs args;
    int rc = parse_args(argc, argv, args);
    if (rc != 0) return rc;
    if (args.help) {
        sluice_copy::cli::usage(argv[0]);
        return 0;
    }

    // Open + validate the input domain (see file_domain.hpp). The source's
    // regular-file check runs BEFORE the destination is created, so a
    // non-regular source (FIFO, char device, directory) never creates a new
    // destination and never truncates an existing one.
    sluice_copy::OpenCopyOutcome oc =
        sluice_copy::open_copy_files(args.src, args.dst);
    if (oc.failure != sluice_copy::OpenCopyFailure::none) {
        std::fprintf(stderr, "%s: %s", argv[0],
                     sluice_copy::open_copy_failure_message(oc.failure));
        if (oc.error.os_errno != 0) {
            std::fprintf(stderr, ": %s", std::strerror(oc.error.os_errno));
        }
        std::fprintf(stderr, "\n");
        return (oc.failure == sluice_copy::OpenCopyFailure::same_file) ? 1 : 2;
    }
    ScopedFd src_guard(oc.src_fd);
    ScopedFd dst_guard(oc.dst_fd);

    // Truncate the destination now that we know it is a different, regular
    // file. On a mid-copy failure the destination may be left partial (this
    // app does not use temp-file + rename; that is Version C).
    if (::ftruncate(oc.dst_fd, 0) != 0) {
        std::fprintf(stderr, "%s: cannot truncate destination '%s': %s\n",
                     argv[0], args.dst.c_str(), std::strerror(errno));
        return 2;
    }

    auto result = sluice_copy::run_pipelined_copy(oc.src_fd, oc.dst_fd,
                                                   args.buffer_size,
                                                   args.pipeline_depth,
                                                   args.workers, args.sync);
    if (!result.has_value()) {
        IoError e = result.error();
        std::fprintf(stderr, "%s: copy failed: %s%s%s\n", argv[0],
                     sluice_copy::cli::code_name(e.code),
                     e.os_errno ? " (" : "", e.os_errno ? std::strerror(e.os_errno) : "");
        // Canceled exit code vs I/O error.
        return (e.code == IoError::Code::canceled) ? 3 : 2;
    }

    CopyStats s = result.value();
    std::printf("%s: copied %llu bytes (read_ops=%llu write_ops=%llu "
                "short_writes=%llu sync=%s pipeline_depth=%llu)\n",
                argv[0],
                static_cast<unsigned long long>(s.bytes_copied),
                static_cast<unsigned long long>(s.read_ops),
                static_cast<unsigned long long>(s.write_ops),
                static_cast<unsigned long long>(s.short_writes),
                s.sync == SyncPolicy::data ? "data" :
                s.sync == SyncPolicy::all ? "all" : "none",
                static_cast<unsigned long long>(args.pipeline_depth));
    return 0;
}
