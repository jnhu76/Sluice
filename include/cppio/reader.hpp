// cppio::Reader — abstraction over a byte source, inspired by Zig std.Io.Reader.
//
// Concrete readers implement read_some. read_exact and stream_to are derived
// operations defined in terms of read_some and a Writer.
#pragma once

#include <cppio/iovec.hpp>
#include <cppio/limit.hpp>
#include <cppio/measurement.hpp>
#include <cppio/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace cppio {

class Writer;

class Reader {
public:
    virtual ~Reader() = default;

    // Primitive: read up to dst.size() bytes into dst. Returns the number read.
    // A return of 0 with no error means EOF. Errors are returned, not thrown.
    virtual Result<std::size_t> read_some(std::span<std::byte> dst) = 0;

    // Derived: read exactly dst.size() bytes, or fail (EOF/error) if fewer
    // remain. dst.size()==0 is an immediate success.
    Result<void> read_exact(std::span<std::byte> dst);

    // Derived: copy bytes from this reader to writer until EOF or error.
    // Returns total bytes transferred. Unbounded; delegates to copy_all with
    // CopyLimit::unlimited().
    Result<std::size_t> stream_to(Writer& writer);

    // Derived: bounded copy with an explicit limit and caller-provided scratch.
    // Delegates to copy_all — no duplicated copy logic. If `stats` is non-null
    // it is forwarded to copy_all for measurement.
    Result<std::uint64_t> stream_to(Writer& writer, std::span<std::byte> scratch, CopyLimit limit,
                                    CopyStats* stats = nullptr);

    // Derived: bounded copy with an internally allocated scratch.
    Result<std::uint64_t> stream_to(Writer& writer, CopyLimit limit);

    // Vector primitive (overridable): read into dsts in order, skipping empty
    // slices. Returns total bytes read. A conservative readv-style primitive:
    // it STOPS as soon as a slice is not fully satisfied — i.e. on a clean EOF
    // (n==0) OR on a positive short read (0 < n < slice size). An error is
    // propagated immediately, even after partial progress (mirrors read_exact).
    // This is NOT read_exact over slices; a future read_exact_vec would keep
    // filling the same slice on a short read. A concrete reader (FileReader)
    // overrides this with a single readv, exposing the same stop-on-short
    // semantics. EOF before any progress returns 0. See
    // docs/readv-writev-design-note.md.
    virtual Result<std::size_t> read_vec(std::span<IoSlice> dsts);

    // Exact-read over multiple slices (CPPIO-CORE-015A): fill every byte of every
    // non-empty slice, retrying the SAME slice on a short read (unlike read_vec,
    // which stops on a short read). Symmetric to Writer::write_all_vec. Empty
    // slices are skipped. Returns void on success. On EOF before completion
    // returns IoError::eof; other read errors propagate immediately, even after
    // partial progress (matching read_exact). Naming note: the writer side is
    // write_all_vec; this is read_vec_all per the task spec.
    Result<void> read_vec_all(std::span<IoSlice> dsts);
};

}  // namespace cppio
