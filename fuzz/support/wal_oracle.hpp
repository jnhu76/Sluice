// Independent test-only WAL record decoder.
//
// This oracle re-implements the WAL frame layout from scratch — it does NOT call
// any production WAL helper. Its job is to answer, for an arbitrary byte stream,
// "does a well-formed record start here, and what are its fields?" so the fuzz
// target can compare production's read_record() answer against an independent
// expectation.
//
// Layout (little-endian), mirrored from docs and wal.hpp:
//   magic:    u32   (== sluice::wal::magic)
//   length:   u32   (payload byte count)
//   payload:  bytes (length bytes)
//   checksum: u32   (sum of payload bytes mod 2^32)
//
// The oracle is total: decode() never throws or reads out of bounds. If the
// stream is too short for a complete record, decoded == false and the fields
// reflect only what was present.
#pragma once

#include <sluice/wal.hpp> // for sluice::wal::magic sentinel only

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fuzz {

// Independent checksum: sum of payload bytes modulo 2^32. Mirrors production's
// checksum_of() without calling it.
inline std::uint32_t oracle_checksum_of(std::span<const std::byte> payload) {
    std::uint64_t sum = 0;
    for (auto b : payload) {
        sum += std::to_integer<unsigned>(b);
    }
    return static_cast<std::uint32_t>(sum & 0xFFFFFFFFU);
}

// Read a little-endian u32 from p, assuming p .. p+4 is valid. The caller checks
// bounds before calling.
inline std::uint32_t oracle_get_le_u32(const std::byte* p) {
    return std::uint32_t(std::to_integer<unsigned>(p[0])) |
           (std::uint32_t(std::to_integer<unsigned>(p[1])) << 8) |
           (std::uint32_t(std::to_integer<unsigned>(p[2])) << 16) |
           (std::uint32_t(std::to_integer<unsigned>(p[3])) << 24);
}

struct WalOracle {
    bool decoded = false;        // a complete record was present
    bool complete_header = false; // at least 8 header bytes were present
    std::uint32_t magic = 0;     // raw magic field
    std::uint32_t length = 0;    // declared payload length
    std::vector<std::byte> payload; // payload bytes actually present (may be < length)
    std::uint32_t stored_checksum = 0; // raw checksum field (only valid if decoded)
    std::size_t consumed = 0;    // total bytes the record would consume

    // True only when the frame is structurally valid AND the stored checksum
    // matches the independent checksum of the payload.
    bool checksum_ok() const {
        return decoded && oracle_checksum_of(payload) == stored_checksum;
    }

    bool magic_ok() const { return magic == sluice::wal::magic; }
};

// Decode at most one record from the front of `bytes`. Trailing bytes beyond the
// first complete record are ignored (permitted by the contract).
inline WalOracle decode_one(std::span<const std::byte> bytes) {
    WalOracle o;
    std::size_t pos = 0;
    auto need = [&](std::size_t n) -> bool {
        if (pos + n > bytes.size()) {
            return false;
        }
        pos += n;
        return true;
    };

    // Header: magic (4) + length (4).
    if (!need(8)) {
        return o;
    }
    o.complete_header = true;
    o.magic = oracle_get_le_u32(bytes.data());
    o.length = oracle_get_le_u32(bytes.data() + 4);

    // Payload: `length` bytes. If truncated, record is not decoded.
    if (o.length > 0) {
        if (pos + o.length > bytes.size()) {
            // Partial payload: capture what's there, but mark incomplete.
            o.payload.assign(bytes.data() + pos, bytes.data() + bytes.size());
            pos = bytes.size();
            o.consumed = pos;
            return o;
        }
        o.payload.assign(bytes.data() + pos, bytes.data() + pos + o.length);
        pos += o.length;
    }

    // Checksum: 4 bytes.
    if (!need(4)) {
        return o;
    }
    o.stored_checksum = oracle_get_le_u32(bytes.data() + pos - 4);
    o.decoded = true;
    o.consumed = pos;
    return o;
}

} // namespace fuzz
