// sluice::async await-style operation helpers — implementation.
// See include/sluice/async/await_op_helpers.hpp for the contract.
#include <sluice/async/await_op_helpers.hpp>

namespace sluice::async {

Result<std::size_t> await_take(RuntimeTaskContext& ctx,
                               Completion<std::size_t>& c) {
    auto wr = ctx.await_completion(c);
    if (!wr.has_value()) return make_unexpected<std::size_t>(wr.error());
    auto rr = c.result();
    c.reset();
    return rr;
}

Result<void> await_take(RuntimeTaskContext& ctx, Completion<void>& c) {
    auto wr = ctx.await_completion(c);
    if (!wr.has_value()) return make_unexpected<void>(wr.error());
    auto rr = c.result();
    c.reset();
    return rr;
}

Result<void> await_drain(RuntimeTaskContext& ctx, Completion<std::size_t>& c) {
    auto wr = ctx.await_completion(c);
    if (!wr.has_value()) return make_unexpected<void>(wr.error());
    (void)c.result();  // consume; secondary terminal outcomes are discarded
    c.reset();
    return {};
}

Result<std::size_t> await_read_once(RuntimeTaskContext& ctx, int fd,
                                    std::span<std::byte> dst,
                                    std::uint64_t offset,
                                    Completion<std::size_t>& c) {
    auto sr = ctx.submit_read(ReadOp{fd, dst.data(), dst.size(), offset}, c);
    if (!sr.has_value()) return make_unexpected<std::size_t>(sr.error());
    return await_take(ctx, c);
}

Result<std::size_t> await_read_fill(RuntimeTaskContext& ctx, int fd,
                                    std::span<std::byte> dst,
                                    std::uint64_t offset,
                                    Completion<std::size_t>& c,
                                    AwaitOpTally* tally) {
    std::size_t filled = 0;
    while (filled < dst.size()) {
        auto rr = await_read_once(
            ctx, fd, std::span<std::byte>(dst.data() + filled, dst.size() - filled),
            offset + filled, c);
        if (!rr.has_value()) return rr;
        if (tally) {
            ++tally->ops;
            if (rr.value() < dst.size() - filled) ++tally->short_ops;
        }
        if (rr.value() == 0) return filled;  // EOF: a partial tail is data.
        filled += rr.value();
    }
    return filled;
}

Result<std::size_t> await_write_exact(RuntimeTaskContext& ctx, int fd,
                                      std::span<const std::byte> src,
                                      std::uint64_t offset,
                                      Completion<std::size_t>& c,
                                      AwaitOpTally* tally) {
    std::size_t written = 0;
    while (written < src.size()) {
        std::size_t remaining = src.size() - written;
        auto sr = ctx.submit_write(
            WriteOp{fd, src.data() + written, remaining, offset + written}, c);
        if (!sr.has_value()) return make_unexpected<std::size_t>(sr.error());
        auto wr = ctx.await_completion(c);
        if (!wr.has_value()) return make_unexpected<std::size_t>(wr.error());
        auto rr = c.result();
        c.reset();
        if (!rr.has_value()) return rr;
        std::size_t wrote = rr.value();
        if (tally) {
            ++tally->ops;
            if (wrote < remaining) ++tally->short_ops;
        }
        if (wrote == 0) {
            // Zero progress on a non-empty write: deterministic error.
            return make_unexpected<std::size_t>(
                IoError{IoError::Code::backend_error});
        }
        written += wrote;
    }
    return written;
}

}  // namespace sluice::async
