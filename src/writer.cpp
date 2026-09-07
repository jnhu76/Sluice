#include <sluice/writer.hpp>

#include <cstddef>
#include <vector>

namespace sluice {

Result<void> Writer::write_all(std::span<const std::byte> src) {
    while (!src.empty()) {
        auto r = write_some(src);
        if (!r.has_value()) {
            return make_unexpected(r.error());
        }
        std::size_t n = r.value();
        if (n == 0) {
            return make_unexpected(IoError{.code = IoError::Code::invalid_state});
        }
        if (n > src.size()) {
            return make_unexpected(IoError{.code = IoError::Code::invalid_state});
        }
        src = src.subspan(n);
    }
    return {};
}

Result<std::size_t> Writer::write_vec(std::span<const ConstIoSlice> srcs) {
    std::size_t total = 0;
    for (const auto& s : srcs) {
        if (s.bytes.empty()) {
            continue;
        }
        auto r = write_some(s.bytes);
        if (!r.has_value()) {
            return make_unexpected<std::size_t>(r.error());
        }
        std::size_t n = r.value();
        if (n > s.bytes.size()) {
            return make_unexpected<std::size_t>(IoError{.code = IoError::Code::invalid_state});
        }
        total += n;
        if (n < s.bytes.size()) {
            break;
        }
    }
    return total;
}

Result<void> Writer::write_all_vec(std::span<const ConstIoSlice> srcs) {
    std::size_t idx = 0;
    std::size_t head_offset = 0;
    while (idx < srcs.size()) {
        std::size_t n_remaining = srcs.size() - idx;
        auto drive = [&](std::span<const ConstIoSlice> rem) { return write_vec(rem); };
        Result<std::size_t> r = [&]() -> Result<std::size_t> {
            if (head_offset == 0) {
                return drive(srcs.subspan(idx));
            }
            std::vector<ConstIoSlice> remaining;
            remaining.reserve(n_remaining);
            remaining.push_back(ConstIoSlice{srcs[idx].bytes.subspan(head_offset)});
            for (std::size_t i = idx + 1; i < srcs.size(); ++i) {
                remaining.push_back(srcs[i]);
            }
            return drive(std::span<const ConstIoSlice>(remaining));
        }();
        if (!r.has_value()) {
            return make_unexpected(r.error());
        }
        std::size_t written = r.value();
        if (written == 0) {
            bool any_left = false;
            for (std::size_t i = idx; i < srcs.size(); ++i) {
                std::size_t off = (i == idx) ? head_offset : 0;
                if (srcs[i].bytes.size() > off) {
                    any_left = true;
                    break;
                }
            }
            if (any_left) {
                return make_unexpected(IoError{.code = IoError::Code::invalid_state});
            }
            break;
        }

        while (written > 0 && idx < srcs.size()) {
            std::size_t left_in_slice = srcs[idx].bytes.size() - head_offset;
            if (written >= left_in_slice) {
                written -= left_in_slice;
                ++idx;
                head_offset = 0;
            } else {
                head_offset += written;
                written = 0;
            }
        }
    }
    return {};
}

} // namespace sluice
