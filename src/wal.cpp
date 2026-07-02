// WAL record write/read implementation. Little-endian framing.
#include <cppio/wal.hpp>

#include <array>
#include <cstring>
#include <vector>

namespace cppio::wal {

namespace detail {

// Fits len in a u32 without truncation. Anything > UINT32_MAX cannot be framed
// by the WAL record layout (the length field is 4 bytes LE) and must be
// rejected up-front rather than silently truncated.
Result<std::uint32_t> checked_u32_len(std::size_t len) {
    if (len > static_cast<std::size_t>(UINT32_MAX)) {
        return make_unexpected<std::uint32_t>(IoError{IoError::Code::invalid_state});
    }
    return static_cast<std::uint32_t>(len);
}

}  // namespace detail

namespace {

void put_le_u32(std::byte* p, std::uint32_t v) {
    p[0] = std::byte{(unsigned char)(v & 0xFF)};
    p[1] = std::byte{(unsigned char)((v >> 8) & 0xFF)};
    p[2] = std::byte{(unsigned char)((v >> 16) & 0xFF)};
    p[3] = std::byte{(unsigned char)((v >> 24) & 0xFF)};
}

std::uint32_t get_le_u32(const std::byte* p) {
    return std::uint32_t(std::to_integer<unsigned>(p[0])) |
           (std::uint32_t(std::to_integer<unsigned>(p[1])) << 8) |
           (std::uint32_t(std::to_integer<unsigned>(p[2])) << 16) |
           (std::uint32_t(std::to_integer<unsigned>(p[3])) << 24);
}

std::uint32_t checksum_of(std::span<const std::byte> payload) {
    std::uint64_t sum = 0;
    for (auto b : payload) sum += std::to_integer<unsigned>(b);
    return static_cast<std::uint32_t>(sum & 0xFFFFFFFFu);
}

}  // namespace

Result<void> write_record(Writer& writer, std::span<const std::byte> payload) {
    auto len_res = detail::checked_u32_len(payload.size());
    if (!len_res.has_value()) return make_unexpected<void>(len_res.error());

    std::array<std::byte, 8> header{};
    put_le_u32(header.data(), magic);
    put_le_u32(header.data() + 4, len_res.value());

    auto h = writer.write_all(std::span<const std::byte>(header));
    if (!h.has_value()) return make_unexpected<void>(h.error());

    if (!payload.empty()) {
        auto p = writer.write_all(payload);
        if (!p.has_value()) return make_unexpected<void>(p.error());
    }

    std::array<std::byte, 4> trailer{};
    put_le_u32(trailer.data(), checksum_of(payload));
    auto t = writer.write_all(std::span<const std::byte>(trailer));
    if (!t.has_value()) return make_unexpected<void>(t.error());

    return {};
}

Result<void> write_record_vec(Writer& writer, std::span<const std::byte> payload) {
    auto len_res = detail::checked_u32_len(payload.size());
    if (!len_res.has_value()) return make_unexpected<void>(len_res.error());

    // Frame the record on the stack and emit header|payload|checksum as a single
    // write_all_vec — byte-identical to write_record. The header/trailer arrays
    // outlive the write_all_vec call, so the slices stay valid.
    std::array<std::byte, 8> header{};
    put_le_u32(header.data(), magic);
    put_le_u32(header.data() + 4, len_res.value());

    std::array<std::byte, 4> trailer{};
    put_le_u32(trailer.data(), checksum_of(payload));

    std::vector<ConstIoSlice> slices;
    slices.reserve(payload.empty() ? 2 : 3);
    slices.push_back(ConstIoSlice{std::span<const std::byte>(header)});
    if (!payload.empty()) {
        slices.push_back(ConstIoSlice{payload});
    }
    slices.push_back(ConstIoSlice{std::span<const std::byte>(trailer)});

    return writer.write_all_vec(std::span<const ConstIoSlice>(slices));
}

Result<std::vector<std::byte>> read_record(Reader& reader) {
    std::array<std::byte, 8> header{};
    auto h = reader.read_exact(std::span<std::byte>(header));
    if (!h.has_value()) return make_unexpected<std::vector<std::byte>>(h.error());

    std::uint32_t rec_magic = get_le_u32(header.data());
    std::uint32_t length = get_le_u32(header.data() + 4);
    if (rec_magic != magic) {
        return make_unexpected<std::vector<std::byte>>(
            IoError{IoError::Code::invalid_state});
    }

    std::vector<std::byte> payload(length);
    if (length > 0) {
        auto p = reader.read_exact(std::span<std::byte>(payload));
        if (!p.has_value()) return make_unexpected<std::vector<std::byte>>(p.error());
    }

    std::array<std::byte, 4> trailer{};
    auto t = reader.read_exact(std::span<std::byte>(trailer));
    if (!t.has_value()) return make_unexpected<std::vector<std::byte>>(t.error());

    std::uint32_t stored = get_le_u32(trailer.data());
    if (stored != checksum_of(payload)) {
        return make_unexpected<std::vector<std::byte>>(
            IoError{IoError::Code::invalid_state});
    }
    return payload;
}

}  // namespace cppio::wal
