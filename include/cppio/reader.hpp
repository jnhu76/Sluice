// cppio::Reader — abstraction over a byte source, inspired by Zig std.Io.Reader.
//
// Concrete readers implement read_some. read_exact and stream_to are derived
// operations defined in terms of read_some and a Writer.
#pragma once

#include <cppio/limit.hpp>
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
    // Delegates to copy_all — no duplicated copy logic.
    Result<std::uint64_t> stream_to(Writer& writer, std::span<std::byte> scratch, CopyLimit limit);

    // Derived: bounded copy with an internally allocated scratch.
    Result<std::uint64_t> stream_to(Writer& writer, CopyLimit limit);
};

}  // namespace cppio
