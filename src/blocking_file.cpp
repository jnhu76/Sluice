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

#ifdef SLUICE_FILE_INTERNAL_TESTING
#include "file_test_seams.hpp"
#endif

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
                  std::size_t length, const std::byte* buffer) {
    const DataOpVerdict verdict = detail::precheck_data_op(detail::DataOpRequest{
        !file.is_open(), file.access(), operation, offset, length});
    // Implementation precondition of this raw-pointer surface, deliberately not
    // a shared rule: SEM-03 treats caller memory validity as not dynamically
    // detectable, so the oracle does not answer buffer presence. This surface
    // fails fast instead of handing a null pointer with a nonzero length to the
    // kernel. `execute` implies a nonzero length.
    if (verdict == DataOpVerdict::execute && buffer == nullptr)
        return Precheck{IoError{.code = IoError::Code::invalid_argument}, false};
    return Precheck{detail::rejection_of(verdict), verdict == DataOpVerdict::complete_empty};
}

#ifdef SLUICE_FILE_INTERNAL_TESTING
// Test-only: the primitive fault seam sits exactly at the native-call boundary,
// so a scripted outcome drives the same primitive and composition code the
// production build runs. A call outside the armed script's family passes
// through. The operands are recorded so a test can pin the buffer and offset a
// composition passed to the native call.
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
    if (!native_offset.has_value()) {
        return make_unexpected<std::size_t>(native_offset.error());
    }
    ssize_t n = detail::retry_on_eintr([&] {
        return native_pread(file.native_handle(), dst.data(), dst.size(), native_offset.value());
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
    if (!native_offset.has_value()) {
        return make_unexpected<std::size_t>(native_offset.error());
    }
    ssize_t n = detail::retry_on_eintr([&] {
        return native_pwrite(file.native_handle(), src.data(), src.size(), native_offset.value());
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
        return native_read(file.native_handle(), dst.data(), dst.size());
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
    // SEM-07: `regular` is the supported data-I/O kind, and every other native
    // kind is reported as `other` without an ordinary-file promise. The kind set
    // is a v1 decision, not a stat-mode mirror.
    info.kind = S_ISREG(st.st_mode) ? FileKind::regular : FileKind::other;
    info.size = static_cast<std::uint64_t>(st.st_size);
    // Linux supplies device/inode here unconditionally; the type still carries
    // the explicit unavailable outcome, which is what keeps the public contract
    // from depending on that.
    info.identity = FileIdentity{static_cast<std::uint64_t>(st.st_dev),
                                 static_cast<std::uint64_t>(st.st_ino)};
    return info;
}

// The size projection of the same observation: one metadata substrate, so
// `size(file)` and `file_info(file).size` cannot disagree.
Result<std::uint64_t> size(const File& file) {
    auto info = file_info(file);
    if (!info.has_value()) {
        return make_unexpected<std::uint64_t>(info.error());
    }
    return info.value().size;
}

Result<void> resize(const File& file, std::uint64_t new_size) {
    if (auto rejection = detail::rejection_of(
            detail::precheck_resize(!file.is_open(), file.access(), new_size));
        rejection.has_value()) {
        return make_unexpected<void>(*rejection);
    }

    const auto native_size = detail::checked_posix_offset(new_size);
    if (!native_size.has_value()) {
        return make_unexpected<void>(native_size.error());
    }
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

// ─── Exact/all composition (SEM-05, ERR-02) ────────────────────────────────

namespace {

using detail::CompositionKind;
using detail::CompositionState;

// Maps the shared reference composition state onto this adapter's public
// outcome, so the direct path consumes the one composition rule instead of
// restating it. `impossible_count` cannot arise from a primitive that honors
// its own contract (0 <= n <= requested); it is routed through the reference
// error rule rather than given public vocabulary of its own. A primitive
// rejection observed after the composition started (a caller racing a close, for
// instance) travels the same path: it is reported as a primitive error with its
// own category preserved, not as a fresh semantic rejection.
CompositionOutcome direct_outcome(const CompositionState& state) noexcept {
    CompositionOutcome outcome;
    outcome.confirmed_bytes = state.confirmed_bytes;
    switch (state.stop) {
    case detail::CompositionStop::complete:
        outcome.end = CompositionEnd::complete;
        break;
    case detail::CompositionStop::eof_before_full:
        outcome.end = CompositionEnd::eof_before_full;
        break;
    case detail::CompositionStop::write_no_progress:
        outcome.end = CompositionEnd::write_no_progress;
        break;
    case detail::CompositionStop::primitive_error:
        outcome.end = CompositionEnd::primitive_error;
        outcome.error = state.error;
        break;
    case detail::CompositionStop::impossible_count:
        outcome.end = CompositionEnd::primitive_error;
        outcome.error = IoError{.code = IoError::Code::invalid_state};
        break;
    }
    return outcome;
}

// The one exact/all loop. `primitive(confirmed)` performs a single primitive
// step at the confirmed prefix, so the positional and shared-cursor forms differ
// only in which primitive they name. Every path terminates without re-entering
// the primitive: a full transfer, a zero-transfer step (EOF for read_exact, no
// progress for write_all), a primitive error, or a count above the remaining
// request. `stopped` in the shared state means "stopped short", so reaching the
// requested total is the loop's own exit condition.
//
// `confirmed` never exceeds the validated request, so an advanced offset stays
// inside the range the precheck accepted and the arithmetic cannot overflow.
template <class Primitive>
Result<CompositionOutcome> compose(CompositionKind kind, std::size_t total,
                                   Primitive&& primitive) {
    CompositionState state;
    while (!state.stopped && state.confirmed_bytes < total) {
        const std::size_t confirmed = state.confirmed_bytes;
        auto step = primitive(confirmed);
        if (!step.has_value()) {
            state = detail::compose_error(state, step.error());
            break;
        }
        state = detail::compose_progress(kind, total, state, step.value());
    }
    return direct_outcome(state);
}

} // namespace

Result<CompositionOutcome> read_exact_at(const File& file, std::uint64_t offset,
                                         std::span<std::byte> dst) {
    const Precheck pre = precheck(file, FileOperation::read, offset, dst.size(), dst.data());
    if (pre.rejection.has_value()) {
        return make_unexpected<CompositionOutcome>(*pre.rejection);
    }
    if (pre.complete_empty) {
        return CompositionOutcome{};
    }
    return compose(CompositionKind::read_exact, dst.size(), [&](std::size_t confirmed) {
        return read_at(file, offset + confirmed, dst.subspan(confirmed));
    });
}

Result<CompositionOutcome> write_all_at(const File& file, std::uint64_t offset,
                                        std::span<const std::byte> src) {
    const Precheck pre = precheck(file, FileOperation::write, offset, src.size(), src.data());
    if (pre.rejection.has_value()) {
        return make_unexpected<CompositionOutcome>(*pre.rejection);
    }
    if (pre.complete_empty) {
        return CompositionOutcome{};
    }
    return compose(CompositionKind::write_all, src.size(), [&](std::size_t confirmed) {
        return write_at(file, offset + confirmed, src.subspan(confirmed));
    });
}

Result<CompositionOutcome> read_exact(const File& file, std::span<std::byte> dst) {
    const Precheck pre = precheck(file, FileOperation::read, 0, dst.size(), dst.data());
    if (pre.rejection.has_value()) {
        return make_unexpected<CompositionOutcome>(*pre.rejection);
    }
    if (pre.complete_empty) {
        return CompositionOutcome{};
    }
    return compose(CompositionKind::read_exact, dst.size(),
                   [&](std::size_t confirmed) { return read(file, dst.subspan(confirmed)); });
}

Result<CompositionOutcome> write_all(const File& file, std::span<const std::byte> src) {
    const Precheck pre = precheck(file, FileOperation::write, 0, src.size(), src.data());
    if (pre.rejection.has_value()) {
        return make_unexpected<CompositionOutcome>(*pre.rejection);
    }
    if (pre.complete_empty) {
        return CompositionOutcome{};
    }
    return compose(CompositionKind::write_all, src.size(),
                   [&](std::size_t confirmed) { return write(file, src.subspan(confirmed)); });
}

} // namespace sluice::blocking
