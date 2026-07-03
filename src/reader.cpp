// Implementation of Reader::read_exact and Reader::stream_to.
#include <cppio/reader.hpp>
#include <cppio/writer.hpp>
#include <cppio/copy.hpp>

#include <array>
#include <cstddef>

namespace cppio {

Result<void> Reader::read_exact(std::span<std::byte> dst) {
    while (!dst.empty()) {
        auto r = read_some(dst);
        if (!r.has_value()) return make_unexpected<void>(r.error());
        std::size_t n = r.value();
        if (n == 0) {
            // No progress and no error => EOF before dst was filled.
            return make_unexpected<void>(IoError{IoError::Code::eof});
        }
        if (n > dst.size()) {
            return make_unexpected<void>(IoError{IoError::Code::invalid_state});
        }
        dst = dst.subspan(n);
    }
    return {};
}

Result<std::size_t> Reader::stream_to(Writer& writer) {
    std::size_t total = 0;
    // Small stack buffer; this is the simple, correct implementation.
    // Optimization (zero-copy, larger buffers) is intentionally deferred.
    std::array<std::byte, 8192> buf{};
    while (true) {
        auto rr = read_some(std::span<std::byte>(buf));
        if (!rr.has_value()) return make_unexpected<std::size_t>(rr.error());
        std::size_t got = rr.value();
        if (got == 0) return total;  // clean EOF
        auto wr = writer.write_all(std::span<const std::byte>(buf.data(), got));
        if (!wr.has_value()) return make_unexpected<std::size_t>(wr.error());
        total += got;
    }
}

Result<std::uint64_t> Reader::stream_to(Writer& writer, std::span<std::byte> scratch,
                                        CopyLimit limit, CopyStats* stats) {
    // Delegate to copy_all so the bounded-copy semantics live in exactly one
    // place (no duplicated loop, no drift between stream_to and copy_all).
    return copy_all(*this, writer, scratch, limit, stats);
}

Result<std::uint64_t> Reader::stream_to(Writer& writer, CopyLimit limit) {
    return copy_all(*this, writer, limit);
}

// Default vector fallback: drive each non-empty slice through read_some in
// order. A conservative vector primitive (CPPIO-CORE-005B): like a single
// readv-style operation, it STOPS as soon as a slice is not fully satisfied —
// i.e. on a clean EOF (n==0) OR on a positive short read (0 < n < slice size).
// An error is propagated immediately, even after partial progress (consistent
// with read_exact and write_all). This deliberately does NOT behave like
// read_exact over slices (which would keep filling the same slice on a short
// read); that is the job of a future read_exact_vec. EOF before any progress
// returns 0; EOF/error after progress returns the progress count / the error.
Result<std::size_t> Reader::read_vec(std::span<IoSlice> dsts) {
    std::size_t total = 0;
    for (auto& d : dsts) {
        if (d.bytes.empty()) continue;  // empty slices skipped, no read
        auto r = read_some(d.bytes);
        if (!r.has_value()) return make_unexpected<std::size_t>(r.error());
        std::size_t n = r.value();
        if (n > d.bytes.size()) {
            // Defensive: a reader returning more than asked is broken.
            return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
        }
        // Both EOF (n==0) and a positive short read (0 < n < size) mean the
        // slice was not fully satisfied: stop and report the partial total.
        total += n;
        if (n < d.bytes.size()) return total;  // EOF or short read: stop
    }
    return total;
}

// read_vec_all: exact-read over slices. Unlike read_vec (which stops as soon as
// a slice is not fully satisfied), this keeps retrying the SAME slice's unfilled
// tail until it is complete, then moves to the next. Symmetric to
// Writer::write_all_vec. EOF before all non-empty slices are full -> eof; other
// errors propagate immediately (matching read_exact). Empty slices are skipped
// (and never cause a read). Partial progress is observable in the caller's
// buffers even on failure.
Result<void> Reader::read_vec_all(std::span<IoSlice> dsts) {
    for (auto& d : dsts) {
        if (d.bytes.empty()) continue;  // empty slices skipped, no read
        std::size_t filled = 0;
        while (filled < d.bytes.size()) {
            auto r = read_some(d.bytes.subspan(filled));
            if (!r.has_value()) return make_unexpected<void>(r.error());
            std::size_t n = r.value();
            if (n > d.bytes.size() - filled) {
                // Defensive: a reader returning more than asked is broken.
                return make_unexpected<void>(IoError{IoError::Code::invalid_state});
            }
            if (n == 0) {
                // Clean EOF before this slice was filled.
                return make_unexpected<void>(IoError{IoError::Code::eof});
            }
            filled += n;
        }
    }
    return {};
}

}  // namespace cppio
