// FileReader / FileWriter POSIX implementation.
#include <cppio/file.hpp>
#include <cppio/detail/posix_retry.hpp>

#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

namespace cppio {

namespace {

// Wrap a raw ssize_t syscall result into Result<size_t>, mapping errno via the
// portable from_errno_value helper. The EINTR retry is handled by the caller
// via detail::retry_on_eintr, so by the time we get here errno is a real error.
Result<std::size_t> syscall_result(ssize_t n) {
    if (n < 0) return make_unexpected<std::size_t>(from_errno_value(errno));
    return static_cast<std::size_t>(n);
}

}  // namespace

// ---------------- FileReader ----------------

FileReader::FileReader(const std::string& path, SyscallStats* stats) : stats_(stats) {
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) {
        // Preserve the real errno so read_some() can report the actual cause
        // (ENOENT, ENOTDIR, EACCES, EMFILE, ...) rather than a synthetic code.
        open_error_ = from_errno_value(errno);
    }
}

void FileReader::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

FileReader::~FileReader() { close(); }

Result<std::size_t> FileReader::read_some(std::span<std::byte> dst) {
    if (fd_ < 0) {
        // Surface the real open() failure if we have it; otherwise this is a
        // default-constructed / moved-from reader.
        if (stats_) ++stats_->read_syscall_errors;
        return make_unexpected<std::size_t>(
            open_error_.value_or(IoError{IoError::Code::permission_denied}));
    }
    if (dst.empty()) return std::size_t{0};
    ssize_t n = detail::retry_on_eintr([&] {
        return ::read(fd_, dst.data(), dst.size());
    });
    auto result = syscall_result(n);
    if (stats_) {
        if (result.has_value()) {
            ++stats_->read_syscalls;
            stats_->read_syscall_bytes += result.value();
        } else {
            ++stats_->read_syscall_errors;
        }
    }
    return result;
}

// ---------------- FileWriter ----------------

FileWriter::FileWriter(const std::string& path, SyscallStats* stats) : stats_(stats) {
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd_ < 0) {
        open_error_ = from_errno_value(errno);
    }
}

void FileWriter::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

FileWriter::~FileWriter() { close(); }

Result<std::size_t> FileWriter::write_some(std::span<const std::byte> src) {
    if (fd_ < 0) {
        if (stats_) ++stats_->write_syscall_errors;
        return make_unexpected<std::size_t>(
            open_error_.value_or(IoError{IoError::Code::permission_denied}));
    }
    if (src.empty()) return std::size_t{0};
    ssize_t n = detail::retry_on_eintr([&] {
        return ::write(fd_, src.data(), src.size());
    });
    auto result = syscall_result(n);
    if (stats_) {
        if (result.has_value()) {
            ++stats_->write_syscalls;
            stats_->write_syscall_bytes += result.value();
        } else {
            ++stats_->write_syscall_errors;
        }
    }
    return result;
}

}  // namespace cppio
