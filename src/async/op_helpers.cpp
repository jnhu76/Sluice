#include <sluice/async/op_helpers.hpp>

#include <cassert>

namespace sluice::async {

namespace {

Result<std::size_t> one_step(AsyncIoContext& ctx, Completion<std::size_t>& c, int fd,
                             std::byte* dst, const std::byte* src, std::size_t len,
                             std::uint64_t offset) {
    c.reset();
    Result<void> sr = src ? ctx.submit_write(WriteOp{fd, src, len, offset}, c)
                          : ctx.submit_read(ReadOp{fd, dst, len, offset}, c);
    if (!sr.has_value())
        return make_unexpected<std::size_t>(sr.error());

    while (!c.ready()) {
        auto pr = ctx.poll();
        (void)pr;
    }
    return c.result();
}
} // namespace

Result<std::size_t> read_all(AsyncIoContext& ctx, int fd, std::span<std::byte> dst,
                             std::uint64_t offset) {
    if (dst.empty())
        return std::size_t{0};
    Completion<std::size_t> c;
    std::size_t filled = 0;
    std::uint64_t off = offset;
    while (filled < dst.size()) {
        auto r = one_step(ctx, c, fd, dst.data() + filled, nullptr, dst.size() - filled, off);
        if (!r.has_value())
            return r;
        std::size_t got = r.value();
        if (got == 0) {
            return make_unexpected<std::size_t>(IoError{IoError::Code::eof});
        }
        filled += got;
        off += got;
    }
    return filled;
}

Result<std::size_t> write_all(AsyncIoContext& ctx, int fd, std::span<const std::byte> src,
                              std::uint64_t offset) {
    if (src.empty())
        return std::size_t{0};
    Completion<std::size_t> c;
    std::size_t written = 0;
    std::uint64_t off = offset;
    while (written < src.size()) {
        auto r = one_step(ctx, c, fd, nullptr, src.data() + written, src.size() - written, off);
        if (!r.has_value())
            return r;
        std::size_t put = r.value();
        if (put == 0) {
            return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
        }
        written += put;
        off += put;
    }
    return written;
}

namespace {

Result<void> sync_step(AsyncIoContext& ctx, Completion<void>& c, int fd, bool data_only) {
    c.reset();
    Result<void> sr =
        data_only ? ctx.submit_sync_data(SyncDataOp{fd}, c) : ctx.submit_sync_all(SyncAllOp{fd}, c);
    if (!sr.has_value())
        return sr;
    while (!c.ready()) {
        (void)ctx.poll();
    }
    return c.result();
}
} // namespace

Result<void> sync_data_all(AsyncIoContext& ctx, int fd) {
    Completion<void> c;
    return sync_step(ctx, c, fd, true);
}

Result<void> sync_all_all(AsyncIoContext& ctx, int fd) {
    Completion<void> c;
    return sync_step(ctx, c, fd, false);
}

} // namespace sluice::async
