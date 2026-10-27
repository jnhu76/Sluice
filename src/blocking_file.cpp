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
#include <utility>

#ifdef SLUICE_FILE_INTERNAL_TESTING
#include "file_test_seams.hpp"
#endif

namespace sluice::blocking {

namespace {

using detail::DataOpVerdict;
using detail::FileOperation;

inline DataOpVerdict admit_data_op(const File& file, FileOperation operation,
                                   std::uint64_t offset, std::size_t length) {
    return detail::precheck_data_op(
        detail::DataOpRequest{!file.is_open(), file.access(), operation, offset, length});
}

#ifdef SLUICE_FILE_INTERNAL_TESTING
bool intercepted(file_testing::NativeCall call, int fd) noexcept {
    file_testing::NativeScript* script = file_testing::NativeScript::active();
    return script != nullptr && script->intercepts(call, fd);
}

long scripted(file_testing::NativeCall call, int fd, const void* buffer, std::size_t count,
              long offset) noexcept {
    return file_testing::NativeScript::active()->next(call, fd, buffer, count, offset);
}
#endif

ssize_t native_read(int fd, void* buf, std::size_t count) {
#ifdef SLUICE_FILE_INTERNAL_TESTING
    if (intercepted(file_testing::NativeCall::read, fd))
        return static_cast<ssize_t>(scripted(file_testing::NativeCall::read, fd, buf, count, -1));
#endif
    return ::read(fd, buf, count);
}

ssize_t native_pread(int fd, void* buf, std::size_t count, off_t offset) {
#ifdef SLUICE_FILE_INTERNAL_TESTING
    if (intercepted(file_testing::NativeCall::pread, fd))
        return static_cast<ssize_t>(
            scripted(file_testing::NativeCall::pread, fd, buf, count, static_cast<long>(offset)));
#endif
    return ::pread(fd, buf, count, offset);
}

ssize_t native_write(int fd, const void* buf, std::size_t count) {
#ifdef SLUICE_FILE_INTERNAL_TESTING
    if (intercepted(file_testing::NativeCall::write, fd))
        return static_cast<ssize_t>(scripted(file_testing::NativeCall::write, fd, buf, count, -1));
#endif
    return ::write(fd, buf, count);
}

ssize_t native_pwrite(int fd, const void* buf, std::size_t count, off_t offset) {
#ifdef SLUICE_FILE_INTERNAL_TESTING
    if (intercepted(file_testing::NativeCall::pwrite, fd))
        return static_cast<ssize_t>(
            scripted(file_testing::NativeCall::pwrite, fd, buf, count, static_cast<long>(offset)));
#endif
    return ::pwrite(fd, buf, count, offset);
}

template <class Fn> auto step_retry_on_eintr(Fn&& fn) -> decltype(fn()) {
#if defined(SLUICE_DIRECT_MUTANT_STEP_SKIPS_EINTR_RETRY)
    return fn();
#else
    return detail::retry_on_eintr(std::forward<Fn>(fn));
#endif
}

}
Result<std::size_t> read_at(const File& file, std::uint64_t offset,
                            std::span<std::byte> dst) {
    const DataOpVerdict verdict =
        admit_data_op(file, FileOperation::read, offset, dst.size());
    if (verdict == DataOpVerdict::complete_empty)
        return std::size_t{0};
    if (detail::is_rejection(verdict))
        return make_unexpected<std::size_t>(detail::rejection_error(verdict));
    if (dst.data() == nullptr)
        return make_unexpected<std::size_t>(IoError{.code = IoError::Code::invalid_argument});
    // The execute verdict proved offset fits off_t; the cast cannot lose bits.
    ssize_t n = detail::retry_on_eintr([&] {
        return native_pread(file.native_handle(), dst.data(), dst.size(),
                            static_cast<off_t>(offset));
    });
    if (n < 0) {
        return make_unexpected<std::size_t>(from_errno_value(errno));
    }
    return static_cast<std::size_t>(n);
}

Result<std::size_t> write_at(const File& file, std::uint64_t offset,
                             std::span<const std::byte> src) {
    const DataOpVerdict verdict =
        admit_data_op(file, FileOperation::write, offset, src.size());
    if (verdict == DataOpVerdict::complete_empty)
        return std::size_t{0};
    if (detail::is_rejection(verdict))
        return make_unexpected<std::size_t>(detail::rejection_error(verdict));
    if (src.data() == nullptr)
        return make_unexpected<std::size_t>(IoError{.code = IoError::Code::invalid_argument});
    // The execute verdict proved offset fits off_t; the cast cannot lose bits.
    ssize_t n = detail::retry_on_eintr([&] {
        return native_pwrite(file.native_handle(), src.data(), src.size(),
                             static_cast<off_t>(offset));
    });
    if (n < 0) {
        return make_unexpected<std::size_t>(from_errno_value(errno));
    }
    return static_cast<std::size_t>(n);
}

Result<std::size_t> read(const File& file, std::span<std::byte> dst) {
    const DataOpVerdict verdict = admit_data_op(file, FileOperation::read, 0, dst.size());
    if (verdict == DataOpVerdict::complete_empty)
        return std::size_t{0};
    if (detail::is_rejection(verdict))
        return make_unexpected<std::size_t>(detail::rejection_error(verdict));
    if (dst.data() == nullptr)
        return make_unexpected<std::size_t>(IoError{.code = IoError::Code::invalid_argument});

    ssize_t n = detail::retry_on_eintr([&] {
        return native_read(file.native_handle(), dst.data(), dst.size());
    });
    if (n < 0) {
        return make_unexpected<std::size_t>(from_errno_value(errno));
    }
    return static_cast<std::size_t>(n);
}

Result<std::size_t> write(const File& file, std::span<const std::byte> src) {
    const DataOpVerdict verdict = admit_data_op(file, FileOperation::write, 0, src.size());
    if (verdict == DataOpVerdict::complete_empty)
        return std::size_t{0};
    if (detail::is_rejection(verdict))
        return make_unexpected<std::size_t>(detail::rejection_error(verdict));
    if (src.data() == nullptr)
        return make_unexpected<std::size_t>(IoError{.code = IoError::Code::invalid_argument});

    ssize_t n = detail::retry_on_eintr([&] {
        return native_write(file.native_handle(), src.data(), src.size());
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

Result<FileInfo> file_info(const File& file) {
    if (auto rejection =
            detail::rejection_of(detail::precheck_state_op(!file.is_open(), file.access(),
                                                           FileOperation::file_info));
        rejection.has_value()) {
        return make_unexpected<FileInfo>(*rejection);
    }

    struct ::stat st {};
    int rc = detail::retry_on_eintr([&] { return ::fstat(file.native_handle(), &st); });
    if (rc < 0) {
        return make_unexpected<FileInfo>(from_errno_value(errno));
    }

    FileInfo info;
    info.kind = S_ISREG(st.st_mode) ? FileKind::regular : FileKind::other;
    info.size = static_cast<std::uint64_t>(st.st_size);
    info.identity = FileIdentity{static_cast<std::uint64_t>(st.st_dev),
                                 static_cast<std::uint64_t>(st.st_ino)};
    return info;
}

Result<std::uint64_t> size(const File& file) {
    auto info = file_info(file);
    if (!info.has_value()) {
        return make_unexpected<std::uint64_t>(info.error());
    }
    return info.value().size;
}

Result<void> resize(const File& file, std::uint64_t new_size) {
    const DataOpVerdict verdict =
        detail::precheck_resize(!file.is_open(), file.access(), new_size);
    if (detail::is_rejection(verdict)) {
        return make_unexpected<void>(detail::rejection_error(verdict));
    }
    // The execute verdict proved new_size fits off_t; the cast cannot lose bits.
    int rc = detail::retry_on_eintr(
        [&] { return ::ftruncate(file.native_handle(), static_cast<off_t>(new_size)); });
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

namespace {

using detail::CompositionKind;

constexpr EffectCertainty failure_publish_certainty() noexcept {
#if defined(SLUICE_COMPOSE_MUTANT_FAILURE_ACCOUNTED)
    return EffectCertainty::accounted;
#else
    return EffectCertainty::unknown;
#endif
}

Result<CompositionOutcome> complete_outcome(std::size_t confirmed_bytes) noexcept {
    CompositionOutcome outcome;
    outcome.confirmed_bytes = confirmed_bytes;
    return outcome;
}

[[maybe_unused]] Result<CompositionOutcome> stalled_outcome(CompositionEnd end,
                                                            std::size_t confirmed_bytes) noexcept {
    CompositionOutcome outcome;
    outcome.confirmed_bytes = confirmed_bytes;
    outcome.end = end;
    return outcome;
}

Result<CompositionOutcome> primitive_error_outcome(std::size_t confirmed_bytes,
                                                   IoError error) noexcept {
    CompositionOutcome outcome;
    outcome.confirmed_bytes = confirmed_bytes;
    outcome.end = CompositionEnd::primitive_error;
    outcome.remaining = failure_publish_certainty();
    outcome.error = error;
    return outcome;
}

// The composition entry admitted the whole request before compose() runs, so
// the step offsets below stay inside the validated range; the off_t casts
// cannot lose bits.
Result<std::size_t> prevalidated_pread_step(const File& file, std::uint64_t offset,
                                            std::span<std::byte> dst) {
    ssize_t n = step_retry_on_eintr([&] {
        return native_pread(file.native_handle(), dst.data(), dst.size(),
                            static_cast<off_t>(offset));
    });
    if (n < 0) {
        return make_unexpected<std::size_t>(from_errno_value(errno));
    }
    return static_cast<std::size_t>(n);
}

Result<std::size_t> prevalidated_read_step(const File& file, std::span<std::byte> dst) {
    ssize_t n = step_retry_on_eintr([&] {
        return native_read(file.native_handle(), dst.data(), dst.size());
    });
    if (n < 0) {
        return make_unexpected<std::size_t>(from_errno_value(errno));
    }
    return static_cast<std::size_t>(n);
}

// `confirmed` never exceeds the validated request, so the arithmetic cannot
// overflow.
template <class Primitive>
Result<CompositionOutcome> compose(CompositionKind kind, std::size_t total,
                                   Primitive&& primitive) {
    std::size_t confirmed_bytes = 0;
    while (confirmed_bytes < total) {
        auto step = primitive(confirmed_bytes);
        if (!step.has_value()) {
            return primitive_error_outcome(confirmed_bytes, step.error());
        }
#if defined(SLUICE_COMPOSE_MUTANT_ADVANCE_BY_REQUESTED) || \
    defined(SLUICE_DIRECT_MUTANT_STEP_REPORTS_FULL_COUNT)
        const std::size_t transferred = total - confirmed_bytes;
#else
        const std::size_t transferred = step.value();
#endif
#if !defined(SLUICE_COMPOSE_MUTANT_SKIP_IMPOSSIBLE_COUNT)
        if (transferred > total - confirmed_bytes) {
            return primitive_error_outcome(
                confirmed_bytes, IoError{.code = IoError::Code::invalid_state});
        }
#endif
        confirmed_bytes += transferred;
        if (confirmed_bytes == total)
            break;
#if !defined(SLUICE_COMPOSE_MUTANT_RETRY_ZERO_PROGRESS)
        if (transferred == 0) {
            return stalled_outcome(kind == CompositionKind::read_exact
                                       ? CompositionEnd::eof_before_full
                                       : CompositionEnd::write_no_progress,
                                   confirmed_bytes);
        }
#endif
    }
    return complete_outcome(confirmed_bytes);
}

}
Result<CompositionOutcome> read_exact_at(const File& file, std::uint64_t offset,
                                         std::span<std::byte> dst) {
    const DataOpVerdict verdict =
        admit_data_op(file, FileOperation::read, offset, dst.size());
    if (verdict == DataOpVerdict::complete_empty)
        return CompositionOutcome{};
    if (detail::is_rejection(verdict))
        return make_unexpected<CompositionOutcome>(detail::rejection_error(verdict));
    if (dst.data() == nullptr)
        return make_unexpected<CompositionOutcome>(
            IoError{.code = IoError::Code::invalid_argument});
    return compose(CompositionKind::read_exact, dst.size(), [&](std::size_t confirmed) {
        return prevalidated_pread_step(file, offset + confirmed, dst.subspan(confirmed));
    });
}

Result<CompositionOutcome> write_all_at(const File& file, std::uint64_t offset,
                                        std::span<const std::byte> src) {
    const DataOpVerdict verdict =
        admit_data_op(file, FileOperation::write, offset, src.size());
    if (verdict == DataOpVerdict::complete_empty)
        return CompositionOutcome{};
    if (detail::is_rejection(verdict))
        return make_unexpected<CompositionOutcome>(detail::rejection_error(verdict));
    if (src.data() == nullptr)
        return make_unexpected<CompositionOutcome>(
            IoError{.code = IoError::Code::invalid_argument});
    return compose(CompositionKind::write_all, src.size(), [&](std::size_t confirmed) {
        return write_at(file, offset + confirmed, src.subspan(confirmed));
    });
}

Result<CompositionOutcome> read_exact(const File& file, std::span<std::byte> dst) {
    const DataOpVerdict verdict = admit_data_op(file, FileOperation::read, 0, dst.size());
    if (verdict == DataOpVerdict::complete_empty)
        return CompositionOutcome{};
    if (detail::is_rejection(verdict))
        return make_unexpected<CompositionOutcome>(detail::rejection_error(verdict));
    if (dst.data() == nullptr)
        return make_unexpected<CompositionOutcome>(
            IoError{.code = IoError::Code::invalid_argument});
    return compose(CompositionKind::read_exact, dst.size(),
                   [&](std::size_t confirmed) { return prevalidated_read_step(file, dst.subspan(confirmed)); });
}

Result<CompositionOutcome> write_all(const File& file, std::span<const std::byte> src) {
    const DataOpVerdict verdict = admit_data_op(file, FileOperation::write, 0, src.size());
    if (verdict == DataOpVerdict::complete_empty)
        return CompositionOutcome{};
    if (detail::is_rejection(verdict))
        return make_unexpected<CompositionOutcome>(detail::rejection_error(verdict));
    if (src.data() == nullptr)
        return make_unexpected<CompositionOutcome>(
            IoError{.code = IoError::Code::invalid_argument});
    return compose(CompositionKind::write_all, src.size(),
                   [&](std::size_t confirmed) { return write(file, src.subspan(confirmed)); });
}

}
