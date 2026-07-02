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

Result<std::size_t> ObservedReader::read_vec(std::span<IoSlice> dsts) {
    // Delegating to inner_.read_vec runs the default fallback for the
    // non-overriding readers this wrapper is meant to observe (MemoryReader,
    // FaultReader, ...). We count the call/bytes/iovecs and — by design — a
    // fallback call, since this layer observes the default-fallback path. The
    // real (non-fallback) readv path is measured by FileReader's own
    // VectorStats; do not wrap a FileReader here for vec stats (it carries its
    // own). See docs/readv-writev-design-note.md.
    auto r = inner_.read_vec(dsts);
    if (vec_stats_) {
        ++vec_stats_->read_vec_calls;
        ++vec_stats_->read_vec_fallback_calls;
        std::uint64_t iovecs = 0;
        for (const auto& d : dsts) if (!d.bytes.empty()) ++iovecs;
        vec_stats_->read_vec_iovecs += iovecs;
        if (r.has_value()) vec_stats_->read_vec_bytes += r.value();
    }
    return r;
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

Result<std::size_t> ObservedWriter::write_vec(std::span<const ConstIoSlice> srcs) {
    // See read_vec above: this wrapper observes the default-fallback path, so
    // every vector call counts as a fallback. The real writev path is measured
    // by FileWriter's own VectorStats.
    auto r = inner_.write_vec(srcs);
    if (vec_stats_) {
        ++vec_stats_->write_vec_calls;
        ++vec_stats_->write_vec_fallback_calls;
        std::uint64_t iovecs = 0;
        for (const auto& s : srcs) if (!s.bytes.empty()) ++iovecs;
        vec_stats_->write_vec_iovecs += iovecs;
        if (r.has_value()) vec_stats_->write_vec_bytes += r.value();
    }
    return r;
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
