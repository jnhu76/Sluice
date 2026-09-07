
#include <sluice/buffer.hpp>

#include <algorithm>
#include <cstring>

namespace sluice {



Result<std::size_t> BufferedReader::read_some(std::span<std::byte> dst) {



    if (buf_.empty()) {
        return make_unexpected<std::size_t>(IoError{.code = IoError::Code::invalid_state});
    }
    if (dst.empty()) {
        return std::size_t{0};
    }

    if (stats_) {
        ++stats_->read_requests;
        stats_->read_request_bytes += dst.size();
    }

    std::size_t total = 0;
    while (!dst.empty()) {

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

            if (dst.empty()) {
                break;
            }
        } else if (stats_) {
            ++stats_->read_buffer_misses;
        }





        if (dst.size() > buf_.size()) {


            auto r = inner_.read_some(dst);
            if (!r.has_value()) {
                if (total > 0) {
                    return total;
                }
                return make_unexpected<std::size_t>(r.error());
            }
            std::size_t n = r.value();
            total += n;

            return total;
        }


        if (seek_ > 0) {
            std::memmove(buf_.data(), buf_.data() + seek_, end_ - seek_);
            end_ -= seek_;
            seek_ = 0;
        }
        auto r = inner_.read_some(std::span<std::byte>(buf_.data() + end_, buf_.size() - end_));
        if (!r.has_value()) {
            if (total > 0) {
                return total;
            }
            return make_unexpected<std::size_t>(r.error());
        }
        std::size_t got = r.value();
        if (got == 0) {

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



Result<void> BufferedReader::consume_buffered(std::size_t n) {
    std::size_t avail = end_ - seek_;
    if (n > avail) {
        return make_unexpected<void>(IoError{.code = IoError::Code::invalid_state});
    }
    seek_ += n;
    return {};
}

Result<void> BufferedWriter::flush_dirty() {
    if (end_ > 0 && stats_) {
        ++stats_->write_flush_calls;
    }
    while (end_ > 0) {
        auto r = inner_.write_some(std::span<const std::byte>(buf_.data(), end_));
        if (!r.has_value()) {
            flush_ever_failed_ = true;
            return make_unexpected<void>(r.error());
        }
        std::size_t n = r.value();
        if (n == 0) {
            flush_ever_failed_ = true;
            return make_unexpected<void>(IoError{.code = IoError::Code::invalid_state});
        }
        if (stats_) {
            stats_->write_flush_bytes += n;
        }
        if (n >= end_) {
            end_ = 0;
            break;
        }

        std::memmove(buf_.data(), buf_.data() + n, end_ - n);
        end_ -= n;
    }
    return {};
}

Result<std::size_t> BufferedWriter::write_some(std::span<const std::byte> src) {

    if (buf_.empty()) {
        return make_unexpected<std::size_t>(IoError{.code = IoError::Code::invalid_state});
    }
    if (src.empty()) {
        return std::size_t{0};
    }

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
                                       IoError{.code = IoError::Code::invalid_state});
            }
            src = src.subspan(n);
            continue;
        }


        std::size_t n = std::min(src.size(), room);
        std::memcpy(buf_.data() + end_, src.data(), n);
        end_ += n;
        src = src.subspan(n);
        total += n;
        if (stats_) {
            ++stats_->write_buffered_calls;
            stats_->write_buffered_bytes += n;
        }

        break;
    }
    return total;
}

Result<void> BufferedWriter::flush() {
    auto f = flush_dirty();
    if (!f.has_value()) {
        return make_unexpected<void>(f.error());
    }
    return inner_.flush();
}

}
