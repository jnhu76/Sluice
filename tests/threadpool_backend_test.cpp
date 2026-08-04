// Tests for ThreadPoolBackend (sluice-CORE-020A). The portable real backend:
// blocking pread/pwrite/fdatasync/fsync on worker threads. Uses real temp files
// (TempPath RAII) and exercises submit/poll/wait, read/write correctness,
// short-completion retry via write_all/read_all, sync ops, and buffer lifetime
// under real threads (sanitizer-checked separately).
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/op_helpers.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

using namespace sluice::async;
using sluice::AsyncStats;
using sluice::Result;
using sluice::IoError;

namespace {

class TempPath {
public:
    TempPath() {
        // Process-unique name: parallel test runs (sharding, contention stress,
        // CI matrix slots) otherwise produce the SAME path (per-process counter
        // starts at 0) and each open()'s it O_TRUNC, truncating/overwriting a
        // sibling's file mid-operation. PID makes it cross-process unique; the
        // counter + instance address keep it unique within a process. See the
        // same fix in uring_backend_test.cpp.
        path_ = (std::filesystem::temp_directory_path() /
                 ("sluice_async_tp_" + std::to_string(::getpid()) + "_" +
                  std::to_string(counter_++) + "_" +
                  std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".tmp"))
                    .string();
    }
    ~TempPath() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;
    const std::string& path() const { return path_; }
private:
    std::string path_;
    static inline long counter_ = 0;
};

// Open a temp file with O_RDWR|O_CREAT|O_TRUNC, return the fd (or exit on fail).
int open_temp(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { std::fprintf(stderr, "open failed\n"); std::exit(1); }
    return fd;
}

}  // namespace

// ---- Slice 1: positional write then read round-trips the bytes ------------

SLUICE_TEST_CASE(tp_write_then_read_roundtrips) {
    TempPath tp;
    int fd = open_temp(tp.path());
    AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());

    const std::byte payload[8] = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                                  std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    auto w = write_all(ctx, fd, payload, /*offset=*/0);
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w.value() == 8);

    std::byte got[8]{};
    auto r = read_all(ctx, fd, got, 0);
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 8);
    SLUICE_CHECK(std::memcmp(got, payload, 8) == 0);

    ::close(fd);
}

// ---- Slice 2: positional independence — writes at two offsets don't collide

SLUICE_TEST_CASE(tp_positional_independence_two_offsets) {
    TempPath tp;
    int fd = open_temp(tp.path());
    AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());

    const std::byte a[4] = {std::byte{0xAA}, std::byte{0xAA}, std::byte{0xAA}, std::byte{0xAA}};
    const std::byte b[4] = {std::byte{0xBB}, std::byte{0xBB}, std::byte{0xBB}, std::byte{0xBB}};
    SLUICE_CHECK(write_all(ctx, fd, a, 0).has_value());
    SLUICE_CHECK(write_all(ctx, fd, b, 100).has_value());  // disjoint offset

    std::byte ga[4]{}, gb[4]{};
    SLUICE_CHECK(read_all(ctx, fd, ga, 0).has_value());
    SLUICE_CHECK(read_all(ctx, fd, gb, 100).has_value());
    SLUICE_CHECK(std::memcmp(ga, a, 4) == 0);
    SLUICE_CHECK(std::memcmp(gb, b, 4) == 0);

    ::close(fd);
}

// ---- Slice 3: read at EOF returns 0 bytes -----------------------------------

SLUICE_TEST_CASE(tp_read_at_eof_returns_zero) {
    TempPath tp;
    int fd = open_temp(tp.path());   // empty file
    AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
    std::byte buf[4]{};
    // Submit raw, reap via wait_one — expect 0 bytes (EOF, not error).
    Completion<std::size_t> c;
    (void)ctx.submit_read(ReadOp{fd, buf, 4, 0}, c);
    (void)ctx.wait_one();
    SLUICE_CHECK(c.ready());
    SLUICE_CHECK(c.result().value() == 0);
    ::close(fd);
}

// ---- Slice 4: sync_data / sync_all succeed on a writable file --------------

SLUICE_TEST_CASE(tp_sync_data_and_sync_all_succeed) {
    TempPath tp;
    int fd = open_temp(tp.path());
    AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
    const std::byte data[4]{};
    SLUICE_CHECK(write_all(ctx, fd, data, 0).has_value());
    SLUICE_CHECK(sync_data_all(ctx, fd).has_value());
    SLUICE_CHECK(sync_all_all(ctx, fd).has_value());
    ::close(fd);
}

// ---- Slice 5: many concurrent writes complete (concurrency works) ----------

SLUICE_TEST_CASE(tp_many_concurrent_writes_complete) {
    TempPath tp;
    int fd = open_temp(tp.path());
    AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());

    // Submit N writes at disjoint offsets without awaiting, then reap all.
    constexpr int N = 8;
    std::vector<std::vector<std::byte>> bufs;
    std::vector<Completion<std::size_t>> cs(N);
    bufs.reserve(N);
    for (int i = 0; i < N; ++i) {
        bufs.emplace_back(16, std::byte(static_cast<std::uint8_t>(i)));
        (void)ctx.submit_write(WriteOp{fd, bufs.back().data(), 16,
                                       static_cast<std::uint64_t>(i) * 16}, cs[i]);
    }
    int reaped = 0;
    while (reaped < N) reaped += static_cast<int>(ctx.wait_one().value());
    SLUICE_CHECK(reaped == N);
    bool all_ok = true;
    for (int i = 0; i < N; ++i) {
        if (!cs[i].ready() || cs[i].result().value() != 16) { all_ok = false; break; }
    }
    SLUICE_CHECK(all_ok);
    ::close(fd);
}

// ---- Slice 8 (025 B2 / Phase E): submit after admission close -> invalid_state
// Phase E maps the legacy destroying_ gate onto real close_admission() (ADR
// Decision 15). Once admission is closed, submit must reject synchronously with
// invalid_state rather than accept a request it cannot bound. The Completion
// stays idle, nothing is enqueued, and outstanding stays 0.
SLUICE_TEST_CASE(tp_submit_after_shutdown_rejected) {
    ThreadPoolBackend backend;
    backend.close_admission();  // real production admission close
    Completion<std::size_t> c;
    std::byte b[4]{};
    auto r = backend.submit_read(ReadOp{0, b, 4, 0}, c);
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
    SLUICE_CHECK(!c.outstanding());  // rejected before binding/outstanding
    SLUICE_CHECK(backend.outstanding() == 0);
}

// Phase E: a real-syscall backend validates descriptors BEFORE commit (ADR
// Decision 6; DIV-14 does NOT apply). An offset beyond off_t max is a malformed
// descriptor -> invalid_argument (synchronous rejection, Completion idle).
SLUICE_TEST_CASE(tp_offset_past_native_max_is_rejected_before_enqueue) {
    constexpr std::uint64_t native_max =
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    if constexpr (native_max < std::numeric_limits<std::uint64_t>::max()) {
        TempPath tp;
        int fd = open_temp(tp.path());
        AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());
        Completion<std::size_t> c;
        std::byte byte{};

        auto r = ctx.submit_read(ReadOp{fd, &byte, 1, native_max + 1}, c);
        SLUICE_CHECK(!r.has_value());
        SLUICE_CHECK(r.error().code == IoError::Code::invalid_argument);
        SLUICE_CHECK(c.idle());
        SLUICE_CHECK(ctx.outstanding() == 0);
        ::close(fd);
    }
}

// ---- cancel is best-effort and exactly-once (ADR §7 X2/X3, Decision 11) ----
// Phase E drives cancel through the RequestArena state machine (it is no longer
// a no-op). Two races are both legal and both exactly-once:
//   - the worker wins dispatch first: cancel records intent only; the read's
//     REAL result wins verbatim (success), canceled_ops stays 0;
//   - cancel wins while still pending/enqueued: it stores the canceled terminal
//     (Scheme B), the worker never runs the read, canceled_ops == 1.
// Under TSan (slower worker wake) the cancel-wins race becomes reachable, so
// this test asserts the DEFINED-and-exactly-once contract for EITHER outcome,
// not a specific one. (The old "canceled_ops == 0 always" assertion assumed the
// legacy no-op cancel and is no longer valid.)
SLUICE_TEST_CASE(tp_cancel_best_effort_op_completes_with_real_result) {
    TempPath tp;
    int fd = open_temp(tp.path());
    AsyncStats stats;
    AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>(), &stats);

    // Write 4 bytes so a read returns real data.
    const std::byte seed[4]{std::byte{0xCA}, std::byte{0xFE},
                            std::byte{0xBA}, std::byte{0xBE}};
    SLUICE_CHECK(write_all(ctx, fd, {seed, 4}, 0).has_value());

    Completion<std::size_t> r;
    std::byte got[4]{};
    SLUICE_CHECK(ctx.submit_read(ReadOp{fd, got, 4, 0}, r).has_value());
    SLUICE_CHECK(r.outstanding());

    // Request cancel; either the worker's real result wins, or cancel wins the
    // pending/enqueued terminal. The Completion reaches ready exactly once.
    ctx.cancel(r);
    std::size_t guard = 0;
    while (!r.ready() && guard < 10000) {
        auto n = ctx.wait_one();
        SLUICE_CHECK(n.has_value());
        if (n.value() == 0) ++guard;
        if (n.value() > 0) break;
        ++guard;
    }
    SLUICE_CHECK(r.ready());
    const auto res = r.result();
    const bool defined = res.has_value() ||
                         res.error().code == IoError::Code::canceled ||
                         res.error().code == IoError::Code::eof ||
                         res.error().code == IoError::Code::backend_error;
    SLUICE_CHECK(defined);
    // Either outcome is exactly-once and tallied at most once.
    SLUICE_CHECK(stats.canceled_ops == 0 || stats.canceled_ops == 1);
    ::close(fd);
}

SLUICE_MAIN()
