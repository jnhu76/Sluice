// BufferedReader / BufferedWriter implementations.
#include <cppio/buffer.hpp>

#include <algorithm>
#include <cstring>

namespace cppio {

// ---------------- BufferedReader ----------------

Result<std::size_t> BufferedReader::read_some(std::span<std::byte> dst) {
    // Release-safe precondition: an empty backing buffer can never make
    // progress, so reject up-front rather than spinning or UB-ing. (The
    // constructor also asserts this in debug builds.)
    if (buf_.empty()) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
    }
    if (dst.empty()) return std::size_t{0};

    if (stats_) {
        ++stats_->read_requests;
        stats_->read_request_bytes += dst.size();
    }

    std::size_t total = 0;
    while (!dst.empty()) {
        // Serve as much as possible from the buffer first.
        std::size_t avail = end_ - seek_;
        if (avail > 0) {
            std::size_t n = std::min(dst.size(), avail);
            std::memcpy(dst.data(), buf_.data() + seek_, n);
            seek_ += n;
            dst = dst.subspan(n);
            total += n;
            if (stats_) {
                ++stats_->read_buffer_hits;
                stats_->read_buffer_hit_bytes += n;
            }
            // If dst is satisfied, we're done without touching the inner reader.
            if (dst.empty()) break;
        } else if (stats_) {
            ++stats_->read_buffer_misses;
        }

        // Buffer exhausted: refill once. If dst now fits entirely in the
        // buffer we still refill the buffer (keeping the buffering property);
        // a request larger than the buffer is filled directly from the inner
        // reader after first draining what we have.
        if (dst.size() > buf_.size()) {
            // Large read: go straight to inner, bypassing the buffer for the
            // remainder. The bytes already copied (total) stay valid.
            auto r = inner_.read_some(dst);
            if (!r.has_value()) {
                if (total > 0) return total;  // already delivered some bytes
                return make_unexpected<std::size_t>(r.error());
            }
            std::size_t n = r.value();
            total += n;
            // Stop after one inner read; caller can call again for the rest.
            return total;
        }

        // Compact + refill the buffer.
        if (seek_ > 0) {
            std::memmove(buf_.data(), buf_.data() + seek_, end_ - seek_);
            end_ -= seek_;
            seek_ = 0;
        }
        auto r = inner_.read_some(std::span<std::byte>(buf_.data() + end_, buf_.size() - end_));
        if (!r.has_value()) {
            if (total > 0) return total;
            return make_unexpected<std::size_t>(r.error());
        }
        std::size_t got = r.value();
        if (got == 0) {
            // EOF. Return whatever we accumulated this call.
            return total;
        }
        if (stats_) {
            ++stats_->read_refill_calls;
            stats_->read_refill_bytes += got;
        }
        end_ += got;
    }
    return total;
}

// consume_buffered: advance seek_ by exactly n. No inner-reader call, no refill.
// Rejects n larger than the currently buffered unread region.
Result<void> BufferedReader::consume_buffered(std::size_t n) {
    std::size_t avail = end_ - seek_;
    if (n > avail) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    seek_ += n;
    return {};
}

Result<void> BufferedWriter::flush_dirty() {
    if (end_ > 0 && stats_) ++stats_->write_flush_calls;
    while (end_ > 0) {
        auto r = inner_.write_some(std::span<const std::byte>(buf_.data(), end_));
        if (!r.has_value()) {
            flush_ever_failed_ = true;  // dirty bytes may remain; suppress dtor assert
            return make_unexpected<void>(r.error());
        }
        std::size_t n = r.value();
        if (n == 0) {
            flush_ever_failed_ = true;
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        if (stats_) stats_->write_flush_bytes += n;
        if (n >= end_) {
            end_ = 0;
            break;
        }
        // Drop the written prefix, keep the rest ordered.
        std::memmove(buf_.data(), buf_.data() + n, end_ - n);
        end_ -= n;
    }
    return {};
}

Result<std::size_t> BufferedWriter::write_some(std::span<const std::byte> src) {
    // Release-safe precondition: see BufferedReader::read_some.
    if (buf_.empty()) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
    }
    if (src.empty()) return std::size_t{0};

    if (stats_) {
        ++stats_->write_requests;
        stats_->write_request_bytes += src.size();
    }

    std::size_t total = 0;
    while (!src.empty()) {
        std::size_t room = buf_.size() - end_;
        if (room == 0) {
            auto f = flush_dirty();
            if (!f.has_value()) {
                return total > 0 ? Result<std::size_t>{total}
                                 : make_unexpected<std::size_t>(f.error());
            }
            room = buf_.size();
        }

        if (src.size() > buf_.size() && end_ == 0) {
            // Source larger than the whole buffer and buffer is clean:
            // pass through directly to avoid a needless copy + flush cycle.
            auto r = inner_.write_some(src);
            if (!r.has_value()) {
                return total > 0 ? Result<std::size_t>{total}
                                 : make_unexpected<std::size_t>(r.error());
            }
            std::size_t n = r.value();
            total += n;
            if (stats_) {
                ++stats_->write_direct_calls;
                stats_->write_direct_bytes += n;
            }
            if (n == 0) {
                return total > 0 ? Result<std::size_t>{total}
                                 : make_unexpected<std::size_t>(
                                       IoError{IoError::Code::invalid_state});
            }
            src = src.subspan(n);
            continue;
        }

        // Append into the buffer.
        std::size_t n = std::min(src.size(), room);
        std::memcpy(buf_.data() + end_, src.data(), n);
        end_ += n;
        src = src.subspan(n);
        total += n;
        if (stats_) {
            ++stats_->write_buffered_calls;
            stats_->write_buffered_bytes += n;
        }
        // Stop after absorbing into the buffer: write_some is "may write fewer".
        break;
    }
    return total;
}

Result<void> BufferedWriter::flush() {
    auto f = flush_dirty();
    if (!f.has_value()) return make_unexpected<void>(f.error());
    return inner_.flush();
}

}  // namespace cppio
