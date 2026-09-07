











#pragma once

#include <cstddef>
#include <cstdint>

namespace sluice_hash {





class Sha256 {
public:
    Sha256();



    void update(const std::uint8_t* data, std::size_t len) noexcept;
    void final(std::uint8_t out_digest[32]) noexcept;

    static constexpr std::size_t kDigestBytes = 32;
    static constexpr std::size_t kBlockBytes = 64;

private:
    void compress_block(const std::uint8_t* block) noexcept;

    std::uint32_t h_[8];
    std::uint64_t total_bytes_;
    std::uint8_t buf_[kBlockBytes];
    std::size_t buf_len_;
    bool finalized_;
};


void sha256_hex(const std::uint8_t digest[32], char out[65]);

}
