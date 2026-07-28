// WAL raw-decoder fuzz target.
//
// Treats the fuzz input as: [config byte] [arbitrary byte stream]. The config
// byte selects a short-read cap for the tracking reader; the rest is fed to the
// real production sluice::wal::read_record(). An INDEPENDENT oracle (see
// wal_oracle.hpp) decodes the same stream from scratch — without including
// <sluice/wal.hpp> or calling any production WAL helper — and the target checks:
//
//   W1 no crash / sanitizer failure  — implicit: any UB traps here
//   W2 production success  <=>  oracle canonical validity (decoded AND magic ok
//      AND checksum ok). Bidirectional: a valid canonical frame MUST be accepted
//      and an invalid one MUST be rejected. This closes the gap where a valid
//      persisted frame could be rejected by production without failing the
//      harness.
//   W3 on success, the returned payload equals the oracle payload byte-for-byte
//   W4 on failure, the error classifies as eof or invalid_state
//   W5 bounded read request => never more than one production chunk growth step
//
// The oracle does not include any production header and does not call any
// production WAL helper, so a common-mode mutation that changes the production
// magic or checksum rule is detected here rather than silently agreeing.
#include <sluice/wal.hpp>

#include <fuzz/support/byte_cursor.hpp>
#include <fuzz/support/fuzz_abort.hpp>
#include <fuzz/support/tracking_reader.hpp>
#include <fuzz/support/wal_oracle.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

using fuzz::ByteCursor;
using fuzz::TrackingReader;
using fuzz::WalOracle;
using fuzz::decode_one;
using fuzz::kOracleWalMagic;

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

    // Independent oracle decode of the same stream. Uses the oracle's own magic
    // constant and checksum rule, never production's.
    WalOracle oracle = decode_one(stream);

    // The canonical-validity predicate used for the bidirectional equivalence
    // check. A frame is valid iff it is structurally complete, the magic field
    // equals the independent oracle magic, and the independent checksum of the
    // present payload equals the stored checksum.
    const bool oracle_valid = oracle.decoded && oracle.magic == kOracleWalMagic &&
                              oracle.checksum_ok();

    // Production decode through a tracking reader.
    TrackingReader reader(stream, max_short);
    auto result = sluice::wal::read_record(reader);

    // W2 (bidirectional): production success <=> oracle canonical validity.
    // This is the central invariant. It fails if production accepts a frame the
    // independent oracle rejects, OR rejects a frame the oracle accepts.
    FUZZ_ASSERT(result.has_value() == oracle_valid);

    if (result.has_value()) {
        // W3: success implies byte-for-byte payload agreement with the oracle.
        const auto& payload = result.value();
        FUZZ_ASSERT(payload.size() == oracle.payload.size());
        if (!payload.empty()) {
            FUZZ_ASSERT(std::memcmp(payload.data(), oracle.payload.data(), payload.size()) == 0);
        }
        // The oracle frame must be complete and self-consistent on success.
        FUZZ_ASSERT(oracle.consumed == oracle.expected_consumed());
    } else {
        // W4: with the bounded in-memory harness, failure must classify as eof
        // or invalid_state. A backend_error here would signal a resource problem
        // worth preserving rather than allowlisting.
        const auto code = result.error().code;
        FUZZ_ASSERT(code == sluice::IoError::Code::eof ||
                    code == sluice::IoError::Code::invalid_state);
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
