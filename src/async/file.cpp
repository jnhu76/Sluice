#include <sluice/async/file.hpp>

#include <sluice/async/await_op_helpers.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <utility>

namespace sluice::async {

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

} // namespace

Result<File> File::open(const std::string& path, FileOpen mode) {
    if (mode.access == FileAccess::read_only &&
        mode.contents == FileInitialContents::truncate) {
        return make_unexpected<File>(IoError{IoError::Code::invalid_argument});
    }

    const int fd = ::open(path.c_str(), open_flags_for(mode), 0644);
    if (fd < 0) {
        return make_unexpected<File>(from_errno_value(errno));
    }
    return File{fd};
}

File::File(File&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        (void)close();
        fd_ = std::exchange(other.fd_, -1);
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

Result<std::size_t> read_at(const File& file, RuntimeTaskContext& ctx, std::uint64_t offset,
                            std::span<std::byte> dst, Completion<std::size_t>& c) {
    if (!file.is_open()) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
    }
    if (dst.empty()) {
        return std::size_t{0};
    }
    return await_read_once(ctx, file.native_handle(), dst, offset, c);
}

} // namespace sluice::async
