#include <sluice/file_resource.hpp>

#include <sluice/detail/file_semantics.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <optional>
#include <utility>

namespace sluice {

namespace {

// POSIX spelling of the oracle's open decision (SEM-02). The frozen platform
// defaults live here: close-on-exec descriptors and creation mode 0644 filtered
// by the process umask.
int open_flags_for(FileOpen mode) noexcept {
    int flags = O_CLOEXEC;
    switch (mode.access) {
    case FileAccess::read_only:
        flags |= O_RDONLY;
        break;
    case FileAccess::write_only:
        flags |= O_WRONLY;
        break;
    case FileAccess::read_write:
        flags |= O_RDWR;
        break;
    }
    switch (mode.existence) {
    case FileExistence::open_existing:
        break;
    case FileExistence::create_if_missing:
        flags |= O_CREAT;
        break;
    case FileExistence::create_new:
        flags |= O_CREAT | O_EXCL;
        break;
    }
    if (mode.contents == FileInitialContents::truncate) {
        flags |= O_TRUNC;
    }
    return flags;
}

}

Result<File> File::open(const std::string& path, FileOpen mode) {
    if (auto rejection =
            detail::open_rejection_of(detail::precheck_open(mode, path));
        rejection.has_value()) {
        return make_unexpected<File>(*rejection);
    }

    const int fd = ::open(path.c_str(), open_flags_for(mode), 0644);
    if (fd < 0) {
        return make_unexpected<File>(from_errno_value(errno));
    }
    return File{fd, mode.access};
}

File::File(File&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), access_(other.access_) {}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        (void)close();
        fd_ = std::exchange(other.fd_, -1);
        access_ = other.access_;
    }
    return *this;
}

File::~File() {
    (void)close();
}

Result<void> File::close() noexcept {
    if (fd_ < 0) {
        return {};
    }
    const int fd = std::exchange(fd_, -1);
    if (::close(fd) != 0) {
        return make_unexpected<void>(from_errno_value(errno));
    }
    return {};
}

}
