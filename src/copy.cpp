// copy_all implementation.
#include <cppio/copy.hpp>

#include <array>

namespace cppio {

Result<std::uint64_t> copy_all(Reader& reader, Writer& writer, std::span<std::byte> scratch) {
    std::uint64_t total = 0;
    while (true) {
        auto rr = reader.read_some(scratch);
        if (!rr.has_value()) return make_unexpected<std::uint64_t>(rr.error());
        std::size_t got = rr.value();
        if (got == 0) return total;  // clean EOF
        if (got > scratch.size()) {
            return make_unexpected<std::uint64_t>(IoError{IoError::Code::invalid_state});
        }
        auto wr = writer.write_all(std::span<const std::byte>(scratch.data(), got));
        if (!wr.has_value()) return make_unexpected<std::uint64_t>(wr.error());
        total += got;
    }
}

Result<std::uint64_t> copy_all(Reader& reader, Writer& writer) {
    // Small default scratch; size is a correctness default, not tuned.
    std::array<std::byte, 8192> scratch{};
    return copy_all(reader, writer, std::span<std::byte>(scratch));
}

}  // namespace cppio
