#include <sluice/experimental/uring_io_context.hpp>

#include <sluice/error.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <string>

namespace sluice::experimental {

UringIoContext::UringIoContext(unsigned queue_depth) : batch_(queue_depth) {}

Result<UringWriteResult> UringIoContext::write_file_all(std::string_view path,
                                                        std::span<const std::byte> bytes) {
    int fd = ::open(std::string(path).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return make_unexpected<UringWriteResult>(from_errno_value(errno));
    }
    auto result = batch_.write_all(fd, bytes, 0);
    const int close_result = ::close(fd);
    const int close_errno = errno;

    if (result.has_value() && close_result < 0) {
        return make_unexpected<UringWriteResult>(from_errno_value(close_errno));
    }
    return result;
}

} // namespace sluice::experimental
