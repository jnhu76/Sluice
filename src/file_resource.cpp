#include <sluice/file_resource.hpp>

#include <sluice/detail/file_semantics.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <optional>
#include <utility>

#ifdef SLUICE_FILE_INTERNAL_TESTING
#include "file_test_seams.hpp"
#endif

namespace sluice {

namespace {

int close_native(int fd) noexcept {
#ifdef SLUICE_FILE_INTERNAL_TESTING
    if (file_testing::NativeScript* script = file_testing::NativeScript::active();
        script != nullptr && script->intercepts(file_testing::NativeCall::close, fd)) {
        return static_cast<int>(script->next(file_testing::NativeCall::close, fd));
    }
#endif
    return ::close(fd);
}

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
    // An EINTR retry of close is unsafe on Linux, so the first attempt is
    // terminal.
    const int fd = std::exchange(fd_, -1);
    if (close_native(fd) != 0) {
        return make_unexpected<void>(from_errno_value(errno));
    }
    return {};
}

}
