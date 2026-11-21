#include "file_domain.hpp"

#include "fd_guard.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace sluice_copy {

namespace {

using sluice::IoError;

OpenCopyFailure map_source_failure(detail::SourceFailure f) {
    switch (f) {
    case detail::SourceFailure::open:
        return OpenCopyFailure::src_open;
    case detail::SourceFailure::stat:
        return OpenCopyFailure::src_stat;
    case detail::SourceFailure::not_regular:
        return OpenCopyFailure::src_not_regular;
    case detail::SourceFailure::none:
        break;
    }
    return OpenCopyFailure::none;
}

OpenCopyOutcome fail(OpenCopyFailure f, IoError e) {
    OpenCopyOutcome o;
    o.failure = f;
    o.error = e;
    return o;
}

}

OpenCopyOutcome open_copy_files(const std::string& src_path, const std::string& dst_path) {
    auto src = detail::open_regular_source(src_path);
    if (src.failure != detail::SourceFailure::none) {
        return fail(map_source_failure(src.failure), src.error);
    }
    sluice::File src_file = std::move(*src.file);
    const struct stat& src_stat = src.stat;

    int dst_fd = ::open(dst_path.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (dst_fd < 0) {
        return fail(OpenCopyFailure::dst_open, sluice::from_errno_value(errno));
    }
    detail::ScopedFd dst_guard(dst_fd);

    struct stat dst_stat{};
    if (::fstat(dst_fd, &dst_stat) != 0) {
        return fail(OpenCopyFailure::dst_stat, sluice::from_errno_value(errno));
    }
    if (!S_ISREG(dst_stat.st_mode)) {
        return fail(OpenCopyFailure::dst_not_regular, IoError{IoError::Code::invalid_state});
    }

    if (src_stat.st_dev == dst_stat.st_dev && src_stat.st_ino == dst_stat.st_ino) {
        return fail(OpenCopyFailure::same_file, IoError{IoError::Code::invalid_state});
    }

    OpenCopyOutcome o;
    o.failure = OpenCopyFailure::none;
    o.src_file = std::move(src_file);
    o.dst_fd = dst_guard.fd;
    dst_guard.fd = -1;
    return o;
}

const char* open_copy_failure_message(OpenCopyFailure f) {
    switch (f) {
    case OpenCopyFailure::none:
        return "ok";
    case OpenCopyFailure::src_open:
        return "cannot open source";
    case OpenCopyFailure::src_stat:
        return "cannot stat source";
    case OpenCopyFailure::src_not_regular:
        return "source is not a regular file";
    case OpenCopyFailure::dst_open:
        return "cannot open destination";
    case OpenCopyFailure::dst_stat:
        return "cannot stat destination";
    case OpenCopyFailure::dst_not_regular:
        return "destination is not a regular file";
    case OpenCopyFailure::same_file:
        return "source and destination refer to the same file";
    }
    return "unknown error";
}

}
