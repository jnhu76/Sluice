#include <sluice/async/file.hpp>

#include <sluice/async/await_op_helpers.hpp>

namespace sluice::async {

Result<std::size_t> await_read_at(const File& file, RuntimeTaskContext& ctx, std::uint64_t offset,
                                  std::span<std::byte> dst, Completion<std::size_t>& c) {
    if (!file.is_open()) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
    }
    if (file.access() == FileAccess::write_only) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_argument});
    }
    if (dst.empty()) {
        return std::size_t{0};
    }
    return await_read_once(ctx, file, dst, offset, c);
}

Result<std::size_t> await_write_at(const File& file, RuntimeTaskContext& ctx, std::uint64_t offset,
                                   std::span<const std::byte> src, Completion<std::size_t>& c) {
    if (!file.is_open()) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
    }
    if (file.access() == FileAccess::read_only) {
        return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_argument});
    }
    if (src.empty()) {
        return std::size_t{0};
    }
    auto sr = ctx.submit_write(WriteOp{file, src.data(), src.size(), offset}, c);
    if (!sr.has_value())
        return make_unexpected<std::size_t>(sr.error());
    return await_take(ctx, c);
}

Result<void> await_sync_data(const File& file, RuntimeTaskContext& ctx, Completion<void>& c) {
    if (!file.is_open()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    auto sr = ctx.submit_sync_data(SyncDataOp{file}, c);
    if (!sr.has_value())
        return make_unexpected<void>(sr.error());
    return await_take(ctx, c);
}

Result<void> await_sync_all(const File& file, RuntimeTaskContext& ctx, Completion<void>& c) {
    if (!file.is_open()) {
        return make_unexpected<void>(IoError{IoError::Code::invalid_state});
    }
    auto sr = ctx.submit_sync_all(SyncAllOp{file}, c);
    if (!sr.has_value())
        return make_unexpected<void>(sr.error());
    return await_take(ctx, c);
}

} // namespace sluice::async
