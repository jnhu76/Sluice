// FaultReader / FaultWriter implementations: deterministic short-I/O + failure
// injection layered on top of any Reader/Writer.
#include <cppio/fault.hpp>

#include <algorithm>
#include <cstddef>

namespace cppio {

Result<std::size_t> FaultReader::read_some(std::span<std::byte> dst) {
    ++read_calls_;

    if (plan_.fail_after_read_calls && read_calls_ > *plan_.fail_after_read_calls) {
        return make_unexpected<std::size_t>(plan_.error);
    }
    if (plan_.fail_after_bytes && bytes_seen_ >= *plan_.fail_after_bytes) {
        return make_unexpected<std::size_t>(plan_.error);
    }

    std::span<std::byte> window = dst;
    if (plan_.max_read_size) {
        window = dst.first(std::min(dst.size(), *plan_.max_read_size));
    }

    auto r = inner_.read_some(window);
    if (!r.has_value()) return make_unexpected<std::size_t>(r.error());
    bytes_seen_ += r.value();
    return r.value();
}

Result<std::size_t> FaultWriter::write_some(std::span<const std::byte> src) {
    ++write_calls_;

    if (plan_.fail_after_write_calls && write_calls_ > *plan_.fail_after_write_calls) {
        return make_unexpected<std::size_t>(plan_.error);
    }
    if (plan_.fail_after_bytes && bytes_seen_ >= *plan_.fail_after_bytes) {
        return make_unexpected<std::size_t>(plan_.error);
    }

    std::span<const std::byte> window = src;
    if (plan_.max_write_size) {
        window = src.first(std::min(src.size(), *plan_.max_write_size));
    }

    auto r = inner_.write_some(window);
    if (!r.has_value()) return make_unexpected<std::size_t>(r.error());
    bytes_seen_ += r.value();
    return r.value();
}

Result<void> FaultWriter::flush() {
    if (plan_.fail_flush) return make_unexpected<void>(plan_.error);
    return inner_.flush();
}

}  // namespace cppio
