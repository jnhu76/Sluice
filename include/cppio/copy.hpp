// cppio copy_all — the stream-to/copy primitive.
// Loops read -> write_all until EOF, error, or the CopyLimit is exhausted.
#pragma once

#include <cppio/limit.hpp>
#include <cppio/measurement.hpp>
#include <cppio/reader.hpp>
#include <cppio/writer.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace cppio {

// Primary overload: bounded copy with caller-provided scratch and an explicit
// limit. CopyLimit::unlimited() reproduces the original copy-to-EOF behavior;
// CopyLimit::bytes(n) copies at most n; CopyLimit::nothing() copies nothing and
// touches neither reader nor writer. Empty scratch with a non-zero/unlimited
// limit is invalid_state; nothing() tolerates an empty scratch. If `stats` is
// non-null, copy loop / byte / stop-reason counters are recorded there.
Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, std::span<std::byte> scratch,
                               CopyLimit limit, CopyStats* stats = nullptr);

// Back-compat overload: copies until EOF or error. Delegates to unlimited().
Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, std::span<std::byte> scratch);

// Convenience overload: bounded copy with an internally allocated scratch.
Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, CopyLimit limit);

// Convenience overload: unbounded copy with an internally allocated scratch.
Result<std::uint64_t> copy_all(Reader& reader, Writer& writer);

}  // namespace cppio
