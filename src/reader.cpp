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

}  // namespace cppio
