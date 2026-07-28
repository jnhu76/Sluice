// In-memory Reader that records every requested read size.
//
// Used by the WAL raw-decode fuzz target to (a) supply the fuzz bytes to
// read_record(), (b) exercise the bounded-growth path by allowing short reads,
// and (c) prove the decoder never requests more than one production bounded
// chunk (Property W5). It never lies about how many bytes it returns: the count
// returned is always exactly the number of bytes actually copied into dst.
#pragma once

#include <sluice/reader.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace fuzz {

class TrackingReader final : public sluice::Reader {
  public:
    // Copies the supplied bytes into owned storage. `max_short_read` caps how
    // many bytes a single read_some() may return; it is clamped to >= 1 so the
    // stream always makes progress and a bounded read loop terminates.
    explicit TrackingReader(std::vector<std::byte> bytes, std::size_t max_short_read = 64)
        : buf_(std::move(bytes)), max_short_(std::max<std::size_t>(max_short_read, 1)) {}

    TrackingReader(std::span<const std::byte> bytes, std::size_t max_short_read = 64)
        : buf_(bytes.begin(), bytes.end()),
          max_short_(std::max<std::size_t>(max_short_read, 1)) {}

    sluice::Result<std::size_t> read_some(std::span<std::byte> dst) override {
        if (!dst.empty()) {
            max_request_ = std::max(max_request_, dst.size());
        }
        std::size_t available = buf_.size() - pos_;
        std::size_t count = std::min({available, dst.size(), max_short_});
        if (count > 0) {
            // n != 0, so dst.data() and buf_.data()+pos_ are non-null.
            std::memcpy(dst.data(), buf_.data() + pos_, count);
            pos_ += count;
        }
        return count;
    }

    // Largest dst.size() ever passed to read_some().
    std::size_t max_request() const noexcept { return max_request_; }
    // Configured short-read cap.
    std::size_t max_short() const noexcept { return max_short_; }
    // Total bytes supplied.
    std::size_t size() const noexcept { return buf_.size(); }

  private:
    std::vector<std::byte> buf_;
    std::size_t pos_ = 0;
    std::size_t max_short_;
    std::size_t max_request_ = 0;
};

} // namespace fuzz
