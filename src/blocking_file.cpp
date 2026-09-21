#include <sluice/blocking/file.hpp>
#include <sluice/detail/file_semantics.hpp>
#include <sluice/detail/posix_retry.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <optional>
#include <span>

namespace sluice::blocking {

namespace {

using detail::DataOpVerdict;
using detail::FileOperation;

// Precedence steps 1-4 are the shared oracle's decision (SEM-03); this adapter
// only turns the verdict into its own return shape. `execute` and
// `complete_empty` are the two non-rejection verdicts.
struct Precheck {
    std::optional<IoError> rejection;
    bool complete_empty = false;
};

Precheck precheck(const File& file, FileOperation operation, std::uint64_t offset,
                  std::size_t length, bool buffer_present) {
    const DataOpVerdict verdict = detail::precheck_data_op(detail::DataOpRequest{
        !file.is_open(), file.access(), operation, offset, length, buffer_present});
    return Precheck{detail::rejection_of(verdict), verdict == DataOpVerdict::complete_empty};
}

} // namespace

Result<std::size_t> read_at(const File& file, std::uint64_t offset,
                            std::span<std::byte> dst) {
    const Precheck pre = precheck(file, FileOperation::read, offset, dst.size(), dst.data());
    if (pre.rejection.has_value()) {
        return make_unexpected<std::size_t>(*pre.rejection);
    }
    if (pre.complete_empty) {
        return std::size_t{0};
    }

    const auto native_offset = detail::checked_posix_offset(offset);
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
    const Precheck pre = precheck(file, FileOperation::write, offset, src.size(), src.data());
    if (pre.rejection.has_value()) {
        return make_unexpected<std::size_t>(*pre.rejection);
    }
    if (pre.complete_empty) {
        return std::size_t{0};
    }

    const auto native_offset = detail::checked_posix_offset(offset);
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
    const Precheck pre = precheck(file, FileOperation::read, 0, dst.size(), dst.data());
    if (pre.rejection.has_value()) {
        return make_unexpected<std::size_t>(*pre.rejection);
    }
    if (pre.complete_empty) {
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
    const Precheck pre = precheck(file, FileOperation::write, 0, src.size(), src.data());
    if (pre.rejection.has_value()) {
        return make_unexpected<std::size_t>(*pre.rejection);
    }
    if (pre.complete_empty) {
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
    if (auto rejection =
            detail::rejection_of(detail::precheck_state_op(!file.is_open(), file.access(),
                                                           FileOperation::sync_data));
        rejection.has_value()) {
        return make_unexpected<void>(*rejection);
    }

    int rc = detail::retry_on_eintr([&] { return ::fdatasync(file.native_handle()); });
    if (rc < 0) {
        return make_unexpected<void>(from_errno_value(errno));
    }
    return {};
}

Result<std::uint64_t> size(const File& file) {
    if (auto rejection =
            detail::rejection_of(detail::precheck_state_op(!file.is_open(), file.access(),
                                                           FileOperation::file_info));
        rejection.has_value()) {
        return make_unexpected<std::uint64_t>(*rejection);
    }

    struct ::stat st {};
    int rc = detail::retry_on_eintr([&] { return ::fstat(file.native_handle(), &st); });
    if (rc < 0) {
        return make_unexpected<std::uint64_t>(from_errno_value(errno));
    }
    return static_cast<std::uint64_t>(st.st_size);
}

Result<void> resize(const File& file, std::uint64_t new_size) {
    if (auto rejection = detail::rejection_of(
            detail::precheck_resize(!file.is_open(), file.access(), new_size));
        rejection.has_value()) {
        return make_unexpected<void>(*rejection);
    }

    const auto native_size = detail::checked_posix_offset(new_size);
    int rc = detail::retry_on_eintr(
        [&] { return ::ftruncate(file.native_handle(), native_size.value()); });
    if (rc < 0) {
        return make_unexpected<void>(from_errno_value(errno));
    }
    return {};
}

Result<void> sync_all(const File& file) {
    if (auto rejection =
            detail::rejection_of(detail::precheck_state_op(!file.is_open(), file.access(),
                                                           FileOperation::sync_all));
        rejection.has_value()) {
        return make_unexpected<void>(*rejection);
    }

    int rc = detail::retry_on_eintr([&] { return ::fsync(file.native_handle()); });
    if (rc < 0) {
        return make_unexpected<void>(from_errno_value(errno));
    }
    return {};
}

} // namespace sluice::blocking
