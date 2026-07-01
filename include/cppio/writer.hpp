// cppio::Writer — abstraction over a byte sink, inspired by Zig std.Io.Writer.
//
// Concrete writers implement the two primitives write_some/flush. write_all is
// a derived operation that loops over short writes and rejects zero progress.
#pragma once

#include <cppio/result.hpp>

#include <cstddef>
#include <span>

namespace cppio {

class Writer {
public:
    virtual ~Writer() = default;

    // Primitive: write up to src.size() bytes. May return fewer (short write).
    // Returns 0 only when src is empty; a 0 on non-empty input is treated by
    // write_all as a backend failure (invalid_state).
    virtual Result<std::size_t> write_some(std::span<const std::byte> src) = 0;

    // Primitive: push any user-space buffered state to the underlying sink.
    virtual Result<void> flush() = 0;

    // Derived: retry write_some until all of src is written or an error occurs.
    Result<void> write_all(std::span<const std::byte> src);
};

}  // namespace cppio
