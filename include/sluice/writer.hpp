// sluice::Writer — abstraction over a byte sink, inspired by Zig std.Io.Writer.
//
// Concrete writers implement the two primitives write_some/flush. write_all is
// a derived operation that loops over short writes and rejects zero progress.
#pragma once

#include <sluice/iovec.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <span>

namespace sluice {

class Writer {
public:
    virtual ~Writer() = default;

    // Primitive: write up to src.size() bytes. May return fewer (short write).
    // Returns 0 only when src is empty; a 0 on non-empty input is treated by
    // write_all as a backend failure (invalid_state).
    virtual Result<std::size_t> write_some(std::span<const std::byte> src) = 0;

    // Primitive: push any user-space buffered state to the underlying sink.
    // Layer-specific contract: BufferedWriter drains dirty bytes then calls
    // inner.flush(); FileWriter::flush() is a documented no-op (no fsync) this
    // phase. See README "Flush contract". Not the same as a durable commit.
    virtual Result<void> flush() = 0;

    // Derived: retry write_some until all of src is written or an error occurs.
    Result<void> write_all(std::span<const std::byte> src);

    // Vector primitive (overridable): write from srcs in order, skipping empty
    // slices. Returns total bytes written. Stops after the first short write or
    // on the first error (which is propagated immediately, even after partial
    // progress — mirrors write_all's "errors returned, not swallowed"). A
    // concrete writer (FileWriter) may override this to issue a single writev.
    // Default fallback loops over write_some so every Writer works out of the
    // box. See docs/readv-writev-design-note.md.
    virtual Result<std::size_t> write_vec(std::span<const ConstIoSlice> srcs);

    // Vector derived: write every byte of every (non-empty) slice, retrying
    // across short writes; reject zero progress on non-empty remaining input as
    // invalid_state. Bytes are written in order with no skip or duplication.
    Result<void> write_all_vec(std::span<const ConstIoSlice> srcs);
};

}  // namespace sluice
