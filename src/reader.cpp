
#include <sluice/reader.hpp>
#include <sluice/writer.hpp>
#include <sluice/copy.hpp>

#include <array>
#include <cstddef>

namespace sluice {

Result<void> Reader::read_exact(std::span<std::byte> dst) {
    while (!dst.empty()) {
        auto r = read_some(dst);
        if (!r.has_value()) {
            return make_unexpected<void>(r.error());
        }
        std::size_t n = r.value();
        if (n == 0) {

            return make_unexpected<void>(IoError{.code = IoError::Code::eof});
        }
        if (n > dst.size()) {
            return make_unexpected<void>(IoError{.code = IoError::Code::invalid_state});
        }
        dst = dst.subspan(n);
    }
    return {};
}

Result<std::size_t> Reader::stream_to(Writer& writer) {
    std::size_t total = 0;


    std::array<std::byte, 8192> buf{};
    while (true) {
        auto rr = read_some(std::span<std::byte>(buf));
        if (!rr.has_value()) {
            return make_unexpected<std::size_t>(rr.error());
        }
        std::size_t got = rr.value();
        if (got == 0) {
            return total;
        }
        auto wr = writer.write_all(std::span<const std::byte>(buf.data(), got));
        if (!wr.has_value()) {
            return make_unexpected<std::size_t>(wr.error());
        }
        total += got;
    }
}

Result<std::uint64_t> Reader::stream_to(Writer& writer, std::span<std::byte> scratch,
                                        CopyLimit limit, CopyStats* stats) {


    return copy_all(*this, writer, scratch, limit, stats);
}

Result<std::uint64_t> Reader::stream_to(Writer& writer, CopyLimit limit) {
    return copy_all(*this, writer, limit);
}










Result<std::size_t> Reader::read_vec(std::span<IoSlice> dsts) {
    std::size_t total = 0;
    for (auto& d : dsts) {
        if (d.bytes.empty()) {
            continue;
        }
        auto r = read_some(d.bytes);
        if (!r.has_value()) {
            return make_unexpected<std::size_t>(r.error());
        }
        std::size_t n = r.value();
        if (n > d.bytes.size()) {

            return make_unexpected<std::size_t>(IoError{.code = IoError::Code::invalid_state});
        }


        total += n;
        if (n < d.bytes.size()) {
            return total;
        }
    }
    return total;
}








Result<void> Reader::read_vec_all(std::span<IoSlice> dsts) {
    for (auto& d : dsts) {
        if (d.bytes.empty()) {
            continue;
        }
        std::size_t filled = 0;
        while (filled < d.bytes.size()) {
            auto r = read_some(d.bytes.subspan(filled));
            if (!r.has_value()) {
                return make_unexpected<void>(r.error());
            }
            std::size_t n = r.value();
            if (n > d.bytes.size() - filled) {

                return make_unexpected<void>(IoError{.code = IoError::Code::invalid_state});
            }
            if (n == 0) {

                return make_unexpected<void>(IoError{.code = IoError::Code::eof});
            }
            filled += n;
        }
    }
    return {};
}

}
