// sluice-copy file open + input-domain validation implementation.
#include "file_domain.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sluice_copy {

namespace {

using sluice::IoError;

// App-local RAII file descriptor (brief §21: do NOT promote to core).
struct ScopedFd {
    int fd = -1;
    explicit ScopedFd(int f) : fd(f) {}
    ~ScopedFd() { if (fd >= 0) ::close(fd); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
};

OpenCopyOutcome fail(OpenCopyFailure f, IoError e) {
    OpenCopyOutcome o;
    o.failure = f;
    o.error = e;
    return o;
}

}  // namespace

OpenCopyOutcome open_copy_files(const std::string& src_path,
                                const std::string& dst_path) {
    // 1. Open the source read-only and fstat it IMMEDIATELY. The Version B
    //    pipeline needs a seekable, finite-length source that reaches EOF;
    //    FIFOs/sockets/char devices do not fit that domain. The regular-file
    //    check happens BEFORE the destination is created or opened, so a
    //    rejected source leaves the destination completely untouched.
    int src_fd = ::open(src_path.c_str(), O_RDONLY);
    if (src_fd < 0) {
        return fail(OpenCopyFailure::src_open,
                    sluice::from_errno_value(errno));
    }
    ScopedFd src_guard(src_fd);

    struct stat src_stat{};
    if (::fstat(src_fd, &src_stat) != 0) {
        return fail(OpenCopyFailure::src_stat,
                    sluice::from_errno_value(errno));
    }
    if (!S_ISREG(src_stat.st_mode)) {
        return fail(OpenCopyFailure::src_not_regular,
                    IoError{IoError::Code::invalid_state});
    }

    // 2. Open the destination write/create (NO O_TRUNC — truncation happens
    //    only after the same-file identity check below).
    int dst_fd = ::open(dst_path.c_str(), O_WRONLY | O_CREAT, 0644);
    if (dst_fd < 0) {
        return fail(OpenCopyFailure::dst_open,
                    sluice::from_errno_value(errno));
    }
    ScopedFd dst_guard(dst_fd);

    struct stat dst_stat{};
    if (::fstat(dst_fd, &dst_stat) != 0) {
        return fail(OpenCopyFailure::dst_stat,
                    sluice::from_errno_value(errno));
    }
    if (!S_ISREG(dst_stat.st_mode)) {
        return fail(OpenCopyFailure::dst_not_regular,
                    IoError{IoError::Code::invalid_state});
    }

    // 3. Reject source == destination by filesystem identity (device + inode),
    //    not path-string equality — this covers hard links and bind mounts.
    if (src_stat.st_dev == dst_stat.st_dev &&
        src_stat.st_ino == dst_stat.st_ino) {
        return fail(OpenCopyFailure::same_file,
                    IoError{IoError::Code::invalid_state});
    }

    // Success: hand the descriptors to the caller (guards release).
    OpenCopyOutcome o;
    o.failure = OpenCopyFailure::none;
    o.src_fd = src_guard.fd;
    o.dst_fd = dst_guard.fd;
    src_guard.fd = -1;
    dst_guard.fd = -1;
    return o;
}

const char* open_copy_failure_message(OpenCopyFailure f) {
    switch (f) {
    case OpenCopyFailure::none: return "ok";
    case OpenCopyFailure::src_open: return "cannot open source";
    case OpenCopyFailure::src_stat: return "cannot stat source";
    case OpenCopyFailure::src_not_regular: return "source is not a regular file";
    case OpenCopyFailure::dst_open: return "cannot open destination";
    case OpenCopyFailure::dst_stat: return "cannot stat destination";
    case OpenCopyFailure::dst_not_regular:
        return "destination is not a regular file";
    case OpenCopyFailure::same_file:
        return "source and destination refer to the same file";
    }
    return "unknown error";
}

}  // namespace sluice_copy
