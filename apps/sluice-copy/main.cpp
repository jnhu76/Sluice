// sluice-copy — reference async file copy application (M1-A, Version A).
//
// CLI:
//   sluice-copy [options] <source> <destination>
// Options:
//   --buffer-size <bytes>   per-chunk read/write buffer (default 1 MiB)
//   --workers <count>       Runtime worker count (default 1)
//   --sync none|data|all    durability policy after copy (default none)
//   --help                  show usage
//
// Backend: ThreadPoolBackend (real file I/O). No FakeAsyncBackend in app code.
//
// Version A limitation: on mid-copy failure the destination may be left
// partial. Atomic temp-file + rename is a Version C feature.
//
// Exit codes: 0 = success, 1 = usage error, 2 = I/O error, 3 = canceled.
#include "copy_task.hpp"

#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using sluice::IoError;
using sluice_copy::CopyStats;
using sluice_copy::SyncPolicy;

// App-local RAII file descriptor (brief §21: do NOT promote to core).
struct ScopedFd {
    int fd = -1;
    explicit ScopedFd(int f) : fd(f) {}
    ~ScopedFd() { if (fd >= 0) ::close(fd); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
};

struct CliArgs {
    std::string src;
    std::string dst;
    std::size_t buffer_size = 1 << 20;  // 1 MiB default
    unsigned workers = 1;
    SyncPolicy sync = SyncPolicy::none;
    bool help = false;
};

int usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s [options] <source> <destination>\n"
        "  --buffer-size <bytes>   per-chunk buffer (default 1 MiB)\n"
        "  --workers <count>       runtime workers (default 1)\n"
        "  --sync none|data|all    durability after copy (default none)\n"
        "  --help                  show this help\n",
        prog);
    return 1;
}

bool parse_size(const char* s, std::size_t& out) {
    if (!s || !*s) return false;
    errno = 0;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return false;
    if (v == 0) return false;
    out = static_cast<std::size_t>(v);
    return true;
}

bool parse_workers(const char* s, unsigned& out) {
    std::size_t v = 0;
    if (!parse_size(s, v)) return false;
    if (v == 0) return false;
    out = static_cast<unsigned>(v);
    return true;
}

bool parse_sync(const char* s, SyncPolicy& out) {
    if (!s) return false;
    if (std::strcmp(s, "none") == 0) { out = SyncPolicy::none; return true; }
    if (std::strcmp(s, "data") == 0) { out = SyncPolicy::data; return true; }
    if (std::strcmp(s, "all") == 0) { out = SyncPolicy::all; return true; }
    return false;
}

// Returns 0 on success (fills args), or a non-zero exit code on usage error.
int parse_args(int argc, char** argv, CliArgs& args) {
    int positionals = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s: missing value for %s\n", argv[0], opt);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--help") {
            args.help = true;
            return 0;
        } else if (a == "--buffer-size") {
            const char* v = next("--buffer-size");
            if (!v || !parse_size(v, args.buffer_size)) return usage(argv[0]);
        } else if (a == "--workers") {
            const char* v = next("--workers");
            if (!v || !parse_workers(v, args.workers)) return usage(argv[0]);
        } else if (a == "--sync") {
            const char* v = next("--sync");
            if (!v || !parse_sync(v, args.sync)) return usage(argv[0]);
        } else if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
            std::fprintf(stderr, "%s: unknown option %s\n", argv[0], a.c_str());
            return usage(argv[0]);
        } else {
            if (positionals == 0) args.src = a;
            else if (positionals == 1) args.dst = a;
            else { std::fprintf(stderr, "%s: extra operand %s\n", argv[0], a.c_str()); return usage(argv[0]); }
            ++positionals;
        }
    }
    if (positionals != 2) return usage(argv[0]);
    return 0;
}

const char* code_name(IoError::Code c) {
    switch (c) {
    case IoError::Code::eof: return "eof";
    case IoError::Code::canceled: return "canceled";
    case IoError::Code::no_space: return "no_space";
    case IoError::Code::permission_denied: return "permission_denied";
    case IoError::Code::invalid_state: return "invalid_state";
    case IoError::Code::backend_error: return "backend_error";
    default: return "io_error";
    }
}

}  // namespace

int main(int argc, char** argv) {
    CliArgs args;
    int rc = parse_args(argc, argv, args);
    if (rc != 0) return rc;
    if (args.help) {
        usage(argv[0]);
        return 0;
    }

    // Open source read-only.
    int src_fd = ::open(args.src.c_str(), O_RDONLY);
    if (src_fd < 0) {
        std::fprintf(stderr, "%s: cannot open source '%s': %s\n", argv[0],
                     args.src.c_str(), std::strerror(errno));
        return 2;
    }
    ScopedFd src_guard(src_fd);

    // Open destination: write, create (NO O_TRUNC — must not truncate before
    // same-file check). Truncation happens after identity verification.
    int dst_fd = ::open(args.dst.c_str(), O_WRONLY | O_CREAT, 0644);
    if (dst_fd < 0) {
        std::fprintf(stderr, "%s: cannot open destination '%s': %s\n", argv[0],
                     args.dst.c_str(), std::strerror(errno));
        return 2;
    }
    ScopedFd dst_guard(dst_fd);

    // Reject source == destination by filesystem identity, not path string.
    // fstat both fds BEFORE any truncation so the source is preserved if they
    // refer to the same inode (hard link or same pathname).
    struct stat src_stat{}, dst_stat{};
    if (::fstat(src_fd, &src_stat) != 0 || ::fstat(dst_fd, &dst_stat) != 0) {
        std::fprintf(stderr, "%s: cannot stat source or destination: %s\n",
                     argv[0], std::strerror(errno));
        return 2;
    }
    if (src_stat.st_dev == dst_stat.st_dev && src_stat.st_ino == dst_stat.st_ino) {
        std::fprintf(stderr, "%s: source and destination refer to the same file\n",
                     argv[0]);
        return 1;
    }

    // Truncate the destination now that we know it is a different file.
    if (::ftruncate(dst_fd, 0) != 0) {
        std::fprintf(stderr, "%s: cannot truncate destination '%s': %s\n",
                     argv[0], args.dst.c_str(), std::strerror(errno));
        return 2;
    }

    auto result = sluice_copy::run_sequential_copy(src_fd, dst_fd,
                                                   args.buffer_size, args.workers,
                                                   args.sync);
    if (!result.has_value()) {
        IoError e = result.error();
        std::fprintf(stderr, "%s: copy failed: %s%s%s\n", argv[0], code_name(e.code),
                     e.os_errno ? " (" : "", e.os_errno ? std::strerror(e.os_errno) : "");
        // Canceled exit code vs I/O error.
        return (e.code == IoError::Code::canceled) ? 3 : 2;
    }

    CopyStats s = result.value();
    std::printf("%s: copied %llu bytes (read_ops=%llu write_ops=%llu "
                "short_writes=%llu sync=%s)\n",
                argv[0],
                static_cast<unsigned long long>(s.bytes_copied),
                static_cast<unsigned long long>(s.read_ops),
                static_cast<unsigned long long>(s.write_ops),
                static_cast<unsigned long long>(s.short_writes),
                s.sync == SyncPolicy::data ? "data" :
                s.sync == SyncPolicy::all ? "all" : "none");
    return 0;
}
