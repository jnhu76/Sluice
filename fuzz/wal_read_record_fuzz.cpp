// WAL raw-decoder fuzz target.
//
// Treats the fuzz input as: [config byte] [arbitrary byte stream]. The config
// byte selects a short-read cap for the tracking reader; the rest is fed to the
// real production sluice::wal::read_record(). An INDEPENDENT oracle (see
// wal_oracle.hpp) decodes the same stream from scratch and the target checks:
//
//   W1 no crash / sanitizer failure  — implicit: any UB traps here
//   W2 successful decode => valid frame (magic, length, checksum, payload)
//   W3 invalid canonical framing => no success
//   W4 error classification => eof or invalid_state
//   W5 bounded read request => never more than one production chunk
//
// The oracle does not call any production WAL helper.
#include <sluice/wal.hpp>

#include <fuzz/support/byte_cursor.hpp>
#include <fuzz/support/fuzz_abort.hpp>
#include <fuzz/support/tracking_reader.hpp>
#include <fuzz/support/wal_oracle.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

using fuzz::ByteCursor;
using fuzz::TrackingReader;
using fuzz::WalOracle;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // Need at least the config byte. With only a config byte and no stream the
    // oracle reports "not decoded" and production reports eof — still a valid
    // (empty) case, so we do not early-return.
    const std::byte* bytes = reinterpret_cast<const std::byte*>(data);
    ByteCursor cur(bytes, size);

    // Config: short-read cap 1..64. Varying this exercises the bounded-growth
    // path inside read_record()'s read_exact loop without letting a single
    // read_one() stall the stream.
    std::size_t max_short = cur.take_range(1, 64);
    auto stream = cur.take_rest();

    // Independent oracle decode of the same stream.
    WalOracle oracle = fuzz::decode_one(stream);

    // Production decode through a tracking reader.
    TrackingReader reader(stream, max_short);
    auto result = sluice::wal::read_record(reader);

    if (result.has_value()) {
        // W2: success implies a fully present, valid frame.
        FUZZ_ASSERT(oracle.decoded);
        FUZZ_ASSERT(oracle.magic_ok());
        FUZZ_ASSERT(stream.size() >= 12); // header(8) + checksum(4), empty payload ok
        FUZZ_ASSERT(oracle.checksum_ok());
        const auto& payload = result.value();
        FUZZ_ASSERT(payload.size() == oracle.payload.size());
        if (!payload.empty()) {
            FUZZ_ASSERT(std::memcmp(payload.data(), oracle.payload.data(), payload.size()) == 0);
        }
    } else {
        // W4: with the bounded in-memory harness, failure must classify as eof
        // or invalid_state. A backend_error here would signal a resource
        // problem worth preserving rather than allowlisting.
        const auto code = result.error().code;
        FUZZ_ASSERT(code == sluice::IoError::Code::eof ||
                    code == sluice::IoError::Code::invalid_state);
    }

    // W3: if the oracle sees a structurally invalid frame (truncated, bad magic,
    // bad checksum), production must not succeed.
    if (!oracle.decoded || !oracle.magic_ok() || !oracle.checksum_ok()) {
        FUZZ_ASSERT(!result.has_value());
    }

    // W5: production must never request more than one bounded growth step. The
    // header read is a fixed 8 bytes; payload reads are capped at read_chunk_size.
    // The largest single request is therefore max(8, read_chunk_size(remaining)).
    // We bound it by the worst case: read_chunk_size(huge) == 64KiB, but the
    // header read is only 8 bytes, so the cap is the chunk bound. Derive it from
    // the documented contract rather than hard-coding 65536.
    constexpr std::size_t kHeader = 8;
    const std::size_t chunk_bound =
        sluice::wal::detail::read_chunk_size(std::numeric_limits<std::uint32_t>::max());
    const std::size_t max_allowed = std::max(kHeader, chunk_bound);
    FUZZ_ASSERT(reader.max_request() <= max_allowed);

    return 0;
}
