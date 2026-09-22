#pragma once

#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// Direct (completed-return) File operations: each call executes immediately on
// the caller thread and returns its completed outcome. The shared File
// semantic rules in `detail/file_semantics.hpp` govern every operation here.

namespace sluice::blocking {

// ─── Primitives ────────────────────────────────────────────────────────────

// A primitive reports the native count only: a short count is successful
// partial progress and a nonempty read returning 0 is EOF. Exact/all promises
// belong to the composition helpers below.

Result<std::size_t> read_at(const File& file, std::uint64_t offset, std::span<std::byte> dst);

Result<std::size_t> write_at(const File& file, std::uint64_t offset,
                             std::span<const std::byte> src);

// The cursor is the kernel's open-file-description offset, shared through
// native duplication; it is never lowered to an internal offset plus
// positional I/O.

Result<std::size_t> read(const File& file, std::span<std::byte> dst);

Result<std::size_t> write(const File& file, std::span<const std::byte> src);

// ─── Durability, metadata and size ─────────────────────────────────────────

Result<void> sync_data(const File& file);

Result<void> sync_all(const File& file);

Result<FileInfo> file_info(const File& file);

// The size projection of one metadata observation; no snapshot across
// separate calls.
Result<std::uint64_t> size(const File& file);

Result<void> resize(const File& file, std::uint64_t new_size);

// ─── Exact/all composition ─────────────────────────────────────────────────

// Why a composition stopped. `eof_before_full` and `write_no_progress` stay
// distinguishable from each other and from native-error outcomes.
enum class CompositionEnd : std::uint8_t {
    complete,
    eof_before_full,
    write_no_progress,
    primitive_error,
};

// Whether a possibly-effective portion of a failed attempt is unaccounted for.
enum class EffectCertainty : std::uint8_t {
    // `confirmed_bytes` accounts for the whole operation.
    accounted,
    // The unconfirmed remainder must be treated as possibly applied; no error
    // implies rollback.
    unknown,
};

// The completed outcome of an exact/all composition: confirmed bytes reported
// next to the stop reason, so a failure never discards its prefix. On Linux a
// primitive error is not a trustworthy count for its own attempt's effect, so
// `remaining` reports that distinction instead of leaving the caller to infer
// it from the direction.
struct CompositionOutcome {
    std::size_t confirmed_bytes = 0;
    CompositionEnd end = CompositionEnd::complete;
    // Certainty of what is left over after `confirmed_bytes`. A primitive error
    // and an impossible count report `unknown`; a completed, EOF-before-full or
    // no-progress stop reports `accounted`.
    EffectCertainty remaining = EffectCertainty::accounted;
    // Present exactly when `end` is `primitive_error`. That reason is either the
    // primitive's own error with its native detail preserved, or the shared
    // rule's `invalid_state` for a count that cannot come from a primitive
    // honoring its contract, which carries no native detail. A successful or
    // no-progress outcome carries no error, so a consumer cannot read a stale
    // category.
    std::optional<IoError> error;

    constexpr bool complete() const noexcept { return end == CompositionEnd::complete; }
};

// Semantic rejections (closed File, illegal access, invalid range) travel in
// the Result error channel before any byte moves; a primitive error after
// confirmed progress is reported in the value.

Result<CompositionOutcome> read_exact_at(const File& file, std::uint64_t offset,
                                         std::span<std::byte> dst);

Result<CompositionOutcome> write_all_at(const File& file, std::uint64_t offset,
                                        std::span<const std::byte> src);

Result<CompositionOutcome> read_exact(const File& file, std::span<std::byte> dst);

Result<CompositionOutcome> write_all(const File& file, std::span<const std::byte> src);

} // namespace sluice::blocking
