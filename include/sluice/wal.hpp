// sluice::wal — minimal write-ahead log record format for testing writer
// semantics. Not a database. Record layout (little-endian):
//
//   magic:    u32   (fixed sentinel)
//   length:   u32   (payload byte count)
//   payload:  bytes (length bytes)
//   checksum: u32   (sum of payload bytes mod 2^32)
//
// Trust/resource contract: this format is for application-owned local records
// produced by write_record/write_record_vec. read_record validates framing and
// checksum but is not an untrusted-file parser and intentionally has no record
// size policy below the on-disk u32 limit. It grows the payload incrementally so
// a corrupt short file cannot trigger one allocation from its declared length;
// a genuinely present multi-gigabyte record can still consume corresponding
// memory. Callers accepting external or partially trusted input must impose
// their own byte/record limit before using this API.
#pragma once

#include <sluice/reader.hpp>
#include <sluice/sync.hpp>
#include <sluice/writer.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sluice::wal {

inline constexpr std::uint32_t magic = 0x57414CU; // "WAL"

// Encode and write one record through writer (uses write_all).
Result<void> write_record(Writer& writer, std::span<const std::byte> payload);

// Encode and write one record through writer using the vector path: emits
// header | payload | checksum as a single write_all_vec({header,payload,checksum}).
// Produces byte-identical output to write_record; round-trips with read_record.
// Same payload overflow guard as write_record. See docs/reference/sync-io-model.md
// (Vector I/O semantics).
Result<void> write_record_vec(Writer& writer, std::span<const std::byte> payload);

// Read and validate one record. Clean EOF at record start -> error::eof.
// Truncated stream -> error::eof. Bad checksum -> error::invalid_state.
Result<std::vector<std::byte>> read_record(Reader& reader);

// Minimal WAL durability wrapper. Tracks three LSNs with the
// invariant durable_lsn <= flushed_lsn <= written_lsn. This is NOT group commit
// — each sync() is a single-writer barrier. The record format is unchanged;
// WalWriter just frames write_record/write_record_vec through its inner writer
// and advances LSNs on successful flush/sync. See
// docs/architecture/sync-durability-model.md.
class WalWriter {
  public:
    // Without a SyncableWriter, sync() returns invalid_state (nothing to sync).
    explicit WalWriter(Writer& writer);
    // `syncable` is the optional sync capability of the underlying sink (e.g. a
    // FileWriter). If provided, sync() flushes then calls sync_data().
    WalWriter(Writer& writer, SyncableWriter* syncable);

    // Frame and write one record (scalar path). Advances written_lsn on success.
    Result<void> write_record(std::span<const std::byte> payload);
    // Frame and write one record (vector path). Advances written_lsn on success.
    Result<void> write_record_vec(std::span<const std::byte> payload);

    // Drain the inner writer; on success flushed_lsn advances to written_lsn.
    Result<void> flush();

    // flush() then (if SyncableWriter present) sync_data(); on success
    // durable_lsn advances to flushed_lsn. Without a SyncableWriter returns
    // invalid_state.
    Result<void> sync();

    std::uint64_t written_lsn() const noexcept { return written_lsn_; }
    std::uint64_t flushed_lsn() const noexcept { return flushed_lsn_; }
    std::uint64_t durable_lsn() const noexcept { return durable_lsn_; }

  private:
    Writer& writer_;
    SyncableWriter* syncable_;
    std::uint64_t written_lsn_ = 0;
    std::uint64_t flushed_lsn_ = 0;
    std::uint64_t durable_lsn_ = 0;
};

namespace detail {

// Checks a payload length fits in the u32 length field. Returns the length on
// success or invalid_state if it would truncate. Extracted so the overflow
// check is testable without allocating a 4 GiB payload.
Result<std::uint32_t> checked_u32_len(std::size_t len);

// Bounded growth step used by read_record. This is not a record-format limit:
// it prevents a corrupt length header from causing one up-front allocation,
// while preserving every u32-representable payload length.
std::size_t read_chunk_size(std::size_t remaining) noexcept;

} // namespace detail

} // namespace sluice::wal
