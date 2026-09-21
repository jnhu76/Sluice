#include <sluice/async/file.hpp>

#include <sluice/async/await_op_helpers.hpp>
#include <sluice/detail/file_semantics.hpp>

namespace sluice::async {

Result<std::size_t> await_read_at(const File& file, RuntimeTaskContext& ctx, std::uint64_t offset,
                                  std::span<std::byte> dst, Completion<std::size_t>& c) {
    const auto verdict = sluice::detail::precheck_data_op({!file.is_open(), file.access(),
                                                  sluice::detail::FileOperation::read, offset, dst.size(),
                                                  dst.data() != nullptr});
    if (auto rejection = sluice::detail::rejection_of(verdict); rejection.has_value()) {
        return make_unexpected<std::size_t>(*rejection);
    }
    if (verdict == sluice::detail::DataOpVerdict::complete_empty) {
        return std::size_t{0};
    }
    return await_read_once(ctx, file, dst, offset, c);
}

Result<std::size_t> await_write_at(const File& file, RuntimeTaskContext& ctx, std::uint64_t offset,
                                   std::span<const std::byte> src, Completion<std::size_t>& c) {
    const auto verdict = sluice::detail::precheck_data_op({!file.is_open(), file.access(),
                                                  sluice::detail::FileOperation::write, offset, src.size(),
                                                  src.data() != nullptr});
    if (auto rejection = sluice::detail::rejection_of(verdict); rejection.has_value()) {
        return make_unexpected<std::size_t>(*rejection);
    }
    if (verdict == sluice::detail::DataOpVerdict::complete_empty) {
        return std::size_t{0};
    }
    auto sr = ctx.submit_write(WriteOp{file, src.data(), src.size(), offset}, c);
    if (!sr.has_value())
        return make_unexpected<std::size_t>(sr.error());
    return await_take(ctx, c);
}

Result<void> await_sync_data(const File& file, RuntimeTaskContext& ctx, Completion<void>& c) {
    if (auto rejection = sluice::detail::rejection_of(sluice::detail::precheck_state_op(
            !file.is_open(), file.access(), sluice::detail::FileOperation::sync_data));
        rejection.has_value()) {
        return make_unexpected<void>(*rejection);
    }
    auto sr = ctx.submit_sync_data(SyncDataOp{file}, c);
    if (!sr.has_value())
        return make_unexpected<void>(sr.error());
    return await_take(ctx, c);
}

Result<void> await_sync_all(const File& file, RuntimeTaskContext& ctx, Completion<void>& c) {
    if (auto rejection = sluice::detail::rejection_of(sluice::detail::precheck_state_op(
            !file.is_open(), file.access(), sluice::detail::FileOperation::sync_all));
        rejection.has_value()) {
        return make_unexpected<void>(*rejection);
    }
    auto sr = ctx.submit_sync_all(SyncAllOp{file}, c);
    if (!sr.has_value())
        return make_unexpected<void>(sr.error());
    return await_take(ctx, c);
}

} // namespace sluice::async
