// Implementation of Writer::write_all — loops over short writes, propagates
// errors, and treats zero progress on non-empty input as invalid_state.
#include <cppio/writer.hpp>

namespace cppio {

Result<void> Writer::write_all(std::span<const std::byte> src) {
    while (!src.empty()) {
        auto r = write_some(src);
        if (!r.has_value()) return make_unexpected(r.error());
        std::size_t n = r.value();
        if (n == 0) {
            return make_unexpected(IoError{IoError::Code::invalid_state});
        }
        if (n > src.size()) {
            // Defensive: a writer returning more than asked is broken.
            return make_unexpected(IoError{IoError::Code::invalid_state});
        }
        src = src.subspan(n);
    }
    return {};
}

}  // namespace cppio
