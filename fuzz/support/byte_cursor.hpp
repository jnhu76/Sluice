// Deterministic, total byte-stream decoder for fuzz input.
//
// Every libFuzzer buffer is a flat byte array. Decoding it into structured
// fields (lengths, modes, small integers) must be TOTAL: no out-of-bounds read,
// no undefined behavior, and a defined value for every possible byte sequence,
// including truncated ones. When the cursor runs out of bytes it yields
// deterministic defaults (zero) instead of failing — the harness decides what
// a short input means.
//
// All multi-byte integers are read little-endian to match the WAL on-disk layout
// and the little-endian seed corpus.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fuzz {

class ByteCursor {
  public:
    ByteCursor(const std::byte* data, std::size_t size) : data_(data), size_(size) {}
    explicit ByteCursor(std::span<const std::byte> s) : data_(s.data()), size_(s.size()) {}

    std::size_t remaining() const noexcept { return size_ - pos_; }
    bool empty() const noexcept { return pos_ >= size_; }

    // Consume one byte, or 0 if the stream is exhausted.
    std::byte take_byte() noexcept {
        if (pos_ >= size_) {
            return std::byte{0};
        }
        return data_[pos_++];
    }

    // Consume one byte as an unsigned integer.
    std::uint8_t take_u8() noexcept { return std::to_integer<std::uint8_t>(take_byte()); }

    // Consume four bytes as a little-endian u32, zero-padding if exhausted.
    std::uint32_t take_u32_le() noexcept {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ + i >= size_) {
                break;
            }
            v |= static_cast<std::uint32_t>(std::to_integer<unsigned>(data_[pos_ + i]))
                 << (8 * i);
        }
        pos_ = std::min(pos_ + 4, size_);
        return v;
    }

    // Map the next byte uniformly into [0, count-1] via real modulo. Total: an
    // exhausted stream yields 0, and count==0 yields 0. Unlike a clamp, this
    // distributes values across the whole range so enum selectors (failure modes,
    // error codes, strategies, buffered capability) are exercised evenly rather
    // than biasing the last member.
    std::uint8_t take_mod(std::uint8_t count) noexcept {
        if (count == 0) {
            return 0;
        }
        return static_cast<std::uint8_t>(take_u8() % count);
    }

    // Consume a length in [lo, hi] (inclusive) from a little-endian u32, clamped
    // into range. Total: an exhausted stream yields lo.
    std::size_t take_range(std::size_t lo, std::size_t hi) noexcept {
        std::uint32_t raw = take_u32_le();
        if (hi <= lo) {
            return lo;
        }
        // hi - lo fits comfortably in u32 for the small ranges the harness uses.
        std::uint32_t span = static_cast<std::uint32_t>(hi - lo);
        return lo + static_cast<std::size_t>(raw % (span + 1));
    }

    // View of the unconsumed tail. Does not advance the cursor. Safe for an
    // exhausted or empty input: returns an empty span without dereferencing
    // data_ (which may be null for an empty libFuzzer buffer).
    std::span<const std::byte> tail() const noexcept {
        if (pos_ >= size_) {
            return {};
        }
        return {data_ + pos_, size_ - pos_};
    }

    // Copy the unconsumed tail into an owned vector.
    std::vector<std::byte> take_rest() const {
        auto t = tail();
        return {t.begin(), t.end()};
    }

  private:
    const std::byte* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

} // namespace fuzz
