// cppio copy_all — the stream-to/copy primitive.
// Loops read -> write_all until EOF or error, returning total bytes copied.
#pragma once

#include <cppio/reader.hpp>
#include <cppio/writer.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace cppio {

// Copies reader -> writer using the caller-provided scratch buffer. No internal
// allocation. Returns total bytes copied on clean EOF; propagates any error.
Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, std::span<std::byte> scratch);

// Convenience overload: allocates a small internal scratch buffer.
Result<std::uint64_t> copy_all(Reader& reader, Writer& writer);

}  // namespace cppio
