#include <sluice/blocking/file.hpp>
#include <sluice/detail/io_validation.hpp>
#include <sluice/detail/posix_retry.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <span>

namespace sluice::blocking {

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

// Direct ::read/::write: the kernel owns and atomically advances the shared offset.
Result<std::size_t> read(const File& file, std::span<std::byte> dst) {
    if (!file.is_open()) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
    }
    if (file.access() == FileAccess::write_only) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_argument});
    }
    if (dst.empty()) {
        return std::size_t{0};
    }

    ssize_t n = detail::retry_on_eintr([&] {
        return ::read(file.native_handle(), dst.data(), dst.size());
    });
    if (n < 0) {
        return make_unexpected<std::size_t>(from_errno_value(errno));
    }
    return static_cast<std::size_t>(n);
}

Result<std::size_t> write(const File& file, std::span<const std::byte> src) {
    if (!file.is_open()) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
    }
    if (file.access() == FileAccess::read_only) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_argument});
    }
    if (src.empty()) {
        return std::size_t{0};
    }

    ssize_t n = detail::retry_on_eintr([&] {
        return ::write(file.native_handle(), src.data(), src.size());
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

Result<std::uint64_t> size(const File& file) {
    if (!file.is_open()) {
        return make_unexpected<std::uint64_t>(IoError{IoError::Code::invalid_state});
    }

    struct ::stat st {};
    int rc = detail::retry_on_eintr([&] { return ::fstat(file.native_handle(), &st); });
    if (rc < 0) {
        return make_unexpected<std::uint64_t>(from_errno_value(errno));
    }
    return static_cast<std::uint64_t>(st.st_size);
}

Result<void> resize(const File& file, std::uint64_t new_size) {
    if (!file.is_open()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    if (file.access() == FileAccess::read_only) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }

    auto native_size = detail::checked_posix_offset(new_size);
    if (!native_size.has_value()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_argument});
    }

    int rc = detail::retry_on_eintr(
        [&] { return ::ftruncate(file.native_handle(), native_size.value()); });
    if (rc < 0) {
        return make_unexpected<void>(from_errno_value(errno));
    }
    return {};
}

Result<void> sync_all(const File& file) {
    if (!file.is_open()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }

    int rc = detail::retry_on_eintr([&] { return ::fsync(file.native_handle()); });
    if (rc < 0) {
        return make_unexpected<void>(from_errno_value(errno));
    }
    return {};
}

} // namespace sluice::blocking
