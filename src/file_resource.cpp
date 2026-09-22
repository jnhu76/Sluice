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

// Test-only: the close seam sits exactly at the native-call boundary, so every
// path that closes a canonical File (explicit close, destructor, move
// assignment) is driven by the same script. The production build calls the
// native close directly.
int close_native(int fd) noexcept {
#ifdef SLUICE_FILE_INTERNAL_TESTING
    if (file_testing::NativeScript* script = file_testing::NativeScript::active();
        script != nullptr && script->intercepts(file_testing::NativeCall::close, fd)) {
        return static_cast<int>(script->next(file_testing::NativeCall::close, fd));
    }
#endif
    return ::close(fd);
}

// POSIX spelling of the oracle's open decision. The frozen platform defaults
// live here: close-on-exec descriptors and creation mode 0644 filtered by the
// process umask.
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
    // Ownership is consumed before the attempt: the first native close is
    // terminal for this File, so a failed close leaves no owning File behind
    // and the descriptor is never closed a second time (an EINTR retry is
    // unsafe on Linux).
    const int fd = std::exchange(fd_, -1);
    if (close_native(fd) != 0) {
        return make_unexpected<void>(from_errno_value(errno));
    }
    return {};
}

}
