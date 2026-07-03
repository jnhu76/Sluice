// sluice copy_all — the stream-to/copy primitive.
// Loops read -> write_all until EOF, error, or the CopyLimit is exhausted.
#pragma once

#include <sluice/copy_strategy.hpp>
#include <sluice/limit.hpp>
#include <sluice/measurement.hpp>
#include <sluice/reader.hpp>
#include <sluice/writer.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace sluice {

// Strategy-aware primary overload (CPPIO-CORE-007C): selects the copy path from
// options.strategy, respects options.limit, and fills *decision (if non-null)
// with requested vs selected and which path moved bytes. Auto currently behaves
// as BufferedFirst (006). Scratch forces the scratch read/write loop and never
// uses the buffered fast path. Deferred strategies are handled in 007E. If
// `stats` is non-null, copy loop / byte / stop-reason counters are recorded.
Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, std::span<std::byte> scratch,
                               CopyOptions options, CopyStats* stats = nullptr,
                               CopyDecision* decision = nullptr);

// Bounded copy with caller-provided scratch and an explicit limit. Delegates to
// the strategy overload with CopyOptions{limit, CopyStrategy::Auto}. Preserves
// the pre-007 behavior (Auto == BufferedFirst, so a BufferedReadable reader
// still gets the fast path).
Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, std::span<std::byte> scratch,
                               CopyLimit limit, CopyStats* stats = nullptr);

// Back-compat overload: copies until EOF or error. Delegates to unlimited().
Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, std::span<std::byte> scratch);

// Convenience overload: bounded copy with an internally allocated scratch.
Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, CopyLimit limit);

// Convenience overload: unbounded copy with an internally allocated scratch.
Result<std::uint64_t> copy_all(Reader& reader, Writer& writer);

}  // namespace sluice
