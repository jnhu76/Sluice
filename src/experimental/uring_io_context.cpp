// UringIoContext implementation (CPPIO-CORE-013D). Owns POSIX open/close around
// a UringWriteBatch::write_all. Standalone — not a sluice::IoContext subclass.
#include <sluice/experimental/uring_io_context.hpp>

#include <sluice/error.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <string>

namespace sluice::experimental {

UringIoContext::UringIoContext(unsigned queue_depth) : batch_(queue_depth) {}

Result<UringWriteResult> UringIoContext::write_file_all(std::string_view path,
                                                        std::span<const std::byte> bytes) {
    // POSIX open with O_CLOEXEC; the fd is closed via RAII below.
    int fd = ::open(std::string(path).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return make_unexpected<UringWriteResult>(from_errno_value(errno));
    }
    auto result = batch_.write_all(fd, bytes, /*file_offset=*/0);
    ::close(fd); // RAII-equivalent: always close, even on error.
    return result;
}

} // namespace sluice::experimental
