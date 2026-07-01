// FileReader / FileWriter POSIX implementation.
#include <cppio/file.hpp>

#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

namespace cppio {

namespace {

Result<std::size_t> read_to_result(ssize_t n) {
    if (n < 0) return make_unexpected<std::size_t>(from_errno_value(errno));
    return static_cast<std::size_t>(n);
}

Result<std::size_t> write_to_result(ssize_t n) {
    if (n < 0) return make_unexpected<std::size_t>(from_errno_value(errno));
    return static_cast<std::size_t>(n);
}

}  // namespace

// ---------------- FileReader ----------------

FileReader::FileReader(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    // open failure is recorded (fd_ < 0); surfaced on first read.
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
        // open() failed or the reader was moved-from. Permission denied is the
        // closest available code for an unreadable path.
        return make_unexpected<std::size_t>(
            IoError{IoError::Code::permission_denied, ENOENT});
    }
    if (dst.empty()) return std::size_t{0};
    ssize_t n = ::read(fd_, dst.data(), dst.size());
    return read_to_result(n);
}

// ---------------- FileWriter ----------------

FileWriter::FileWriter(const std::string& path) {
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
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
        return make_unexpected<std::size_t>(
            IoError{IoError::Code::permission_denied, ENOENT});
    }
    if (src.empty()) return std::size_t{0};
    ssize_t n = ::write(fd_, src.data(), src.size());
    return write_to_result(n);
}

}  // namespace cppio
