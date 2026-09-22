#pragma once

// Shared File decision rules called by every execution path: open legality,
// the access matrix, validation precedence, range checks, the native error
// mapping, and the composition/effect/durability reference rules. This is a
// pure decision surface — it answers whether an operation is legal and which
// result follows, never which backend, worker, ring or slot runs it, and it
// owns no File, buffer or thread. Platform lowering (POSIX flag spelling)
// stays with the platform code.

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

// ─── Range and offset ───────────────────────────────────────────────────────

static_assert(std::numeric_limits<off_t>::is_integer && std::numeric_limits<off_t>::is_signed,
              "sluice positional I/O requires a signed integral off_t");
static_assert(std::numeric_limits<off_t>::digits >= 63,
              "sluice positional I/O requires 64-bit large-file support "
              "(_FILE_OFFSET_BITS=64 / LFS)");
static_assert(std::numeric_limits<ssize_t>::is_integer && std::numeric_limits<ssize_t>::is_signed,
              "sluice positional I/O requires a signed integral ssize_t");

inline constexpr std::uint64_t kMaxNativeOffset =
    static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());

// Largest transfer a single positional or shared-cursor call may request: a
// larger count is not natively representable, so it is an invalid range rather
// than a limit of any particular execution.
inline constexpr std::size_t kMaxNativeTransfer =
    static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());

// A zero-length request addresses no byte, so it has no range to validate; the
// operation precheck below relies on this to put the logical no-op ahead of
// the range step.
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

// ─── Operation kinds and access legality ────────────────────────────────────

enum class FileOperation : std::uint8_t {
    read,
    write,
    file_info,
    resize,
    sync_data,
    sync_all,
};

// Access legality is a property of the operation, shared by every execution
// path.
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

// ─── Open legality ─────────────────────────────────────────────────────────

enum class OpenVerdict : std::uint8_t {
    legal,
    reject_path_has_embedded_nul,
    reject_read_only_truncate,
};

// Both rejections are `invalid_argument` with no OS or namespace effect, so
// their relative order is not caller-observable. This must run before the
// native open: an embedded NUL would otherwise silently truncate the name.
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

// ─── Validation precedence ─────────────────────────────────────────────────

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
    // No buffer-presence field on purpose: caller memory validity is not
    // dynamically detectable, so buffer presence is not a shared rule. A
    // raw-pointer surface keeps its own null-buffer fail-fast beside its
    // pointers.
};

// Byte-operation validation: closed, access, logical no-op, then range.
// Context health/compatibility, execution support and capacity belong to the
// request path, not here.
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

// file_info / resize / sync_data / sync_all: no length operand, so the
// zero-length no-op step does not apply.
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

// ─── Primitive outcome and composition ──────────────────────────────────────

// Classification of a successful primitive transfer; an OS error is a separate
// outcome, never turned into a byte count here.
enum class PrimitiveOutcome : std::uint8_t {
    empty_request,       // success 0; the request observed no EOF
    eof,                 // a nonempty read observed 0 at its position; primitive success
    full_progress,       // 0 < transferred == requested
    short_progress,      // 0 < transferred < requested; a short count is allowed
    zero_write_progress, // a nonempty write reported 0: composition must stop
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
    eof_before_full,   // read_exact ran out of file before the requested length
    write_no_progress, // write_all saw a nonempty write transfer nothing
    primitive_error,   // the primitive reported an error or cancellation
    impossible_count,  // a primitive claimed more than was requested
};

// Reference state of an exact/all composition. Confirmed bytes accumulate over
// successful steps; a stop never discards the prefix.
struct CompositionState {
    std::size_t confirmed_bytes = 0;
    bool stopped = false;
    CompositionStop stop = CompositionStop::complete;
    // Meaningful only when stop == primitive_error.
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

// A reference IoError projection of a stopped composition, for a consumer that
// must return an `IoError`. The composition authority is the state itself —
// `stop`, the accumulated `confirmed_bytes` and the primitive's own error —
// which `compose_progress`/`compose_error` decide; this is one representation
// of it, not a second rule. `primitive_error` keeps the primitive's own error;
// the two no-progress stops report distinct values so they stay
// distinguishable. The no-progress write stop reuses `invalid_state` because no
// canonical category exists for it. The direct surface publishes stop reasons
// structurally and does not call this projection.
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

// ─── Effects and partial progress ───────────────────────────────────────────

// Whether a possibly-effective portion of the operation is unaccounted for.
enum class EffectCertainty : std::uint8_t {
    // `confirmed_bytes` accounts for the whole operation.
    accounted,
    // A possibly-effective portion has no trustworthy count and must be
    // treated as possibly applied. No error implies rollback.
    unknown,
};

struct IoEffect {
    std::uint64_t confirmed_bytes = 0;
    EffectCertainty remaining = EffectCertainty::accounted;

    friend bool operator==(const IoEffect&, const IoEffect&) noexcept = default;
};

// Bounded terminal outcome of one logical operation: confirmed progress, a
// terminal reason, and the certainty of what is left over, reported together.
struct IoOutcome {
    bool succeeded = false;
    IoEffect effect{};
    IoError error{};

    static constexpr IoOutcome success(std::uint64_t confirmed_bytes = 0) noexcept {
        return IoOutcome{true, IoEffect{confirmed_bytes, EffectCertainty::accounted}, IoError{}};
    }

    // `accounted` on a failure is an evidence claim: it needs positive proof
    // that nothing beyond `confirmed_bytes` took effect, such as a cancel that
    // won before dispatch. The operation's direction never provides that proof.
    static constexpr IoOutcome failure(IoError reason,
                                       std::uint64_t confirmed_bytes = 0) noexcept {
        return IoOutcome{false, IoEffect{confirmed_bytes, EffectCertainty::accounted}, reason};
    }

    // A failure whose possibly-effective portion has no trustworthy count;
    // `known_prefix` stays a lower bound from earlier completed steps.
    static constexpr IoOutcome uncertain(IoError reason,
                                         std::uint64_t known_prefix = 0) noexcept {
        return IoOutcome{false, IoEffect{known_prefix, EffectCertainty::unknown}, reason};
    }

    constexpr bool is_canceled() const noexcept {
        return !succeeded && error.code == IoError::Code::canceled;
    }

    friend bool operator==(const IoOutcome&, const IoOutcome&) noexcept = default;
};

// Names the terminal shapes that cannot carry an unaccounted remainder: a
// terminal shaped only as {is_error, error, confirmed_bytes} has no field for
// effect certainty.
constexpr bool prefix_only_terminal_can_carry(const IoOutcome& outcome) noexcept {
    return outcome.effect.remaining == EffectCertainty::accounted;
}

// A failed attempt that dispatched may already have taken effect — reached the
// file, or filled part of the caller's destination buffer — and supplies no
// trustworthy count for the possibly-effective portion, so its remainder is
// unknown whatever the operation reads or writes. Scope: file and
// borrowed-buffer effects; the cursor position after a failed shared-cursor
// call is not modeled.
constexpr IoOutcome failed_dispatched_attempt(IoError reason,
                                              std::uint64_t known_prefix = 0) noexcept {
    return IoOutcome::uncertain(reason, known_prefix);
}

// A cancel that won before dispatch proved no dispatch and no effect, so the
// remainder is accounted; `confirmed_prefix` carries earlier completed steps.
constexpr IoOutcome canceled_before_dispatch(std::uint64_t confirmed_prefix = 0) noexcept {
    return IoOutcome::failure(IoError{.code = IoError::Code::canceled}, confirmed_prefix);
}

// A cancel racing an in-flight attempt keeps the trusted count instead of
// collapsing into an unqualified canceled result; the attempt's own remainder
// stays unknown.
constexpr IoOutcome canceled_racing_in_flight_attempt(std::uint64_t confirmed_bytes) noexcept {
    return IoOutcome::uncertain(IoError{.code = IoError::Code::canceled}, confirmed_bytes);
}

// Certainty of a stopped composition's remainder. Only a stop whose primitive
// reported a successful count accounts for it: a primitive error is not a
// trustworthy count for the attempt's own effect — a failing read may already
// have filled part of the destination buffer, a failing write may already have
// reached the file — and an impossible count is untrustworthy by definition.
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

// ─── Durability reference rules (Linux regular-file profile) ───────────────

enum class MutationKind : std::uint8_t {
    write,
    resize_shrink,
    resize_grow,
    metadata, // permissions/ownership/timestamps; covered only by sync_all
};

enum class CompletionState : std::uint8_t {
    submitted, // initiation/acceptance happened; no acquired terminal yet
    observed,  // direct completion or acquired public terminal; ordered before later sync
};

enum class SyncKind : std::uint8_t {
    data,
    all,
};

struct MutationRecord {
    MutationKind kind = MutationKind::write;
    CompletionState completion = CompletionState::submitted;
    // Ordering stand-in for "completion happens-before sync initiation". A real
    // system orders these by publication, not by a shared counter.
    std::uint64_t completion_sequence = 0;
};

// Sequence numbers are strictly ordered: a mutation is ordered before a sync
// only when its number is strictly smaller. Equal numbers are unordered and
// support no durability claim in either direction.

struct SyncRecord {
    SyncKind kind = SyncKind::data;
    bool succeeded = false;
    // Acceptance point for a request sync; operation initiation after
    // validation for a direct sync.
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
        // A completed file-size mutation is covered without a covered write
        // (Linux fdatasync covers the length metadata), in both directions.
        return true;
    case MutationKind::metadata:
        return sync.kind == SyncKind::all;
    }
    return false;
}

// Named negative rule: no mutation, a completed resize included, is durable
// without a covering sync.
constexpr bool grants_durability_alone(const MutationRecord&) noexcept {
    return false;
}

// Ordering fact only: observed strictly after the sync's initiation. No
// durability claim follows by itself.
constexpr bool ordered_after_sync(const SyncRecord& sync, const MutationRecord& mutation) noexcept {
    return mutation.completion == CompletionState::observed &&
           mutation.completion_sequence > sync.initiation_sequence;
}

// Negative rule: a successful sync never guarantees that the exact state it
// covered survives a later conflicting mutation.
constexpr bool preserves_exact_state(const SyncRecord&, const MutationRecord&) noexcept {
    return false;
}

} // namespace sluice::detail
