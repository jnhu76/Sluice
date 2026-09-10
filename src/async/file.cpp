#include <sluice/async/file.hpp>

#include <sluice/async/await_op_helpers.hpp>

namespace sluice::async {

Result<std::size_t> await_read_at(const File& file, RuntimeTaskContext& ctx, std::uint64_t offset,
                                  std::span<std::byte> dst, Completion<std::size_t>& c) {
    if (!file.is_open()) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
    }
    if (dst.empty()) {
        return std::size_t{0};
    }
    return await_read_once(ctx, file.native_handle(), dst, offset, c);
}

}
