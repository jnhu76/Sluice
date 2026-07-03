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
#include <sluice/result.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace sluice::async;
using sluice::Result;
using sluice::IoError;

namespace {

class TempPath {
public:
    TempPath() {
        path_ = (std::filesystem::temp_directory_path() /
                 ("sluice_async_tp_" + std::to_string(counter_++) + ".tmp")).string();
    }
    ~TempPath() { std::filesystem::remove(path_); }
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

SLUICE_MAIN()
