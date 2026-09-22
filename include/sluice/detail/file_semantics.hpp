#pragma once

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

static_assert(std::numeric_limits<off_t>::is_integer && std::numeric_limits<off_t>::is_signed,
              "sluice positional I/O requires a signed integral off_t");
static_assert(std::numeric_limits<off_t>::digits >= 63,
              "sluice positional I/O requires 64-bit large-file support "
              "(_FILE_OFFSET_BITS=64 / LFS)");
static_assert(std::numeric_limits<ssize_t>::is_integer && std::numeric_limits<ssize_t>::is_signed,
              "sluice positional I/O requires a signed integral ssize_t");

inline constexpr std::uint64_t kMaxNativeOffset =
    static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());

inline constexpr std::size_t kMaxNativeTransfer =
    static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());

constexpr bool range_is_valid(std::uint64_t offset, std::size_t length) noexcept {
    if (length == 0)
        return true;
    if (offset > kMaxNativeOffset)
        return false;
    // Last addressed byte is offset + (length - 1), computed without overflowing.
    return length - 1 <= kMaxNativeOffset - offset;
}

constexpr bool size_is_representable(std::uint64_t size) noexcept {
    return size <= kMaxNativeOffset;
}

inline Result<off_t> checked_posix_offset(std::uint64_t offset) {
    if (offset > kMaxNativeOffset)
        return make_unexpected<off_t>(IoError{.code = IoError::Code::invalid_argument});
    return static_cast<off_t>(offset);
}

enum class FileOperation : std::uint8_t {
    read,
    write,
    file_info,
    resize,
    sync_data,
    sync_all,
};

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

enum class OpenVerdict : std::uint8_t {
    legal,
    reject_path_has_embedded_nul,
    reject_read_only_truncate,
};

// This must run before the native open: an embedded NUL would otherwise
// silently truncate the name.
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

enum class DataOpVerdict : std::uint8_t {
    execute,
    complete_empty,
    reject_closed,
    reject_access,
    reject_range,
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

constexpr std::optional<IoError> rejection_of(DataOpVerdict verdict) noexcept {
    if (!is_rejection(verdict))
        return std::nullopt;
    return rejection_error(verdict);
}

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
};

constexpr DataOpVerdict precheck_data_op(const DataOpRequest& request) noexcept {
    if (request.closed)
        return DataOpVerdict::reject_closed;
    if (!access_allows(request.access, request.operation))
        return DataOpVerdict::reject_access;
    if (request.length == 0)
        return DataOpVerdict::complete_empty;
    if (!range_is_valid(request.offset, request.length))
        return DataOpVerdict::reject_range;
    if (request.length > kMaxNativeTransfer)
        return DataOpVerdict::reject_range;
    return DataOpVerdict::execute;
}

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

enum class PrimitiveOutcome : std::uint8_t {
    empty_request,
    eof,
    full_progress,
    short_progress,
    zero_write_progress,
};

constexpr PrimitiveOutcome classify_primitive(FileOperation operation, std::size_t requested,
                                              std::size_t transferred) noexcept {
    if (requested == 0)
        return PrimitiveOutcome::empty_request;
    if (transferred == 0) {
        return operation == FileOperation::read ? PrimitiveOutcome::eof
                                                : PrimitiveOutcome::zero_write_progress;
    }
    return transferred == requested ? PrimitiveOutcome::full_progress
                                    : PrimitiveOutcome::short_progress;
}

enum class CompositionKind : std::uint8_t {
    read_exact,
    write_all,
};

enum class CompositionStop : std::uint8_t {
    complete,
    eof_before_full,
    write_no_progress,
    primitive_error,
    impossible_count,
};

struct CompositionState {
    std::size_t confirmed_bytes = 0;
    bool stopped = false;
    CompositionStop stop = CompositionStop::complete;
    IoError error{.code = IoError::Code::backend_error};

    constexpr bool complete() const noexcept { return !stopped; }
    friend bool operator==(const CompositionState&, const CompositionState&) noexcept = default;
};

// Precondition: state.confirmed_bytes <= requested_total.
constexpr CompositionState compose_progress(CompositionKind kind, std::size_t requested_total,
                                            CompositionState state,
                                            std::size_t transferred) noexcept {
    if (state.stopped)
        return state;
    const std::size_t remaining = requested_total - state.confirmed_bytes;
    if (transferred > remaining) {
        state.stopped = true;
        state.stop = CompositionStop::impossible_count;
        return state;
    }
    state.confirmed_bytes += transferred;
    if (state.confirmed_bytes == requested_total)
        return state;
    if (transferred == 0) {
        state.stopped = true;
        state.stop = kind == CompositionKind::read_exact ? CompositionStop::eof_before_full
                                                        : CompositionStop::write_no_progress;
    }
    return state;
}

constexpr CompositionState compose_error(CompositionState state, IoError error) noexcept {
    if (state.stopped)
        return state;
    state.stopped = true;
    state.stop = CompositionStop::primitive_error;
    state.error = error;
    return state;
}

constexpr std::optional<IoError> composition_error(const CompositionState& state) noexcept {
    if (!state.stopped)
        return std::nullopt;
    switch (state.stop) {
    case CompositionStop::complete:
        return std::nullopt;
    case CompositionStop::eof_before_full:
        return IoError{.code = IoError::Code::eof};
    case CompositionStop::write_no_progress:
    case CompositionStop::impossible_count:
        return IoError{.code = IoError::Code::invalid_state};
    case CompositionStop::primitive_error:
        return state.error;
    }
    return std::nullopt;
}

enum class EffectCertainty : std::uint8_t {
    accounted,
    unknown,
};

struct IoEffect {
    std::uint64_t confirmed_bytes = 0;
    EffectCertainty remaining = EffectCertainty::accounted;

    friend bool operator==(const IoEffect&, const IoEffect&) noexcept = default;
};

struct IoOutcome {
    bool succeeded = false;
    IoEffect effect{};
    IoError error{};

    static constexpr IoOutcome success(std::uint64_t confirmed_bytes = 0) noexcept {
        return IoOutcome{true, IoEffect{confirmed_bytes, EffectCertainty::accounted}, IoError{}};
    }

    static constexpr IoOutcome failure(IoError reason,
                                       std::uint64_t confirmed_bytes = 0) noexcept {
        return IoOutcome{false, IoEffect{confirmed_bytes, EffectCertainty::accounted}, reason};
    }

    static constexpr IoOutcome uncertain(IoError reason,
                                         std::uint64_t known_prefix = 0) noexcept {
        return IoOutcome{false, IoEffect{known_prefix, EffectCertainty::unknown}, reason};
    }

    constexpr bool is_canceled() const noexcept {
        return !succeeded && error.code == IoError::Code::canceled;
    }

    friend bool operator==(const IoOutcome&, const IoOutcome&) noexcept = default;
};

constexpr bool prefix_only_terminal_can_carry(const IoOutcome& outcome) noexcept {
    return outcome.effect.remaining == EffectCertainty::accounted;
}

constexpr IoOutcome failed_dispatched_attempt(IoError reason,
                                              std::uint64_t known_prefix = 0) noexcept {
    return IoOutcome::uncertain(reason, known_prefix);
}

constexpr IoOutcome canceled_before_dispatch(std::uint64_t confirmed_prefix = 0) noexcept {
    return IoOutcome::failure(IoError{.code = IoError::Code::canceled}, confirmed_prefix);
}

constexpr IoOutcome canceled_racing_in_flight_attempt(std::uint64_t confirmed_bytes) noexcept {
    return IoOutcome::uncertain(IoError{.code = IoError::Code::canceled}, confirmed_bytes);
}

constexpr EffectCertainty composition_effect_certainty(const CompositionState& state) noexcept {
    if (!state.stopped)
        return EffectCertainty::accounted;
    switch (state.stop) {
    case CompositionStop::complete:
    case CompositionStop::eof_before_full:
    case CompositionStop::write_no_progress:
        return EffectCertainty::accounted;
    case CompositionStop::primitive_error:
    case CompositionStop::impossible_count:
        return EffectCertainty::unknown;
    }
    return EffectCertainty::accounted;
}

enum class MutationKind : std::uint8_t {
    write,
    resize_shrink,
    resize_grow,
    metadata,
};

enum class CompletionState : std::uint8_t {
    submitted,
    observed,
};

enum class SyncKind : std::uint8_t {
    data,
    all,
};

struct MutationRecord {
    MutationKind kind = MutationKind::write;
    CompletionState completion = CompletionState::submitted;
    std::uint64_t completion_sequence = 0;
};

struct SyncRecord {
    SyncKind kind = SyncKind::data;
    bool succeeded = false;
    std::uint64_t initiation_sequence = 0;
};

constexpr bool covers(const SyncRecord& sync, const MutationRecord& mutation) noexcept {
    if (!sync.succeeded)
        return false;
    if (mutation.completion != CompletionState::observed)
        return false;
    if (mutation.completion_sequence >= sync.initiation_sequence)
        return false;
    switch (mutation.kind) {
    case MutationKind::write:
        return true;
    case MutationKind::resize_shrink:
    case MutationKind::resize_grow:
        return true;
    case MutationKind::metadata:
        return sync.kind == SyncKind::all;
    }
    return false;
}

constexpr bool grants_durability_alone(const MutationRecord&) noexcept {
    return false;
}

constexpr bool ordered_after_sync(const SyncRecord& sync, const MutationRecord& mutation) noexcept {
    return mutation.completion == CompletionState::observed &&
           mutation.completion_sequence > sync.initiation_sequence;
}

constexpr bool preserves_exact_state(const SyncRecord&, const MutationRecord&) noexcept {
    return false;
}

}
