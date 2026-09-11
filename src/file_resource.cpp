#include <sluice/file_resource.hpp>
#include <sluice/detail/io_validation.hpp>
#include <sluice/detail/posix_retry.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <span>
#include <utility>

namespace sluice {

namespace {

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
    if (mode.access == FileAccess::read_only &&
        mode.contents == FileInitialContents::truncate) {
        return make_unexpected<File>(IoError{IoError::Code::invalid_argument});
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

namespace blocking {

Result<std::size_t> read_at(const File& file, std::uint64_t offset,
                            std::span<std::byte> dst) {
    if (!file.is_open()) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
    }
    if (dst.empty()) {
        return std::size_t{0};
    }

    auto native_offset = detail::checked_posix_offset(offset);
    if (!native_offset.has_value()) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_argument});
    }

    ssize_t n = detail::retry_on_eintr([&] {
        return ::pread(file.native_handle(), dst.data(), dst.size(), native_offset.value());
    });
    if (n < 0) {
        return make_unexpected<std::size_t>(from_errno_value(errno));
    }
    return static_cast<std::size_t>(n);
}

Result<std::size_t> write_at(const File& file, std::uint64_t offset,
                             std::span<const std::byte> src) {
    if (!file.is_open()) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
    }
    if (file.access() == FileAccess::read_only) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_argument});
    }
    if (src.empty()) {
        return std::size_t{0};
    }

    auto native_offset = detail::checked_posix_offset(offset);
    if (!native_offset.has_value()) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_argument});
    }

    ssize_t n = detail::retry_on_eintr([&] {
        return ::pwrite(file.native_handle(), src.data(), src.size(), native_offset.value());
    });
    if (n < 0) {
        return make_unexpected<std::size_t>(from_errno_value(errno));
    }
    return static_cast<std::size_t>(n);
}

Result<void> sync_data(const File& file) {
    if (!file.is_open()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }

    int rc = detail::retry_on_eintr([&] { return ::fdatasync(file.native_handle()); });
    if (rc < 0) {
        return make_unexpected<void>(from_errno_value(errno));
    }
    return {};
}

} // namespace blocking

}
