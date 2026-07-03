// Implementation of the async "all" helpers (sluice-CORE-018). These block the
// caller in a poll-loop driving one internal Completion until the full buffer
// transfers or an error occurs. Mirrors blocking read_exact/write_all.
#include <sluice/async/op_helpers.hpp>

#include <cassert>

namespace sluice::async {

namespace {
// One step of the loop: submit, poll until ready, return this step's result.
// Resets the Completion so it can be reused for the next step.
Result<std::size_t> one_step(AsyncIoContext& ctx, Completion<std::size_t>& c,
                             int fd, std::byte* dst, const std::byte* src,
                             std::size_t len, std::uint64_t offset) {
    c.reset();
    Result<void> sr = src
        ? ctx.submit_write(WriteOp{fd, src, len, offset}, c)
        : ctx.submit_read(ReadOp{fd, dst, len, offset}, c);
    if (!sr.has_value()) return make_unexpected<std::size_t>(sr.error());

    // Poll until this Completion is ready (drives the backend).
    while (!c.ready()) {
        auto pr = ctx.poll();
        (void)pr;
    }
    return c.result();
}
}  // namespace

Result<std::size_t> read_all(AsyncIoContext& ctx, int fd,
                             std::span<std::byte> dst, std::uint64_t offset) {
    if (dst.empty()) return std::size_t{0};
    Completion<std::size_t> c;
    std::size_t filled = 0;
    std::uint64_t off = offset;
    while (filled < dst.size()) {
        auto r = one_step(ctx, c, fd, dst.data() + filled, nullptr,
                          dst.size() - filled, off);
        if (!r.has_value()) return r;  // error propagates immediately
        std::size_t got = r.value();
        if (got == 0) {
            // EOF before dst full — partial progress or not, it's eof (E4).
            // Zero bytes on non-empty remaining input is EOF here (backend EOF),
            // not invalid_state (that's for write_all).
            return make_unexpected<std::size_t>(IoError{IoError::Code::eof});
        }
        filled += got;
        off += got;
    }
    return filled;
}

Result<std::size_t> write_all(AsyncIoContext& ctx, int fd,
                              std::span<const std::byte> src, std::uint64_t offset) {
    if (src.empty()) return std::size_t{0};
    Completion<std::size_t> c;
    std::size_t written = 0;
    std::uint64_t off = offset;
    while (written < src.size()) {
        auto r = one_step(ctx, c, fd, nullptr, src.data() + written,
                          src.size() - written, off);
        if (!r.has_value()) return r;
        std::size_t put = r.value();
        if (put == 0) {
            // Zero progress on non-empty remaining input — invalid_state.
            return make_unexpected<std::size_t>(IoError{IoError::Code::invalid_state});
        }
        written += put;
        off += put;
    }
    return written;
}

}  // namespace sluice::async
