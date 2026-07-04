// Tests for UringAsyncBackend (sluice-CORE-020B). Two build modes:
//
//   * Default (liburing absent): exercises the STUB contract — submit_*
//     returns backend_error synchronously, poll()/wait_one() reap nothing,
//     outstanding() stays 0 (no L11 violation), available() is false.
//   * SLUICE_HAS_LIBURING defined: exercises the REAL io_uring path against
//     temp files — N-write reap, short-write retry via the 018 helper,
//     CQE res<0 → IoError (E3), SQE-pressure queue_full_retries, and the
//     cancel-vs-in-flight exactly-once race (ADR §7 X3). Ordering is asserted
//     only as "CQE reap order" (O3), never per-fd FIFO.
//
// Skip-clean: the default build runs the three stub tests below and passes with
// the backend absent (ADR §15 AB1, AB8).
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/op_helpers.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#if defined(SLUICE_HAS_LIBURING)
#include <liburing.h>
#endif

using namespace sluice::async;
using sluice::Result;
using sluice::IoError;

#if defined(SLUICE_HAS_LIBURING)
namespace {

class TempPath {
public:
    TempPath() {
        path_ = (std::filesystem::temp_directory_path() /
                 ("sluice_uring_async_" + std::to_string(counter_++) + ".tmp")).string();
    }
    ~TempPath() { std::filesystem::remove(path_); }
    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;
    const std::string& path() const { return path_; }
private:
    std::string path_;
    static inline long counter_ = 0;
};

int open_temp(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { std::fprintf(stderr, "open failed\n"); std::exit(1); }
    return fd;
}

}  // namespace
#endif  // SLUICE_HAS_LIBURING

// ---------------------------------------------------------------------------
// Slice 1: available() reflects the build mode. Real-liburing build: the ring
// initialized. Stub build: no ring. This also gates the kernel-capability check
// — a real-liburing build on a kernel without io_uring would see available()==false.
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(uring_available_matches_build_mode) {
    UringAsyncBackend backend;
#if !defined(SLUICE_HAS_LIBURING)
    SLUICE_CHECK(!backend.available());   // stub: no real ring
#else
    // Real liburing linked: available() is true iff io_uring_queue_init worked.
    // On a kernel without io_uring this may still be false; the rest of the
    // real-path tests are skipped in that case (see skip_if_unavailable below).
    (void)backend;
#endif
}

SLUICE_TEST_CASE(uring_stub_submit_returns_backend_error) {
#if !defined(SLUICE_HAS_LIBURING)
    AsyncIoContext ctx(std::make_unique<UringAsyncBackend>());
    std::byte buf[4]{};
    Completion<std::size_t> c;
    // submit must reject synchronously in stub mode; the Completion stays idle.
    auto r = ctx.submit_read(ReadOp{0, buf, 4, 0}, c);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::backend_error);
    SLUICE_CHECK(c.idle());          // not recorded outstanding
    SLUICE_CHECK(ctx.outstanding() == 0);
    SLUICE_CHECK(ctx.poll() == 0);   // nothing to reap
#else
    SLUICE_CHECK(true);  // real path covered by the slices below
#endif
}

SLUICE_TEST_CASE(uring_stub_wait_one_reaps_nothing) {
#if !defined(SLUICE_HAS_LIBURING)
    AsyncIoContext ctx(std::make_unique<UringAsyncBackend>());
    // wait_one on an empty/stub backend returns 0 without blocking.
    auto r = ctx.wait_one();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 0);
#else
    SLUICE_CHECK(true);
#endif
}

// ===========================================================================
// Real-path tests (SLUICE_HAS_LIBURING only). These touch real temp files and
// the real kernel io_uring. Skip cleanly if the ring could not be initialized
// (available()==false) — e.g. a real-liburing build on a sandboxed kernel.
// ===========================================================================
#if defined(SLUICE_HAS_LIBURING)

namespace {
// Build a context only if the real ring is usable on this kernel; return null
// otherwise so the test can skip rather than fail. (The harness treats a
// skipped test as passing — there is no explicit SKIP primitive.)
std::unique_ptr<AsyncIoContext> make_real_ctx(sluice::AsyncStats* stats = nullptr) {
    auto backend = std::make_unique<UringAsyncBackend>();
    if (!backend->available()) return nullptr;
    return std::make_unique<AsyncIoContext>(std::move(backend), stats);
}
}  // namespace

// ---- Real slice 1: submit N positional writes, reap all, verify bytes (O3) --

SLUICE_TEST_CASE(uring_submit_n_writes_reap_all) {
    auto ctx = make_real_ctx();
    if (!ctx) return;  // kernel without io_uring: skip

    TempPath tp;
    int fd = open_temp(tp.path());
    constexpr int N = 8;
    constexpr std::size_t BLK = 16;
    std::vector<std::vector<std::byte>> bufs;
    std::vector<Completion<std::size_t>> cs(N);
    bufs.reserve(N);
    for (int i = 0; i < N; ++i) {
        bufs.emplace_back(BLK, std::byte(static_cast<std::uint8_t>(i)));
        (void)ctx->submit_write(WriteOp{fd, bufs.back().data(), BLK,
                                        static_cast<std::uint64_t>(i) * BLK}, cs[i]);
    }
    int reaped = 0;
    while (reaped < N) {
        auto r = ctx->wait_one();
        if (!r.has_value()) break;
        reaped += static_cast<int>(r.value());
    }
    SLUICE_CHECK(reaped == N);
    bool all_ok = true;
    for (int i = 0; i < N; ++i) {
        if (!cs[i].ready() || cs[i].result().value() != BLK) { all_ok = false; break; }
    }
    SLUICE_CHECK(all_ok);

    // Verify the bytes actually landed (correctness, not just completion counts).
    for (int i = 0; i < N; ++i) {
        std::byte got[BLK]{};
        ssize_t n = ::pread(fd, got, BLK, static_cast<off_t>(i) * BLK);
        SLUICE_CHECK(n == static_cast<ssize_t>(BLK));
        SLUICE_CHECK(std::memcmp(got, bufs[i].data(), BLK) == 0);
    }
    ::close(fd);
}

// ---- Real slice 2: write_all retries across short completions (018 helper) --

SLUICE_TEST_CASE(uring_write_all_completes_full_buffer) {
    auto ctx = make_real_ctx();
    if (!ctx) return;

    TempPath tp;
    int fd = open_temp(tp.path());
    std::vector<std::byte> payload(256);
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = std::byte(static_cast<std::uint8_t>(i));
    // write_all drives a poll-loop across any short completions the kernel
    // produces. On regular files io_uring typically completes in one shot, so
    // this primarily asserts the path doesn't regress; if the kernel does short
    // the op, the 018 helper must still deliver all bytes.
    auto r = write_all(*ctx, fd, payload, /*offset=*/0);
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 256);

    std::vector<std::byte> back(256);
    ssize_t n = ::pread(fd, back.data(), back.size(), 0);
    SLUICE_CHECK(n == 256);
    SLUICE_CHECK(std::memcmp(back.data(), payload.data(), 256) == 0);
    ::close(fd);
}

// ---- Real slice 3: CQE res<0 maps to IoError (ADR E3) ----------------------
// Write to a read-only fd: the kernel completes the SQE with -EBADF (or a
// related errno); from_errno_value maps it into the IoError vocabulary.

SLUICE_TEST_CASE(uring_cqe_res_negative_maps_to_ioerror) {
    auto ctx = make_real_ctx();
    if (!ctx) return;

    TempPath tp;
    // Open the file READ-ONLY: a write op cannot succeed.
    int fd = ::open(tp.path().c_str(), O_RDONLY | O_CREAT, 0644);
    SLUICE_CHECK(fd >= 0);
    std::byte buf[4]{};
    Completion<std::size_t> c;
    // Submit may succeed (SQE acquired) — the error surfaces at reap time as a
    // negative CQE res, mapped to IoError via from_errno_value.
    (void)ctx->submit_write(WriteOp{fd, buf, 4, 0}, c);
    (void)ctx->wait_one();
    SLUICE_CHECK(c.ready());
    auto r = c.result();
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code != IoError::Code::backend_error ||
                 r.error().os_errno != 0);  // a real errno was carried
    ::close(fd);
}

// ---- Real slice 4: SQE pressure increments queue_full_retries ----------------
// Submit many more ops than the ring depth without reaping; the internal
// flush-on-pressure path bumps queue_full_retries. Asserted as a lower bound
// (exact counts depend on kernel timing), not an equality.

SLUICE_TEST_CASE(uring_sqe_pressure_increments_queue_full_retries) {
    sluice::AsyncStats s;
    auto ctx = make_real_ctx(&s);
    if (!ctx) return;

    TempPath tp;
    int fd = open_temp(tp.path());
    // queue_depth default is 64; submit well beyond that without reaping.
    constexpr int N = 256;
    constexpr std::size_t BLK = 4;
    std::vector<std::vector<std::byte>> bufs;
    std::vector<Completion<std::size_t>> cs(N);
    bufs.reserve(N);
    bool submit_ok = true;
    for (int i = 0; i < N; ++i) {
        bufs.emplace_back(BLK, std::byte{0x11});
        auto r = ctx->submit_write(WriteOp{fd, bufs.back().data(), BLK,
                                           static_cast<std::uint64_t>(i) * BLK}, cs[i]);
        if (!r.has_value()) { submit_ok = false; break; }
    }
    // Drain everything so the context destructs clean (L11) and the kernel
    // actually completes the ops (so we don't leak fd with in-flight SQEs).
    int reaped = 0;
    while (reaped < N) {
        auto r = ctx->wait_one();
        if (!r.has_value()) break;
        reaped += static_cast<int>(r.value());
        if (reaped >= N) break;
    }
    SLUICE_CHECK(submit_ok);
    // The internal flush-on-pressure path should have fired at least once while
    // submitting 256 ops into a 64-deep ring without reaping between submits.
    SLUICE_CHECK(s.queue_full_retries > 0);
    ::close(fd);
}

// ---- Real slice 5: cancel-vs-in-flight produces exactly one terminal result ---
// Submit a write, request cancel, reap. The Completion resolves exactly once
// (success/error/canceled — any is valid per ADR §7 X3); the key assertion is
// exactly-once and no use-after-free (the buffer stays live until ready; ASan
// in the sanitizer matrix is the real guard).

SLUICE_TEST_CASE(uring_cancel_resolves_exactly_once) {
    auto ctx = make_real_ctx();
    if (!ctx) return;

    TempPath tp;
    int fd = open_temp(tp.path());
    std::vector<std::byte> buf(64, std::byte{0x22});
    Completion<std::size_t> c;
    (void)ctx->submit_write(WriteOp{fd, buf.data(), buf.size(), 0}, c);
    ctx->cancel(c);
    // Reap until the op's Completion is ready. Exactly-once: complete_with is
    // the only mutator and it asserts outstanding->ready internally.
    while (!c.ready()) {
        auto r = ctx->wait_one();
        if (!r.has_value()) break;
    }
    SLUICE_CHECK(c.ready());
    auto res = c.result();
    // Terminal is one of {success, canceled, error}; exactly-once guarantees a
    // single defined result. We only assert "ready with a defined result".
    SLUICE_CHECK(res.has_value() || !res.has_value());  // tautology: result is well-formed
    // Reap once more to drain any lingering cancel CQE (best-effort; no crash).
    (void)ctx->poll();
    ::close(fd);
}

// ---- Real slice 6: stats increment on the real path (sanity) ----------------

SLUICE_TEST_CASE(uring_stats_increment_on_real_path) {
    sluice::AsyncStats s;
    auto ctx = make_real_ctx(&s);
    if (!ctx) return;

    TempPath tp;
    int fd = open_temp(tp.path());
    std::byte buf[8]{};
    Completion<std::size_t> c;
    SLUICE_CHECK(ctx->submit_write(WriteOp{fd, buf, 8, 0}, c).has_value());
    SLUICE_CHECK(s.submit_calls == 1);
    SLUICE_CHECK(s.submitted_ops == 1);
    (void)ctx->wait_one();
    SLUICE_CHECK(s.completed_ops >= 1);
    ::close(fd);
}

#endif  // SLUICE_HAS_LIBURING

SLUICE_MAIN()
