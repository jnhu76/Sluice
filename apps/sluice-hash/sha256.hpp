// sluice-hash — app-local SHA-256 (FIPS 180-4).
//
// A straight implementation of the published SHA-256 specification (NIST FIPS
// 180-4 §6.2): the standard message schedule + compression function with the
// standard K constants and IV. This is NOT a novel hash: correctness is
// anchored by the NIST test vectors in tests/sluice_hash_sha256_test.cpp.
//
// Kept app-local deliberately (file-tools plan §3.2): the repository has no
// crypto dependency surface, and pulling an external crypto library for one
// application target is not warranted yet — see
// docs/applications/file-tools-findings.md for the promotion/swap-out
// discussion. Streaming API so callers stay memory-bounded.
#pragma once

#include <cstddef>
#include <cstdint>

namespace sluice_hash {

// Streaming SHA-256. update() may be called any number of times with any
// chunking (the digest depends only on the concatenated bytes, not on the
// chunk boundaries); final() appends the standard padding and writes the
// 32-byte big-endian digest. After final() the instance must not be reused.
class Sha256 {
public:
    Sha256();

    // Both point at caller-owned memory valid for the call. noexcept: the
    // implementation allocates nothing.
    void update(const std::uint8_t* data, std::size_t len) noexcept;
    void final(std::uint8_t out_digest[32]) noexcept;

    static constexpr std::size_t kDigestBytes = 32;
    static constexpr std::size_t kBlockBytes = 64;

private:
    void compress_block(const std::uint8_t* block) noexcept;

    std::uint32_t h_[8];
    std::uint64_t total_bytes_;   // total message length in BYTES (< 2^61)
    std::uint8_t buf_[kBlockBytes];
    std::size_t buf_len_;
    bool finalized_;
};

// Lowercase hex encoding of 32 raw bytes into out (65 chars: 64 + NUL).
void sha256_hex(const std::uint8_t digest[32], char out[65]);

}  // namespace sluice_hash
