#pragma once

// Shared File semantic oracle: the single authority for the v1 File rules that
// direct execution, ThreadPool and io_uring must all obey. The validation rules
// (open legality, access matrix, precedence, range) and the error mapping are
// called by every execution path, so one code change moves them together. The
// composition, effect and durability rules are the reference model those paths
// are required to conform to; the paths that must publish them are named in the
// conformance ledger and not all of them consume the rules yet.
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
    // No buffer-presence field on purpose. SEM-03 treats caller preconditions
    // such as valid memory as not generally dynamically detectable and defines
    // no buffer-presence step, so buffer presence is not a shared File semantic
    // rule. A raw-pointer surface may still fail fast on a null buffer with a
    // nonzero length as its own implementation precondition; that check lives
    // beside the surface's pointers, not here.
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

// ─── Primitive outcome and composition (SEM-05) ─────────────────────────────

// Classification of a *successful* primitive byte transfer. An OS error is a
// separate outcome: it is never turned into a byte count here.
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
// successful steps and are never discarded by a later stop, so a failure reports
// its prefix separately from its reason (ERR-02).
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

// The canonical error a stopped composition reports. `primitive_error` keeps the
// primitive's own error; the two no-progress stops report distinct values so
// EOF-before-full and write-no-progress stay distinguishable (SEM-05). A
// complete composition reports nothing.
//
// The root names a "no-progress failure" without assigning it a canonical
// category, so the closest existing category is used and no new category is
// introduced; the ambiguity is recorded in the ledger for the A1 slice.
//
// The direct surface publishes the same stop reasons structurally instead and
// carries an `IoError` only for a primitive failure, so it deliberately does not
// call this rule. The two mappings are pinned side by side, from one injected
// primitive sequence, by `every_stop_reason_is_pinned_against_the_oracle_error_rule`
// in `tests/direct_composition_fault_test.cpp`.
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

// ─── Effects and partial progress (ERR-02) ─────────────────────────────────

// Whether a possibly-effective portion of the operation is unaccounted for.
enum class EffectCertainty : std::uint8_t {
    // `confirmed_bytes` accounts for the whole operation: every remaining byte
    // is either confirmed or proven unaffected.
    accounted,
    // A possibly-effective portion has no trustworthy count and must be treated
    // as possibly applied. No error implies rollback.
    unknown,
};

struct IoEffect {
    std::uint64_t confirmed_bytes = 0;
    EffectCertainty remaining = EffectCertainty::accounted;

    friend bool operator==(const IoEffect&, const IoEffect&) noexcept = default;
};

// Bounded terminal outcome of one logical operation. It carries the three
// required reports at once: a confirmed progress count, a terminal reason, and
// the certainty of what is left over. Cancellation travels through the same
// failure channel as an error, so a confirmed count racing a cancel is never
// erased into an unqualified canceled result.
struct IoOutcome {
    bool succeeded = false;
    IoEffect effect{};
    IoError error{};

    // A successful primitive count is exact progress; a scalar or void operation
    // reports success without inventing a byte count.
    static constexpr IoOutcome success(std::uint64_t confirmed_bytes = 0) noexcept {
        return IoOutcome{true, IoEffect{confirmed_bytes, EffectCertainty::accounted}, IoError{}};
    }

    // A failure or cancellation whose remainder is *proven* unaffected. That is
    // an evidence claim, not a default: it needs a positive proof that nothing
    // beyond `confirmed_bytes` took effect, such as a cancel that won before
    // dispatch. An operation's direction never provides that proof — a failed
    // read may already have filled part of the caller's destination buffer,
    // which LIFE-01 makes part of the operation's borrow.
    static constexpr IoOutcome failure(IoError reason,
                                       std::uint64_t confirmed_bytes = 0) noexcept {
        return IoOutcome{false, IoEffect{confirmed_bytes, EffectCertainty::accounted}, reason};
    }

    // A failure or cancellation where a possibly-effective portion has no
    // trustworthy count. `known_prefix` stays a lower bound from earlier
    // completed steps. This is the required shape for a dispatched attempt
    // that failed or was canceled, per `failed_dispatched_attempt`.
    static constexpr IoOutcome uncertain(IoError reason,
                                         std::uint64_t known_prefix = 0) noexcept {
        return IoOutcome{false, IoEffect{known_prefix, EffectCertainty::unknown}, reason};
    }

    constexpr bool is_canceled() const noexcept {
        return !succeeded && error.code == IoError::Code::canceled;
    }

    friend bool operator==(const IoOutcome&, const IoOutcome&) noexcept = default;
};

// ERR-02 requires the reason, the confirmed prefix and the unaccounted remainder
// to be reportable together. A terminal shaped only as
// {is_error, error, confirmed_bytes} has no field for effect certainty, so this
// predicate names exactly which outcomes that shape cannot carry; the A1 ledger
// records the request-path storage that still has the narrower shape.
constexpr bool prefix_only_terminal_can_carry(const IoOutcome& outcome) noexcept {
    return outcome.effect.remaining == EffectCertainty::accounted;
}

// Conversion rule for a failed physical attempt that dispatched. Dispatch
// evidence, not direction, decides the remainder: the attempt may already have
// taken effect — reached the file, or filled part of the caller's destination
// buffer — and supplies no trustworthy count for the possibly-effective
// portion, so its remainder is unaccounted rather than zero (ERR-02, V15),
// whatever the operation reads or writes. `known_prefix` stays a lower bound
// from earlier completed steps.
//
// Scope: this covers the operation's effects on the file and on the borrowed
// buffers. It does not model the shared-cursor position after a failed
// shared-cursor call, which v1 does not promise.
constexpr IoOutcome failed_dispatched_attempt(IoError reason,
                                              std::uint64_t known_prefix = 0) noexcept {
    return IoOutcome::uncertain(reason, known_prefix);
}

// CANCEL-01: a cancel that won before dispatch proved no dispatch and no
// effect, so the remainder is accounted. `confirmed_prefix` carries earlier
// completed composition steps; the canceled attempt itself contributed nothing.
constexpr IoOutcome canceled_before_dispatch(std::uint64_t confirmed_prefix = 0) noexcept {
    return IoOutcome::failure(IoError{.code = IoError::Code::canceled}, confirmed_prefix);
}

// CANCEL-01/ERR-02: a cancel racing an in-flight attempt keeps the trusted
// count instead of collapsing it into an unqualified canceled result, but the
// attempt's remainder cannot be proven unaffected, so it stays unknown rather
// than accounted.
constexpr IoOutcome canceled_racing_in_flight_attempt(std::uint64_t confirmed_bytes) noexcept {
    return IoOutcome::uncertain(IoError{.code = IoError::Code::canceled}, confirmed_bytes);
}

// ─── Durability reference rules (SEM-06, Linux regular-file profile) ───────

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
// only when its number is strictly smaller. Equal numbers mean the model cannot
// order the two events, and an unordered pair supports no durability claim.

struct SyncRecord {
    SyncKind kind = SyncKind::data;
    bool succeeded = false;
    // For a request sync this is the acceptance point; for a direct sync it is
    // the call's operation initiation after validation.
    std::uint64_t initiation_sequence = 0;
};

// Coverage is per mutation and per completion: a mutation only submitted before
// the sync was initiated is not covered, which is the V16 rule.
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
        // v1-r3 Linux regular-file profile: a completed file-size mutation is
        // covered even without a covered write, in both directions.
        return true;
    case MutationKind::metadata:
        return sync.kind == SyncKind::all;
    }
    return false;
}

// A mutation is never durable on its own: only a successful sync that covers it
// establishes durability. A completed resize in particular grants none.
constexpr bool grants_durability_alone(const MutationRecord&) noexcept {
    return false;
}

// An ordering fact only: this mutation is observed strictly after the sync's
// initiation. It carries no durability claim by itself, and an unordered pair
// (equal sequence numbers) supports none either. Whether the later mutation
// actually conflicts with the state the sync covered is not a property of this
// record, so supersession is derived where both halves are known: the V17
// reference case pairs an earlier covered mutation with a conflicting one
// ordered after, which is the pair SEM-06 names as superseding.
constexpr bool ordered_after_sync(const SyncRecord& sync, const MutationRecord& mutation) noexcept {
    return mutation.completion == CompletionState::observed &&
           mutation.completion_sequence > sync.initiation_sequence;
}

// The V17 negative rule: a successful sync never guarantees that the exact state
// it covered survives a later conflicting mutation.
constexpr bool preserves_exact_state(const SyncRecord&, const MutationRecord&) noexcept {
    return false;
}

} // namespace sluice::detail
