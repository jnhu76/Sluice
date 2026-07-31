// sluice-copy Version B pipeline integration tests.
//
// Real temporary files + ThreadPoolBackend. Drives the SAME copy_task code the
// CLI uses (apps/sluice-copy/copy_task.cpp) via run_pipelined_copy[_with_backend],
// asserting end-to-end correctness that the scripted-backend contract tests
// cannot (the scripted backend performs no real I/O, so it cannot prove byte-
// for-byte content). Covers:
//   - exact source/destination byte equality across many sizes/depths/buffers;
//   - edge sizes 0, 1, buffer-1, buffer, buffer+1, depth*buffer, depth*buffer+1;
//   - depths 1, 2, 3, 4, 8 (depth=1 == Version A);
//   - multi-round slot reuse (file much larger than depth*buffer);
//   - binary data with embedded zeros;
//   - sync none/data/all after a pipelined copy;
//   - a deterministic proof that depth>1 yields >= 2 concurrent reads against
//     ThreadPoolBackend (the real backend), via a best-effort observer.
//
// No sleeps used as proof of ordering; the content-equality checks are the
// proof. copy_task.cpp is compiled into this target alongside the test.
#include "harness.hpp"

#include "copy_task.hpp"  // apps/sluice-copy public header

#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>
#include <vector>

using namespace sluice_copy;
using sluice::IoError;
using sluice::Result;

namespace {

// RAII temp file (anonymous, auto-unlinked).
struct TempFile {
    int fd;
    TempFile() {
        char p[] = "/tmp/sluice_copy_pit_XXXXXX";
        fd = ::mkstemp(p);
        SLUICE_CHECK(fd >= 0);
        ::unlink(p);
    }
    ~TempFile() { if (fd >= 0) ::close(fd); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

// Deterministic byte for a given offset (no /dev/urandom so failures are
// reproducible). Mixes in zeros every 7th byte to exercise embedded zeros.
std::byte byte_at(std::size_t i) {
    unsigned char b = static_cast<unsigned char>((i * 31 + 7) & 0xFF);
    return (i % 7 == 0) ? std::byte{0} : std::byte{b};
}

void seed_file(int fd, std::size_t n) {
    std::vector<std::byte> data(n);
    for (std::size_t i = 0; i < n; ++i) data[i] = byte_at(i);
    if (n > 0) {
        ssize_t w = ::pwrite(fd, data.data(), n, 0);
        SLUICE_CHECK(static_cast<std::size_t>(w) == n);
    }
}

bool files_equal(int a, int b, std::size_t n) {
    if (n == 0) return true;
    std::vector<std::byte> da(n), db(n);
    ssize_t ra = ::pread(a, da.data(), n, 0);
    ssize_t rb = ::pread(b, db.data(), n, 0);
    if (ra != static_cast<ssize_t>(n) || rb != static_cast<ssize_t>(n))
        return false;
    return std::memcmp(da.data(), db.data(), n) == 0;
}

// Copy src->dst at the given buffer/depth/workers/sync and verify byte-equality
// plus the reported byte count.
void check_copy(int src, int dst, std::size_t total, std::size_t buf,
                std::size_t depth, unsigned workers, SyncPolicy sync) {
    auto r = run_pipelined_copy(src, dst, buf, depth, workers, sync);
    SLUICE_CHECK_MSG(r.has_value(), "pipelined copy returned an error");
    SLUICE_CHECK(r.value().bytes_copied == total);
    SLUICE_CHECK(files_equal(src, dst, total));
}

}  // namespace

// ---------------------------------------------------------------------------
// Edge sizes at a fixed depth, across depths: 0,1,B-1,B,B+1,depth*B,depth*B+1
// plus a multi-round size (forces slot reuse).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(pipeline_integration_edge_sizes_per_depth) {
    constexpr std::size_t B = 4096;
    for (std::size_t depth : {1u, 2u, 3u, 4u, 8u}) {
        std::size_t sizes[] = {
            0, 1, B - 1, B, B + 1, depth * B, depth * B + 1, depth * B * 3 + 7,
        };
        for (std::size_t n : sizes) {
            TempFile src, dst;
            seed_file(src.fd, n);
            check_copy(src.fd, dst.fd, n, B, depth, 1, SyncPolicy::none);
        }
    }
}

// ---------------------------------------------------------------------------
// Multiple buffer sizes at depth>1, including small buffers that force many
// rounds of slot reuse.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(pipeline_integration_buffer_sizes) {
    constexpr std::size_t N = 100003;  // prime-ish, not a buffer multiple
    for (std::size_t buf : {1u, 7u, 512u, 4096u, 65536u}) {
        for (std::size_t depth : {2u, 4u}) {
            TempFile src, dst;
            seed_file(src.fd, N);
            check_copy(src.fd, dst.fd, N, buf, depth, 1, SyncPolicy::none);
        }
    }
}

// ---------------------------------------------------------------------------
// Sync policies after a pipelined copy (data reaches the destination file).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(pipeline_integration_sync_policies) {
    constexpr std::size_t N = 4096 * 3;
    for (SyncPolicy sp : {SyncPolicy::none, SyncPolicy::data, SyncPolicy::all}) {
        for (std::size_t depth : {1u, 2u, 3u}) {
            TempFile src, dst;
            seed_file(src.fd, N);
            check_copy(src.fd, dst.fd, N, 4096, depth, 1, sp);
        }
    }
}

// ---------------------------------------------------------------------------
// Multi-worker Runtime + depth>1: correctness still holds with >1 worker.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(pipeline_integration_multi_worker) {
    constexpr std::size_t N = 4096 * 6;
    for (unsigned workers : {1u, 2u, 4u}) {
        for (std::size_t depth : {2u, 4u}) {
            TempFile src, dst;
            seed_file(src.fd, N);
            check_copy(src.fd, dst.fd, N, 4096, depth, workers, SyncPolicy::none);
        }
    }
}

// ---------------------------------------------------------------------------
// Deterministic proof: depth>1 yields >= 2 concurrent reads against the REAL
// ThreadPoolBackend. We use a backend-injected observer that counts distinct
// simultaneously-outstanding read ops by snapshotting under the backend mutex.
//
// ThreadPoolBackend does not expose internal stats, so we wrap it: a thin
// AsyncBackend decorator that delegates every call to a real ThreadPoolBackend
// while counting the peak number of simultaneously-outstanding reads it has
// been asked to submit (submit_read before the matching completion is reaped).
// ---------------------------------------------------------------------------
namespace {

class ReadConcurrencyProbe : public sluice::async::AsyncBackend {
public:
    explicit ReadConcurrencyProbe(std::unique_ptr<AsyncBackend> inner)
        : inner_(std::move(inner)) {}

    std::size_t peak_outstanding_reads() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return peak_reads_;
    }

    Result<void> submit_read(sluice::async::ReadOp op,
                             sluice::async::Completion<std::size_t>& c) override {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ++live_reads_;
            if (live_reads_ > peak_reads_) peak_reads_ = live_reads_;
        }
        // Wrap the completion so we can decrement live_reads_ when it becomes
        // ready. We intercept by polling is harder; instead, track via a
        // side-channel: record the completion pointer and let poll() reap.
        live_read_completions_.push_back(&c);
        return inner_->submit_read(op, c);
    }
    Result<void> submit_write(sluice::async::WriteOp op,
                              sluice::async::Completion<std::size_t>& c) override {
        return inner_->submit_write(op, c);
    }
    Result<void> submit_sync_data(sluice::async::SyncDataOp op,
                                  sluice::async::Completion<void>& c) override {
        return inner_->submit_sync_data(op, c);
    }
    Result<void> submit_sync_all(sluice::async::SyncAllOp op,
                                 sluice::async::Completion<void>& c) override {
        return inner_->submit_sync_all(op, c);
    }
    std::size_t poll() override {
        std::size_t n = inner_->poll();
        reap_ready_reads();
        return n;
    }
    Result<std::size_t> wait_one() override {
        auto r = inner_->wait_one();
        reap_ready_reads();
        return r;
    }
    void cancel(sluice::async::Completion<std::size_t>& c) override {
        inner_->cancel(c);
    }
    void cancel(sluice::async::Completion<void>& c) override {
        inner_->cancel(c);
    }
    std::size_t outstanding() const noexcept override {
        return inner_->outstanding();
    }

private:
    // Decrement live_reads_ for any tracked read completion that is now ready.
    void reap_ready_reads() {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = live_read_completions_.begin();
             it != live_read_completions_.end();) {
            if ((*it)->ready()) {
                --live_reads_;
                it = live_read_completions_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::unique_ptr<AsyncBackend> inner_;
    mutable std::mutex mtx_;
    std::size_t live_reads_ = 0;
    std::size_t peak_reads_ = 0;
    std::vector<sluice::async::Completion<std::size_t>*> live_read_completions_;
};

}  // namespace

SLUICE_TEST_CASE(pipeline_integration_real_multi_outstanding_reads) {
    constexpr std::size_t B = 4096;
    constexpr std::size_t N = B * 16;  // large enough to fill a depth-4 window
    TempFile src, dst;
    seed_file(src.fd, N);

    auto inner = std::make_unique<sluice::async::ThreadPoolBackend>();
    auto* probe = new ReadConcurrencyProbe(std::move(inner));
    std::unique_ptr<sluice::async::AsyncBackend> backend(probe);

    auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, B, /*depth=*/4, 1,
                                             SyncPolicy::none, std::move(backend));
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value().bytes_copied == N);
    SLUICE_CHECK(files_equal(src.fd, dst.fd, N));
    // The real backend saw >= 2 reads outstanding simultaneously. (peak may not
    // reach the full depth on small/fast files, but must exceed the depth=1
    // baseline of 1 to prove read-ahead is real.)
    SLUICE_CHECK_MSG(probe->peak_outstanding_reads() >= 2,
                     "ThreadPoolBackend never saw >= 2 concurrent reads; "
                     "Version B read-ahead is not producing real concurrency");
}

// ---------------------------------------------------------------------------
// depth=1 against the real backend: peak concurrent reads == 1 (Version A
// behavior preserved).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(pipeline_integration_depth1_single_read) {
    constexpr std::size_t B = 4096;
    constexpr std::size_t N = B * 8;
    TempFile src, dst;
    seed_file(src.fd, N);

    auto inner = std::make_unique<sluice::async::ThreadPoolBackend>();
    auto* probe = new ReadConcurrencyProbe(std::move(inner));
    std::unique_ptr<sluice::async::AsyncBackend> backend(probe);

    auto r = run_pipelined_copy_with_backend(src.fd, dst.fd, B, /*depth=*/1, 1,
                                             SyncPolicy::none, std::move(backend));
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value().bytes_copied == N);
    SLUICE_CHECK(files_equal(src.fd, dst.fd, N));
    SLUICE_CHECK_MSG(probe->peak_outstanding_reads() == 1,
                     "depth=1 must keep at most 1 read outstanding");
}

SLUICE_MAIN()
