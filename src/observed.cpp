// ObservedReader / ObservedWriter implementations: count and delegate.
#include <cppio/observed.hpp>

namespace cppio {

Result<std::size_t> ObservedReader::read_some(std::span<std::byte> dst) {
    ++stats_.read_calls;
    auto r = inner_.read_some(dst);
    if (!r.has_value()) {
        ++stats_.read_errors;
        return make_unexpected<std::size_t>(r.error());
    }
    std::size_t n = r.value();
    stats_.read_bytes += n;
    if (n == 0) ++stats_.eof_count;
    return n;
}

Result<std::size_t> ObservedWriter::write_some(std::span<const std::byte> src) {
    ++stats_.write_calls;
    auto r = inner_.write_some(src);
    if (!r.has_value()) {
        ++stats_.write_errors;
        return make_unexpected<std::size_t>(r.error());
    }
    std::size_t n = r.value();
    stats_.write_bytes += n;
    if (n < src.size()) ++stats_.short_writes;
    return n;
}

Result<void> ObservedWriter::flush() {
    ++stats_.flush_calls;
    auto r = inner_.flush();
    if (!r.has_value()) {
        ++stats_.flush_errors;
        return make_unexpected<void>(r.error());
    }
    return {};
}

}  // namespace cppio
