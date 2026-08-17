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
//   --no-atomic             direct destination write (old Version A/B
//                           behavior); default is the Version C safe path
//   --help                  show usage
//
// Backend: ThreadPoolBackend (real file I/O). No FakeAsyncBackend in app code.
//
// Version C (default): the copy lands in a uniquely-named temp file created in
// the DESTINATION's directory (see safe_output.hpp), the sync policy applies
// to that temp fd, and the destination is replaced by one atomic rename. A
// copy/sync/rename failure never leaves partial content visible at the
// destination and never destroys an existing destination; the temp file is
// cleaned up on every failure path. With --sync data|all the parent directory
// is fsynced after the rename so the replacement itself is crash-durable.
// With --no-atomic the old direct-write behavior applies (a mid-copy failure
// may leave a partial destination).
//
// Memory upper bound is approximately buffer_size * pipeline_depth, capped by
// the app-level limits in copy_task.hpp (kMaxBufferSize/kMaxPipelineDepth/
// kMaxPipelineBytes/kMaxWorkers). The outer call is still blocking (the copy
// completes before the CLI returns); the pipeline is internal. Not zero-copy;
// writes are not parallel.
//
// Input domain: the source must be a REGULAR file (the pipeline requires a
// seekable, finite-length source); the destination must be a regular file or
// not exist (atomic mode creates it via rename). Source == destination and
// hardlink aliases are rejected by inode identity.
//
// Exit codes: 0 = success, 1 = usage error, 2 = I/O error, 3 = canceled.
#include "cli_parse.hpp"
#include "copy_task.hpp"
#include "file_domain.hpp"
#include "safe_output.hpp"

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
using sluice::Result;
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

void print_copy_result(const char* prog, const Result<CopyStats>& result,
                       const CliArgs& args) {
    IoError e = result.error();
    std::fprintf(stderr, "%s: copy failed: %s%s%s\n", prog,
                 sluice_copy::cli::code_name(e.code),
                 e.os_errno ? " (" : "", e.os_errno ? std::strerror(e.os_errno) : "");
}

}  // namespace

int main(int argc, char** argv) {
    CliArgs args;
    int rc = parse_args(argc, argv, args);
    if (rc != 0) return rc;
    if (args.help) {
        sluice_copy::cli::usage(argv[0]);
        return 0;
    }

    if (args.atomic) {
        // ---- Version C: temp file in the destination directory + rename. --
        // open_atomic_copy validates the input domain (regular source,
        // non-same-inode destination) BEFORE creating anything; the
        // destination is never opened for writing and never truncated.
        sluice_copy::SafeOpenOutcome oc =
            sluice_copy::open_atomic_copy(args.src, args.dst);
        if (oc.failure != sluice_copy::SafeOpenFailure::none) {
            std::fprintf(stderr, "%s: %s", argv[0],
                         sluice_copy::safe_open_failure_message(oc.failure));
            if (oc.error.os_errno != 0) {
                std::fprintf(stderr, ": %s", std::strerror(oc.error.os_errno));
            }
            std::fprintf(stderr, "\n");
            return (oc.failure == sluice_copy::SafeOpenFailure::same_file) ? 1 : 2;
        }
        ScopedFd src_guard(oc.src_fd);

        // Copy into the temp fd. temp_fd ownership stays with the outcome:
        // commit/discard close it on every path.
        auto result = sluice_copy::run_pipelined_copy(
            oc.src_fd, oc.temp_fd, args.buffer_size, args.pipeline_depth,
            args.workers, args.sync);
        if (!result.has_value()) {
            sluice_copy::discard_atomic_copy(oc);
            print_copy_result(argv[0], result, args);
            return (result.error().code == IoError::Code::canceled) ? 3 : 2;
        }

        // Sync already applied to the temp fd by the copy task; now close +
        // rename + (per policy) fsync the parent directory. The destination
        // is either fully replaced or fully untouched.
        sluice_copy::SafeCommitStage stage = sluice_copy::SafeCommitStage::none;
        auto commit = sluice_copy::commit_atomic_copy(oc, args.dst, args.sync,
                                                      &stage);
        if (!commit.has_value()) {
            IoError e = commit.error();
            // Truthful per-stage message: only a directory-fsync failure
            // occurs AFTER the rename replaced the destination.
            const char* dst_state =
                (stage == sluice_copy::SafeCommitStage::dir_sync)
                    ? "destination already replaced; rename durability NOT "
                      "guaranteed"
                    : "destination untouched";
            std::fprintf(stderr, "%s: atomic commit failed at %s (%s): %s%s%s\n",
                         argv[0],
                         stage == sluice_copy::SafeCommitStage::close ? "close"
                         : stage == sluice_copy::SafeCommitStage::rename
                             ? "rename"
                             : "directory fsync",
                         dst_state, sluice_copy::cli::code_name(e.code),
                         e.os_errno ? " (" : "",
                         e.os_errno ? std::strerror(e.os_errno) : "");
            return 2;
        }

        CopyStats s = result.value();
        std::printf("%s: copied %llu bytes (read_ops=%llu write_ops=%llu "
                    "short_writes=%llu sync=%s pipeline_depth=%llu atomic=on)\n",
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

    // ---- --no-atomic: the original direct-write path (Version A/B). -------
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
    // path does not use temp-file + rename).
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
        print_copy_result(argv[0], result, args);
        // Canceled exit code vs I/O error.
        return (result.error().code == IoError::Code::canceled) ? 3 : 2;
    }

    CopyStats s = result.value();
    std::printf("%s: copied %llu bytes (read_ops=%llu write_ops=%llu "
                "short_writes=%llu sync=%s pipeline_depth=%llu atomic=off)\n",
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
