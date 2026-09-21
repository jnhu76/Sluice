#pragma once

// Shared File semantic oracle: the single authority for the v1 File rules that
// direct execution, ThreadPool and io_uring must all obey. Execution paths call
// these rules instead of re-deriving File legality, validation precedence,
// range validity or error categories, so one code change moves every path
// together.
//
// This header is a pure decision surface. It may answer only:
//   - is this logical operation legal?
//   - which semantic result/error category follows?
//   - what effect information must be representable?
//   - which durability relation is allowed?
// It must never answer which backend/worker/ring/request slot/observer runs the
// operation, when to poll, or whether capacity exists, and it owns no File,
// backend, context, request, buffer or thread. A logical layer is an authority
// boundary, not a requirement for another class or vtable.
//
// Platform-lowering detail stays with the platform: this header decides that a
// combination is legal, while `src/file_resource.cpp` owns the POSIX flag
// spelling of that decision.

#include <sluice/error.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#include <sys/types.h>

namespace sluice::detail {

// ─── Range and offset (SEM-04) ──────────────────────────────────────────────

static_assert(std::numeric_limits<off_t>::is_integer && std::numeric_limits<off_t>::is_signed,
              "sluice positional I/O requires a signed integral off_t");
static_assert(std::numeric_limits<off_t>::digits >= 63,
              "sluice positional I/O requires 64-bit large-file support "
              "(_FILE_OFFSET_BITS=64 / LFS)");
static_assert(std::numeric_limits<ssize_t>::is_integer && std::numeric_limits<ssize_t>::is_signed,
              "sluice positional I/O requires a signed integral ssize_t");

inline constexpr std::uint64_t kMaxNativeOffset =
    static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());

// Largest transfer a single positional or shared-cursor call may request.
// A larger count is not representable in the native interface, so it is an
// invalid range rather than a limit of any particular execution.
inline constexpr std::size_t kMaxNativeTransfer =
    static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());

// A zero-length request addresses no byte, so it has no range to validate.
// The operation precheck below relies on this: precedence puts the logical
// no-op ahead of the range step.
constexpr bool range_is_valid(std::uint64_t offset, std::size_t length) noexcept {
    if (length == 0)
        return true;
    if (offset > kMaxNativeOffset)
        return false;
    // Last addressed byte is offset + (length - 1), computed without overflowing.
    return length - 1 <= kMaxNativeOffset - offset;
}

// Resize takes a size, not a range: there is no length to combine it with.
constexpr bool size_is_representable(std::uint64_t size) noexcept {
    return size <= kMaxNativeOffset;
}

inline Result<off_t> checked_posix_offset(std::uint64_t offset) {
    if (offset > kMaxNativeOffset)
        return make_unexpected<off_t>(IoError{.code = IoError::Code::invalid_argument});
    return static_cast<off_t>(offset);
}

// ─── Operation kinds and access legality (SEM-03) ───────────────────────────

enum class FileOperation : std::uint8_t {
    read,
    write,
    file_info,
    resize,
    sync_data,
    sync_all,
};

// The access matrix is a property of the operation, not of the execution path:
// a File that is illegal to read from is illegal to read from everywhere.
constexpr bool access_allows(FileAccess access, FileOperation operation) noexcept {
    switch (operation) {
    case FileOperation::read:
        return access != FileAccess::write_only;
    case FileOperation::write:
        return access != FileAccess::read_only;
    case FileOperation::resize:
        return access != FileAccess::read_only;
    case FileOperation::file_info:
    case FileOperation::sync_data:
    case FileOperation::sync_all:
        return true;
    }
    return false;
}

constexpr bool is_byte_operation(FileOperation operation) noexcept {
    return operation == FileOperation::read || operation == FileOperation::write;
}

// ─── Open legality (SEM-02) ────────────────────────────────────────────────

enum class OpenVerdict : std::uint8_t {
    legal,
    reject_path_has_embedded_nul,
    reject_read_only_truncate,
};

// Both rejections are `invalid_argument` with no OS or namespace effect, so
// their relative order is not caller-observable. The path is checked first
// because a malformed path does not denote a resource at all. The caller must
// run this before the native open: an embedded NUL would otherwise silently
// truncate the name, and a rejected combination must not have touched the file.
constexpr OpenVerdict precheck_open(FileOpen mode, std::string_view path) noexcept {
    if (path.find('\0') != std::string_view::npos)
        return OpenVerdict::reject_path_has_embedded_nul;
    if (mode.access == FileAccess::read_only &&
        mode.contents == FileInitialContents::truncate) {
        return OpenVerdict::reject_read_only_truncate;
    }
    return OpenVerdict::legal;
}

constexpr std::optional<IoError> open_rejection_of(OpenVerdict verdict) noexcept {
    switch (verdict) {
    case OpenVerdict::legal:
        return std::nullopt;
    case OpenVerdict::reject_path_has_embedded_nul:
    case OpenVerdict::reject_read_only_truncate:
        return IoError{.code = IoError::Code::invalid_argument};
    }
    return std::nullopt;
}

// ─── Validation precedence (SEM-03) ────────────────────────────────────────

enum class DataOpVerdict : std::uint8_t {
    execute,        // steps 1-4 passed; the caller proceeds to context/admission/execution
    complete_empty, // logical no-op: report success 0 and make no OS call
    reject_closed,  // step 1
    reject_access,  // step 2
    reject_range,   // step 4
};

constexpr bool is_rejection(DataOpVerdict verdict) noexcept {
    return verdict == DataOpVerdict::reject_closed || verdict == DataOpVerdict::reject_access ||
           verdict == DataOpVerdict::reject_range;
}

constexpr IoError rejection_error(DataOpVerdict verdict) noexcept {
    switch (verdict) {
    case DataOpVerdict::reject_closed:
        return IoError{.code = IoError::Code::invalid_state};
    case DataOpVerdict::reject_access:
    case DataOpVerdict::reject_range:
        return IoError{.code = IoError::Code::invalid_argument};
    case DataOpVerdict::execute:
    case DataOpVerdict::complete_empty:
        break;
    }
    // Not a rejection: callers must test is_rejection() first.
    return IoError{.code = IoError::Code::invalid_state};
}

// Convenience for call sites: nullopt when the operation may proceed, the
// canonical rejection otherwise.
constexpr std::optional<IoError> rejection_of(DataOpVerdict verdict) noexcept {
    if (!is_rejection(verdict))
        return std::nullopt;
    return rejection_error(verdict);
}

// Validation entry points share this conversion: a rejection becomes an error,
// while `execute` and `complete_empty` both mean the operation may proceed.
inline Result<void> accept_or_reject(DataOpVerdict verdict) {
    if (auto rejection = rejection_of(verdict); rejection.has_value())
        return make_unexpected<void>(*rejection);
    return {};
}

inline Result<void> accept_or_reject(OpenVerdict verdict) {
    if (auto rejection = open_rejection_of(verdict); rejection.has_value())
        return make_unexpected<void>(*rejection);
    return {};
}

struct DataOpRequest {
    bool closed = false;
    FileAccess access = FileAccess::read_only;
    FileOperation operation = FileOperation::read;
    std::uint64_t offset = 0;
    std::size_t length = 0;
    // The only dynamically detectable buffer precondition: a nonzero length
    // with no buffer cannot describe a valid extent. A zero-length request
    // never reaches this check.
    bool buffer_present = true;
};

// Steps 1-4 of the canonical precedence for byte operations. Steps 5-7
// (context health/compatibility, execution support, capacity/acceptance) belong
// to the request path and are deliberately not answered here.
constexpr DataOpVerdict precheck_data_op(const DataOpRequest& request) noexcept {
    if (request.closed)
        return DataOpVerdict::reject_closed;
    if (!access_allows(request.access, request.operation))
        return DataOpVerdict::reject_access;
    if (request.length == 0)
        return DataOpVerdict::complete_empty;
    if (!request.buffer_present)
        return DataOpVerdict::reject_range;
    if (!range_is_valid(request.offset, request.length))
        return DataOpVerdict::reject_range;
    if (request.length > kMaxNativeTransfer)
        return DataOpVerdict::reject_range;
    return DataOpVerdict::execute;
}

// file_info / resize / sync_data / sync_all: no length, so the zero-length
// no-op step does not apply and the operation moves from step 2 to step 4.
constexpr DataOpVerdict precheck_state_op(bool closed, FileAccess access,
                                          FileOperation operation) noexcept {
    if (closed)
        return DataOpVerdict::reject_closed;
    if (!access_allows(access, operation))
        return DataOpVerdict::reject_access;
    return DataOpVerdict::execute;
}

constexpr DataOpVerdict precheck_resize(bool closed, FileAccess access,
                                        std::uint64_t new_size) noexcept {
    const DataOpVerdict verdict = precheck_state_op(closed, access, FileOperation::resize);
    if (verdict != DataOpVerdict::execute)
        return verdict;
    if (!size_is_representable(new_size))
        return DataOpVerdict::reject_range;
    return DataOpVerdict::execute;
}

} // namespace sluice::detail
