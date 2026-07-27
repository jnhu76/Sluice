// WAL canonical round-trip fuzz target.
//
// Treats the entire fuzz input as an arbitrary payload (bounded by libFuzzer
// -max_len; the harness does not arbitrarily discard bytes). For every payload
// it requires:
//
//   1. write_record() and write_record_vec() emit byte-identical streams.
//   2. read_record() decodes each stream back to the original payload.
//   3. a second decode at exact EOF returns IoError::Code::eof.
//
// This locks in scalar/vector framing parity, empty and binary payload support,
// little-endian length, checksum correctness, exact one-record consumption, and
// clean EOF behavior.
#include <sluice/wal.hpp>
#include <sluice/fault.hpp>

#include <fuzz/support/fuzz_abort.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

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

    // 4. Decode the scalar stream.
    sluice::MemoryReader ri(sb);
    auto ds = sluice::wal::read_record(ri);
    FUZZ_ASSERT(ds.has_value());
    FUZZ_ASSERT(ds.value().size() == payload.size());
    if (!payload.empty()) {
        FUZZ_ASSERT(std::memcmp(ds.value().data(), payload.data(), payload.size()) == 0);
    }

    // 5. Decode the vector stream.
    sluice::MemoryReader vi(vb);
    auto dv = sluice::wal::read_record(vi);
    FUZZ_ASSERT(dv.has_value());
    FUZZ_ASSERT(dv.value().size() == payload.size());
    if (!payload.empty()) {
        FUZZ_ASSERT(std::memcmp(dv.value().data(), payload.data(), payload.size()) == 0);
    }

    // 6. One additional decode at exact EOF returns eof.
    auto es = sluice::wal::read_record(ri);
    FUZZ_ASSERT(!es.has_value());
    FUZZ_ASSERT(es.error().code == sluice::IoError::Code::eof);

    auto ev = sluice::wal::read_record(vi);
    FUZZ_ASSERT(!ev.has_value());
    FUZZ_ASSERT(ev.error().code == sluice::IoError::Code::eof);

    return 0;
}
