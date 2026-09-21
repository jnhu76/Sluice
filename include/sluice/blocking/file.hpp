#pragma once

#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// Direct (completed-return) File operations: each call executes immediately on
// the caller thread and returns its completed outcome, with no request slot,
// context, backend or progress source involved (INV-01). The shared File
// semantic rules in `detail/file_semantics.hpp` govern every operation here.

namespace sluice::blocking {

// ─── Primitives (SEM-05) ───────────────────────────────────────────────────

// A primitive reports the count the native call returned: a short count is a
// successful partial transfer, and a nonempty read returning 0 is EOF at the
// observed position, not an error. Exact/all promises belong to the
// composition helpers below.

Result<std::size_t> read_at(const File& file, std::uint64_t offset, std::span<std::byte> dst);

Result<std::size_t> write_at(const File& file, std::uint64_t offset,
                             std::span<const std::byte> src);

// Shared-cursor forms: the kernel's open-file-description offset, shared
// through native duplication, is the cursor. It is never lowered to an internal
// offset plus positional I/O (SEM-04).

Result<std::size_t> read(const File& file, std::span<std::byte> dst);

Result<std::size_t> write(const File& file, std::span<const std::byte> src);

// ─── Durability, metadata and size (SEM-06, SEM-07) ────────────────────────

Result<void> sync_data(const File& file);

Result<void> sync_all(const File& file);

Result<FileInfo> file_info(const File& file);

// The size projection of one metadata observation: no second metadata rule, and
// no snapshot across separate calls (SEM-07).
Result<std::uint64_t> size(const File& file);

Result<void> resize(const File& file, std::uint64_t new_size);

// ─── Exact/all composition (SEM-05, ERR-02) ────────────────────────────────

// Why a composition stopped. `eof_before_full` and `write_no_progress` stay
// distinguishable, and neither is collapsed into a native-error category: the
// root names a "no-progress failure" without assigning it a canonical IoError.
enum class CompositionEnd : std::uint8_t {
    complete,
    eof_before_full,
    write_no_progress,
    primitive_error,
};

// The completed outcome of an exact/all composition. A stopped composition
// reports its accumulated confirmed bytes next to the reason, so a failure
// never discards the prefix (ERR-02).
//
// A failed attempt contributes no confirmed bytes, and on this path the stop's
// remainder is accounted rather than unknown: a direct Linux regular-file
// primitive reports an attempt that transferred bytes as a positive short count,
// so a rejected attempt transferred no bytes — not into the file, and not into
// the caller's buffer. ERR-02 reserves `unknown` for an attempt that may have
// taken effect without a trustworthy count, which needs an attempt that can be
// canceled after dispatch; only the request paths have one. The premise and its
// boundary are recorded in the A2 review record of the conformance ledger.
struct CompositionOutcome {
    std::size_t confirmed_bytes = 0;
    CompositionEnd end = CompositionEnd::complete;
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
// the Result error channel, before any byte moves; the value reports the
// operation outcome, including a primitive error that happened after confirmed
// progress.

Result<CompositionOutcome> read_exact_at(const File& file, std::uint64_t offset,
                                         std::span<std::byte> dst);

Result<CompositionOutcome> write_all_at(const File& file, std::uint64_t offset,
                                        std::span<const std::byte> src);

Result<CompositionOutcome> read_exact(const File& file, std::span<std::byte> dst);

Result<CompositionOutcome> write_all(const File& file, std::span<const std::byte> src);

} // namespace sluice::blocking
