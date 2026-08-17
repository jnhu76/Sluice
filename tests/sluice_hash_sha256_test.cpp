// sluice-hash SHA-256 tests.
//
// Correctness is anchored by the published FIPS 180-4 / NIST test vectors
// (the same set used by NIST CAVP/SHAVS "short/long message" categories as
// commonly cited). Streaming robustness is proven by re-chunking the same
// input at every boundary-sensitive size and requiring identical digests.
#include "harness.hpp"

#include "sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using sluice_hash::Sha256;

namespace {

std::string digest_hex(const std::uint8_t* data, std::size_t len) {
    Sha256 h;
    h.update(data, len);
    std::uint8_t d[32];
    h.final(d);
    char hex[65];
    sluice_hash::sha256_hex(d, hex);
    return std::string(hex);
}

std::string digest_str(const std::string& s) {
    return digest_hex(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

// Feed the same bytes in varying chunk sizes; all digests must be identical
// (streaming state must not depend on chunk boundaries).
std::string chunked_hex(const std::string& s, std::size_t chunk) {
    Sha256 h;
    for (std::size_t i = 0; i < s.size(); i += chunk) {
        std::size_t n = std::min(chunk, s.size() - i);
        h.update(reinterpret_cast<const std::uint8_t*>(s.data()) + i, n);
    }
    std::uint8_t d[32];
    h.final(d);
    char hex[65];
    sluice_hash::sha256_hex(d, hex);
    return std::string(hex);
}

}  // namespace

// ---------------------------------------------------------------------------
// NIST / FIPS 180-4 vectors
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(sha256_empty_message) {
    SLUICE_CHECK(digest_str("") ==
                 "e3b0c44298fc1c149afbf4c8996fb924"
                 "27ae41e4649b934ca495991b7852b855");
}

SLUICE_TEST_CASE(sha256_abc_one_block) {
    SLUICE_CHECK(digest_str("abc") ==
                 "ba7816bf8f01cfea414140de5dae2223"
                 "b00361a396177a9cb410ff61f20015ad");
}

SLUICE_TEST_CASE(sha256_448_bit_message) {
    SLUICE_CHECK(digest_str("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
                 "248d6a61d20638b8e5c026930c3e6039"
                 "a33ce45964ff2167f6ecedd419db06c1");
}

SLUICE_TEST_CASE(sha256_896_bit_two_block_message) {
    SLUICE_CHECK(digest_str(
                     "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                     "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
                 "cf5b16a778af8380036ce59e7b049237"
                 "0b249b11e8f07a51afac45037afee9d1");
}

SLUICE_TEST_CASE(sha256_one_million_a) {
    // FIPS 180-4 / NIST long-message vector: 1,000,000 repetitions of 'a'.
    // Also the multi-block streaming stress anchor.
    Sha256 h;
    std::string block(1000, 'a');
    for (int i = 0; i < 1000; ++i)
        h.update(reinterpret_cast<const std::uint8_t*>(block.data()),
                 block.size());
    std::uint8_t d[32];
    h.final(d);
    char hex[65];
    sluice_hash::sha256_hex(d, hex);
    SLUICE_CHECK(std::string(hex) ==
                 "cdc76e5c9914fb9281a1c7e284d73e67"
                 "f1809a48a497200e046d39ccc7112cd0");
}

// ---------------------------------------------------------------------------
// Streaming boundary robustness: identical digests across chunkings
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(sha256_chunking_invariance_at_boundaries) {
    // Lengths chosen to straddle the 64-byte block boundary and the padding
    // edge cases (55/56/63/64/65 bytes; message len mod 64 in {55,56,63,0,1}).
    std::string s;
    for (int i = 0; i < 5000; ++i)
        s.push_back(static_cast<char>((i * 31 + 7) & 0xff));

    const std::string reference = digest_str(s);
    const std::size_t chunks[] = {1, 2, 3, 7, 55, 56, 63, 64, 65,
                                  127, 128, 129, 1000};
    for (std::size_t c : chunks) {
        if (chunked_hex(s, c) != reference) {
            SLUICE_CHECK_MSG(false, "chunk-size invariance failed");
            return;
        }
    }
    SLUICE_CHECK(true);
}

SLUICE_TEST_CASE(sha256_incremental_byte_feed_matches_bulk) {
    std::string s = "The quick brown fox jumps over the lazy dog";
    const std::string reference = digest_str(s);
    Sha256 h;
    for (char c : s)
        h.update(reinterpret_cast<const std::uint8_t*>(&c), 1);
    std::uint8_t d[32];
    h.final(d);
    char hex[65];
    sluice_hash::sha256_hex(d, hex);
    SLUICE_CHECK(std::string(hex) == reference);
    // Well-known vector for this exact sentence.
    SLUICE_CHECK(reference ==
                 "d7a8fbb307d7809469ca9abcb0082e4f"
                 "8d5651e46d3cdb762d02d0bf37c9e592");
}

SLUICE_MAIN()
