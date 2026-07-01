// copy_all implementation. The primary overload drives the bounded loop; the
// back-compat / convenience overloads delegate to it.
#include <cppio/copy.hpp>

#include <algorithm>
#include <array>

namespace cppio {

Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, std::span<std::byte> scratch,
                               CopyLimit limit) {
    // nothing() (and bytes(0)): succeed immediately without touching endpoints.
    if (limit.is_limited() && limit.remaining() == 0) {
        return std::uint64_t{0};
    }

    // A non-zero / unlimited copy needs somewhere to stage bytes. An empty
    // scratch can never make progress, so reject rather than spin or no-op.
    if (scratch.empty()) {
        return make_unexpected<std::uint64_t>(IoError{IoError::Code::invalid_state});
    }

    std::uint64_t total = 0;
    while (limit.is_unlimited() || total < limit.remaining()) {
        // Never ask the reader for more than the remaining limit allows, and
        // never more than the scratch can hold.
        std::size_t to_read = scratch.size();
        if (limit.is_limited()) {
            std::uint64_t left = limit.remaining() - total;
            to_read = static_cast<std::size_t>(std::min<std::uint64_t>(scratch.size(), left));
        }

        auto rr = reader.read_some(scratch.first(to_read));
        if (!rr.has_value()) return make_unexpected<std::uint64_t>(rr.error());
        std::size_t got = rr.value();
        if (got == 0) return total;  // clean EOF
        if (got > to_read) {
            // Defensive: a reader returning more than asked is broken.
            return make_unexpected<std::uint64_t>(IoError{IoError::Code::invalid_state});
        }
        auto wr = writer.write_all(std::span<const std::byte>(scratch.data(), got));
        if (!wr.has_value()) return make_unexpected<std::uint64_t>(wr.error());
        total += got;
    }
    return total;
}

Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, std::span<std::byte> scratch) {
    return copy_all(reader, writer, scratch, CopyLimit::unlimited());
}

Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, CopyLimit limit) {
    std::array<std::byte, 8192> scratch{};
    return copy_all(reader, writer, std::span<std::byte>(scratch), limit);
}

Result<std::uint64_t> copy_all(Reader& reader, Writer& writer) {
    return copy_all(reader, writer, CopyLimit::unlimited());
}

}  // namespace cppio
