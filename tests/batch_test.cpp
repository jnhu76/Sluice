// Tests for sluice::async::Batch (sluice-CORE-030, T4).
//
// Grouped completions over AsyncIoContext, derived from Zig std.Io Batch
// (Io.zig:474-624). Each case asserts ONE Batch semantic, TDD-vertical.
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/batch.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

namespace {
int make_temp_fd() {
    static long c = 0;
    auto path = std::filesystem::temp_directory_path() /
                ("sluice_batch_" + std::to_string(c++) + ".tmp");
    int fd = ::open(path.string().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) std::filesystem::remove(path);
    return fd;
}
}  // namespace

// ---- Slice 1 (tracer): add 2 writes, await_one, next yields both -----------
// The core shape: add N ops, await >=1, iterate next() to completion order.
SLUICE_TEST_CASE(batch_add_await_next_yields_all_completions) {
    const int fd = make_temp_fd();
    AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());

    Batch b;
    const std::byte a[4]{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    const std::byte c[4]{std::byte{0x55}, std::byte{0x66}, std::byte{0x77}, std::byte{0x88}};
    BatchOp op_a; op_a.kind = BatchOp::Kind::write;
    op_a.write = WriteOp{fd, a, 4, 0};
    BatchOp op_c; op_c.kind = BatchOp::Kind::write;
    op_c.write = WriteOp{fd, c, 4, 4};
    const std::size_t ia = b.add(op_a);
    const std::size_t ic = b.add(op_c);
    SLUICE_CHECK(ia == 0);
    SLUICE_CHECK(ic == 1);

    // await_one() returns >=1 ready (Zig Io.zig:578: "wait for at least one").
    // Loop await_one/drain-next until both completions are reaped OR nothing is
    // outstanding. Stop if await_one yields no new completion AND nothing is
    // outstanding (avoids a blocking wait_one on an empty backend).
    int seen_a = 0, seen_c = 0, count = 0;
    while (count < 2) {
        std::size_t ready = b.await_one(ctx);
        SLUICE_CHECK(ready >= 1);
        std::size_t popped_now = 0;
        while (auto r = b.next()) {
            ++count; ++popped_now;
            SLUICE_CHECK(!r->is_void);
            SLUICE_CHECK(r->size_res.has_value());
            SLUICE_CHECK(r->size_res.value().has_value());
            SLUICE_CHECK(r->size_res.value().value() == 4);
            if (r->index == ia) ++seen_a;
            else if (r->index == ic) ++seen_c;
        }
        // If await_one surfaced no NEW completion and nothing remains
        // outstanding, we are done — break to avoid a blocking wait_one.
        if (popped_now == 0 && ctx.outstanding() == 0) break;
    }
    SLUICE_CHECK(count == 2);
    SLUICE_CHECK(seen_a == 1);
    SLUICE_CHECK(seen_c == 1);
    SLUICE_CHECK(b.next() == std::nullopt);  // drained

    // Verify bytes on disk via a positional read back (independent of batch).
    std::byte got[8]{};
    ::lseek(fd, 0, SEEK_SET);
    SLUICE_CHECK(::read(fd, got, 8) == 8);
    SLUICE_CHECK(std::memcmp(got, a, 4) == 0);
    SLUICE_CHECK(std::memcmp(got + 4, c, 4) == 0);
    ::close(fd);
}

// ---- Slice 2: mixed-kind batch (read + write + sync) ---------------------
// A batch may mix op kinds (read/write/sync_data/sync_all); each completes via
// its own Completion type. The batch surfaces the right result variant per op.
SLUICE_TEST_CASE(batch_mixed_kinds_complete_independently) {
    const int fd = make_temp_fd();
    AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>());

    // Seed the file with 4 bytes via a write.
    const std::byte seed[4]{std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD}};
    Batch seed_batch;
    BatchOp w; w.kind = BatchOp::Kind::write; w.write = WriteOp{fd, seed, 4, 0};
    const std::size_t iw = seed_batch.add(w);
    (void)seed_batch.await_one(ctx);
    auto wr = seed_batch.next();
    SLUICE_CHECK(wr.has_value());
    SLUICE_CHECK(wr->index == iw);
    SLUICE_CHECK(!wr->is_void);
    SLUICE_CHECK(wr->size_res.value().value() == 4);

    // Now a mixed batch: read the 4 bytes, then sync_all.
    Batch b;
    BatchOp r_op; r_op.kind = BatchOp::Kind::read;
    std::byte got[4]{};
    r_op.read = ReadOp{fd, got, 4, 0};
    BatchOp s_op; s_op.kind = BatchOp::Kind::sync_all;
    s_op.sync_all = SyncAllOp{fd};
    const std::size_t ir = b.add(r_op);
    const std::size_t is = b.add(s_op);

    int reads = 0, syncs = 0, count = 0;
    while (count < 2) {
        std::size_t ready = b.await_one(ctx);
        SLUICE_CHECK(ready >= 1);
        std::size_t popped_now = 0;
        while (auto r = b.next()) {
            ++count; ++popped_now;
            if (r->index == ir) {
                ++reads;
                SLUICE_CHECK(r->is_void == false);
                SLUICE_CHECK(r->size_res.value().value() == 4);
            } else if (r->index == is) {
                ++syncs;
                SLUICE_CHECK(r->is_void == true);
                SLUICE_CHECK(r->void_res.value().has_value());  // sync ok
            }
        }
        if (popped_now == 0 && ctx.outstanding() == 0) break;
    }
    SLUICE_CHECK(reads == 1);
    SLUICE_CHECK(syncs == 1);
    SLUICE_CHECK(std::memcmp(got, seed, 4) == 0);
    ::close(fd);
}

SLUICE_MAIN()
