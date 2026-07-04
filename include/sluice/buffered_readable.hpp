// sluice::BufferedReadable — opt-in capability interface exposing the already
// buffered, unread bytes of a Reader. This is the seam that copy_all's buffered
// fast path (CPPIO-CORE-006D) uses to drain buffered bytes before falling back
// to the scratch read path, mirroring Zig std.Io's Reader.stream which first
// writes r.buffer[r.seek..r.end] (Reader.zig:168).
//
// It is a separate mixin interface (not a virtual hook on the Reader base
// class) so that Readers with no internal buffer (FileReader, MemoryReader,
// FaultReader, ...) carry zero overhead and no dead virtuals. copy_all detects
// the capability with a dynamic_cast, which is cheap and keeps the core Reader
// abstraction minimal. See docs/buffered-fast-path.md.
#pragma once

#include <sluice/result.hpp>

#include <cstddef>
#include <span>

namespace sluice {

class BufferedReadable {
  public:
    virtual ~BufferedReadable() = default;

    // Returns the currently buffered, unread bytes. Does NOT call the inner
    // reader, does NOT mutate state, and may return an empty span. The returned
    // span is only valid until the next mutating operation on the reader.
    virtual std::span<const std::byte> peek_buffered() const = 0;

    // Consumes exactly n bytes from the buffered unread region. Returns
    // invalid_state if n > peek_buffered().size(). Does NOT call the inner
    // reader and never consumes more than requested.
    virtual Result<void> consume_buffered(std::size_t n) = 0;
};

} // namespace sluice
