// WAL canonical round-trip fuzz target.
//
// Treats the entire fuzz input as an arbitrary payload (bounded by libFuzzer
// -max_len; the harness does not arbitrarily discard bytes). For every payload
// it requires:
//
//   1. write_record() and write_record_vec() emit byte-identical streams.
//   2. The emitted bytes are validated by the INDEPENDENT canonical-format
//      oracle (wal_oracle.hpp): magic == oracle magic, declared length == payload
//      size, stored checksum == independent checksum, consumed == 8 + payload + 4,
//      and the record contains no unexplained extra bytes. This kills common-mode
//      writer/reader format drift that a reader-only check would miss.
//   3. read_record() decodes each stream back to the original payload.
//   4. A second decode at exact EOF returns IoError::Code::eof.
//
// The oracle does not include <sluice/wal.hpp> and re-derives magic/checksum
// independently, so a mutation that changes the shared production checksum
// algorithm (affecting both writers and the reader) is caught here.
#include <sluice/wal.hpp>
#include <sluice/fault.hpp>

#include <fuzz/support/fuzz_abort.hpp>
#include <fuzz/support/wal_oracle.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using fuzz::WalOracle;
using fuzz::decode_one;
using fuzz::kOracleWalMagic;
using fuzz::oracle_checksum_of;

// Validate an emitted record byte-stream against the independent canonical
// format. Returns nothing; traps on any deviation.
static void check_canonical(std::span<const std::byte> emitted,
                            std::span<const std::byte> payload) {
    WalOracle o = decode_one(emitted);

    // The frame must decode completely.
    FUZZ_ASSERT(o.decoded);
    // Independent magic must match.
    FUZZ_ASSERT(o.magic == kOracleWalMagic);
    // Declared length equals the original payload size.
    FUZZ_ASSERT(o.length == payload.size());
    // Payload bytes equal the original input.
    FUZZ_ASSERT(o.payload.size() == payload.size());
    if (!payload.empty()) {
        FUZZ_ASSERT(std::memcmp(o.payload.data(), payload.data(), payload.size()) == 0);
    }
    // Stored checksum equals the independent checksum of the payload.
    FUZZ_ASSERT(o.stored_checksum == oracle_checksum_of(payload));
    // Consumed size is exactly header(8) + payload + checksum(4).
    FUZZ_ASSERT(o.consumed == 8 + payload.size() + 4);
    // No unexplained extra bytes after the record.
    FUZZ_ASSERT(emitted.size() == o.consumed);
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> payload(reinterpret_cast<const std::byte*>(data), size);

    // 1. Scalar encode.
    sluice::MemoryWriter scalar;
    auto rs = sluice::wal::write_record(scalar, payload);
    FUZZ_ASSERT(rs.has_value());

    // 2. Vector encode.
    sluice::MemoryWriter vector;
    auto rv = sluice::wal::write_record_vec(vector, payload);
    FUZZ_ASSERT(rv.has_value());

    // 3. Byte-identical encodings (scalar/vector parity).
    const auto& sb = scalar.bytes();
    const auto& vb = vector.bytes();
    FUZZ_ASSERT(sb.size() == vb.size());
    if (!sb.empty()) {
        FUZZ_ASSERT(std::memcmp(sb.data(), vb.data(), sb.size()) == 0);
    }

    // 4. Canonical-format verification by the INDEPENDENT oracle. This is the
    //    common-mode guard: even if both writers and the reader drifted together
    //    (e.g. a shared checksum-algorithm mutation), the independent oracle
    //    would reject the emitted bytes.
    check_canonical(sb, payload);
    check_canonical(vb, payload);

    // 5. Decode the scalar stream round-trips to the original payload.
    sluice::MemoryReader ri(sb);
    auto ds = sluice::wal::read_record(ri);
    FUZZ_ASSERT(ds.has_value());
    FUZZ_ASSERT(ds.value().size() == payload.size());
    if (!payload.empty()) {
        FUZZ_ASSERT(std::memcmp(ds.value().data(), payload.data(), payload.size()) == 0);
    }

    // 6. Decode the vector stream round-trips to the original payload.
    sluice::MemoryReader vi(vb);
    auto dv = sluice::wal::read_record(vi);
    FUZZ_ASSERT(dv.has_value());
    FUZZ_ASSERT(dv.value().size() == payload.size());
    if (!payload.empty()) {
        FUZZ_ASSERT(std::memcmp(dv.value().data(), payload.data(), payload.size()) == 0);
    }

    // 7. One additional decode at exact EOF returns eof. Clean EOF after exactly
    //    one record — no synthesis of undisclosed trailing bytes.
    auto es = sluice::wal::read_record(ri);
    FUZZ_ASSERT(!es.has_value());
    FUZZ_ASSERT(es.error().code == sluice::IoError::Code::eof);

    auto ev = sluice::wal::read_record(vi);
    FUZZ_ASSERT(!ev.has_value());
    FUZZ_ASSERT(ev.error().code == sluice::IoError::Code::eof);

    return 0;
}
