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

// Encode and write one record through writer using the vector path: emits
// header | payload | checksum as a single write_all_vec({header,payload,checksum}).
// Produces byte-identical output to write_record; round-trips with read_record.
// Same payload overflow guard as write_record. See docs/readv-writev-design-note.md.
Result<void> write_record_vec(Writer& writer, std::span<const std::byte> payload);

// Read and validate one record. Clean EOF at record start -> error::eof.
// Truncated stream -> error::eof. Bad checksum -> error::invalid_state.
Result<std::vector<std::byte>> read_record(Reader& reader);

namespace detail {

// Checks a payload length fits in the u32 length field. Returns the length on
// success or invalid_state if it would truncate. Extracted so the overflow
// check is testable without allocating a 4 GiB payload.
Result<std::uint32_t> checked_u32_len(std::size_t len);

}  // namespace detail

}  // namespace cppio::wal
