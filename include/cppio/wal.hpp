// cppio::wal — minimal write-ahead log record format for testing writer
// semantics. Not a database. Record layout (little-endian):
//
//   magic:    u32   (fixed sentinel)
//   length:   u32   (payload byte count)
//   payload:  bytes (length bytes)
//   checksum: u32   (sum of payload bytes mod 2^32)
#pragma once

#include <cppio/reader.hpp>
#include <cppio/writer.hpp>
#include <cppio/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cppio::wal {

inline constexpr std::uint32_t magic = 0x57414CU;  // "WAL"

// Encode and write one record through writer (uses write_all).
Result<void> write_record(Writer& writer, std::span<const std::byte> payload);

// Read and validate one record. Clean EOF at record start -> error::eof.
// Truncated stream -> error::eof. Bad checksum -> error::invalid_state.
Result<std::vector<std::byte>> read_record(Reader& reader);

}  // namespace cppio::wal
