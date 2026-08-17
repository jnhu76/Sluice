// sluice-hash — app-local SHA-256 implementation (FIPS 180-4 §6.2).
#include "sha256.hpp"

namespace sluice_hash {

namespace {

// Round constants and IV from FIPS 180-4 §4.2.2 / §5.3.3 (not magic numbers
// of our invention; anchored by the NIST vectors in the tests).
constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::uint32_t rotr(std::uint32_t x, unsigned n) noexcept {
    return (x >> n) | (x << (32 - n));
}

}  // namespace

Sha256::Sha256()
    : total_bytes_{0}, buf_len_{0}, finalized_{false} {
    // IV from FIPS 180-4 §5.3.3 (fractional parts of sqrt of first 8 primes).
    h_[0] = 0x6a09e667;
    h_[1] = 0xbb67ae85;
    h_[2] = 0x3c6ef372;
    h_[3] = 0xa54ff53a;
    h_[4] = 0x510e527f;
    h_[5] = 0x9b05688c;
    h_[6] = 0x1f83d9ab;
    h_[7] = 0x5be0cd19;
}

void Sha256::compress_block(const std::uint8_t* p) noexcept {
    std::uint32_t w[64];
    for (int t = 0; t < 16; ++t) {
        w[t] = (static_cast<std::uint32_t>(p[4 * t]) << 24) |
               (static_cast<std::uint32_t>(p[4 * t + 1]) << 16) |
               (static_cast<std::uint32_t>(p[4 * t + 2]) << 8) |
               static_cast<std::uint32_t>(p[4 * t + 3]);
    }
    for (int t = 16; t < 64; ++t) {
        std::uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^
                           (w[t - 15] >> 3);
        std::uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^
                           (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }

    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    std::uint32_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
    for (int t = 0; t < 64; ++t) {
        std::uint32_t big_s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        std::uint32_t ch = (e & f) ^ (~e & g);
        std::uint32_t t1 = hh + big_s1 + ch + kK[t] + w[t];
        std::uint32_t big_s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t t2 = big_s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += hh;
}

void Sha256::update(const std::uint8_t* data, std::size_t len) noexcept {
    total_bytes_ += len;
    // Fill a partial buffer first.
    if (buf_len_ > 0) {
        std::size_t take = kBlockBytes - buf_len_;
        if (take > len) take = len;
        for (std::size_t i = 0; i < take; ++i) buf_[buf_len_ + i] = data[i];
        buf_len_ += take;
        data += take;
        len -= take;
        if (buf_len_ == kBlockBytes) {
            compress_block(buf_);
            buf_len_ = 0;
        }
    }
    // Whole blocks straight from the input (no copy).
    while (len >= kBlockBytes) {
        compress_block(data);
        data += kBlockBytes;
        len -= kBlockBytes;
    }
    // Tail into the buffer (only reachable with buf_len_ == 0: a partial
    // buffer above either filled+compressed to a block boundary or exhausted
    // the input).
    for (std::size_t i = 0; i < len; ++i) buf_[buf_len_ + i] = data[i];
    buf_len_ += len;
}

void Sha256::final(std::uint8_t out_digest[32]) noexcept {
    // Standard padding: 0x80, zeros, 64-bit big-endian BIT length (§5.1.1).
    std::uint64_t bit_len = total_bytes_ * 8;
    std::uint8_t one = 0x80;
    update(&one, 1);  // also counts into total_bytes_, which padding ignores

    std::uint8_t zero = 0;
    while (buf_len_ != 56) update(&zero, 1);

    std::uint8_t len_be[8];
    for (int i = 0; i < 8; ++i)
        len_be[i] = static_cast<std::uint8_t>(bit_len >> (56 - 8 * i));
    // Bypass update() (it would recount the length bytes): write directly
    // through one final compress.
    for (int i = 0; i < 8; ++i) buf_[56 + i] = len_be[i];
    compress_block(buf_);
    buf_len_ = 0;
    finalized_ = true;

    for (int i = 0; i < 8; ++i) {
        out_digest[4 * i] = static_cast<std::uint8_t>(h_[i] >> 24);
        out_digest[4 * i + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
        out_digest[4 * i + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
        out_digest[4 * i + 3] = static_cast<std::uint8_t>(h_[i]);
    }
}

void sha256_hex(const std::uint8_t digest[32], char out[65]) {
    static const char kHex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < 32; ++i) {
        out[2 * i] = kHex[digest[i] >> 4];
        out[2 * i + 1] = kHex[digest[i] & 0xf];
    }
    out[64] = '\0';
}

}  // namespace sluice_hash
