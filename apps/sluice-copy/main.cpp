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

struct ScopedFd {
    int fd = -1;
    explicit ScopedFd(int f) : fd(f) {}
    ~ScopedFd() {
        if (fd >= 0)
            ::close(fd);
    }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
};

void print_copy_result(const char* prog, const Result<CopyStats>& result) {
    IoError e = result.error();
    std::fprintf(stderr, "%s: copy failed: %s%s%s\n", prog, sluice_copy::cli::code_name(e.code),
                 e.os_errno ? " (" : "", e.os_errno ? std::strerror(e.os_errno) : "");
}

} // namespace

int main(int argc, char** argv) {
    CliArgs args;
    int rc = parse_args(argc, argv, args);
    if (rc != 0)
        return rc;
    if (args.help) {
        sluice_copy::cli::usage(argv[0]);
        return 0;
    }

    if (args.atomic) {
        sluice_copy::SafeOpenOutcome oc = sluice_copy::open_atomic_copy(args.src, args.dst);
        if (oc.failure != sluice_copy::SafeOpenFailure::none) {
            std::fprintf(stderr, "%s: %s", argv[0],
                         sluice_copy::safe_open_failure_message(oc.failure));
            if (oc.error.os_errno != 0) {
                std::fprintf(stderr, ": %s", std::strerror(oc.error.os_errno));
            }
            std::fprintf(stderr, "\n");
            return (oc.failure == sluice_copy::SafeOpenFailure::same_file) ? 1 : 2;
        }
        auto result = sluice_copy::run_pipelined_copy(
            *oc.src_file, sluice::async::NativeFileRef{oc.temp_fd}, args.buffer_size,
            args.pipeline_depth, args.workers, args.sync);
        if (!result.has_value()) {
            sluice_copy::discard_atomic_copy(oc);
            print_copy_result(argv[0], result);
            return (result.error().code == IoError::Code::canceled) ? 3 : 2;
        }

        sluice_copy::SafeCommitStage stage = sluice_copy::SafeCommitStage::none;
        auto commit = sluice_copy::commit_atomic_copy(oc, args.dst, args.sync, &stage);
        if (!commit.has_value()) {
            IoError e = commit.error();

            const char* dst_state = (stage == sluice_copy::SafeCommitStage::dir_sync)
                                        ? "destination already replaced; rename durability NOT "
                                          "guaranteed"
                                        : "destination untouched";
            std::fprintf(stderr, "%s: atomic commit failed at %s (%s): %s%s%s\n", argv[0],
                         stage == sluice_copy::SafeCommitStage::close    ? "close"
                         : stage == sluice_copy::SafeCommitStage::rename ? "rename"
                                                                         : "directory fsync",
                         dst_state, sluice_copy::cli::code_name(e.code), e.os_errno ? " (" : "",
                         e.os_errno ? std::strerror(e.os_errno) : "");
            return 2;
        }

        CopyStats s = result.value();
        std::printf("%s: copied %llu bytes (read_ops=%llu write_ops=%llu "
                    "short_writes=%llu sync=%s pipeline_depth=%llu atomic=on)\n",
                    argv[0], static_cast<unsigned long long>(s.bytes_copied),
                    static_cast<unsigned long long>(s.read_ops),
                    static_cast<unsigned long long>(s.write_ops),
                    static_cast<unsigned long long>(s.short_writes),
                    s.sync == SyncPolicy::data  ? "data"
                    : s.sync == SyncPolicy::all ? "all"
                                                : "none",
                    static_cast<unsigned long long>(args.pipeline_depth));
        return 0;
    }

    sluice_copy::OpenCopyOutcome oc = sluice_copy::open_copy_files(args.src, args.dst);
    if (oc.failure != sluice_copy::OpenCopyFailure::none) {
        std::fprintf(stderr, "%s: %s", argv[0], sluice_copy::open_copy_failure_message(oc.failure));
        if (oc.error.os_errno != 0) {
            std::fprintf(stderr, ": %s", std::strerror(oc.error.os_errno));
        }
        std::fprintf(stderr, "\n");
        return (oc.failure == sluice_copy::OpenCopyFailure::same_file) ? 1 : 2;
    }
    ScopedFd dst_guard(oc.dst_fd);

    if (::ftruncate(oc.dst_fd, 0) != 0) {
        std::fprintf(stderr, "%s: cannot truncate destination '%s': %s\n", argv[0],
                     args.dst.c_str(), std::strerror(errno));
        return 2;
    }

    auto result = sluice_copy::run_pipelined_copy(
        *oc.src_file, sluice::async::NativeFileRef{oc.dst_fd}, args.buffer_size,
        args.pipeline_depth, args.workers, args.sync);
    if (!result.has_value()) {
        print_copy_result(argv[0], result);

        return (result.error().code == IoError::Code::canceled) ? 3 : 2;
    }

    CopyStats s = result.value();
    std::printf("%s: copied %llu bytes (read_ops=%llu write_ops=%llu "
                "short_writes=%llu sync=%s pipeline_depth=%llu atomic=off)\n",
                argv[0], static_cast<unsigned long long>(s.bytes_copied),
                static_cast<unsigned long long>(s.read_ops),
                static_cast<unsigned long long>(s.write_ops),
                static_cast<unsigned long long>(s.short_writes),
                s.sync == SyncPolicy::data  ? "data"
                : s.sync == SyncPolicy::all ? "all"
                                            : "none",
                static_cast<unsigned long long>(args.pipeline_depth));
    return 0;
}
