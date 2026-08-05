// ThreadPoolBackend persistent-worker regression (Phase E).
//
// HISTORY: the original Version-B regression (2026-08-01) targeted the legacy
// "one std::thread per op" model (DIV-03 / DIV-12): workers_ grew by one entry
// per submitted op and were joined only in the destructor, so a high-op-count
// copy (buf=1, N=100003) accumulated ~200k unjoined pthreads and could exhaust
// platform thread limits. The seam `unjoined_workers_for_test()` counted that
// growth.
//
// Phase E replaced that model with a FIXED persistent worker pool + a bounded
// dispatch ring + RequestArena as the single request-lifecycle authority. The
// resource violation is gone by construction: worker threads created after
// construction == 0, and worker storage never grows. This file restates the
// regression against the new invariant:
//
//   - the worker pool size is fixed for the backend's whole life and equals the
//     configured worker_count;
//   - no matter how many ops are submitted/drained, no new worker is created;
//   - ops still terminate correctly (no loss, no hang) — the semantic pairing
//     for the resource bound (AC-11).
//
// The seam: workers_spawned_for_test() (SLUICE_ASYNC_INTERNAL_TESTING only) is
// the pool size; it never changes after construction.
#include "harness.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

namespace {

class TempPath {
public:
    TempPath() {
        path_ = (std::filesystem::temp_directory_path() /
                 ("sluice_tp_reap_" + std::to_string(::getpid()) + "_" +
                  std::to_string(counter_++) + ".tmp"))
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

// Drain every currently-ready result through the real reaper (wait_one -> poll)
// and return how many results were drained. wait_one() blocks until at least one
// result is ready, so the loop ends exactly when nothing remains outstanding —
// no sleeps, no timeouts.
std::size_t drain_all_ready(ThreadPoolBackend& backend) {
    std::size_t reaped = 0;
    while (backend.outstanding() > 0) {
        auto wr = backend.wait_one();
        if (!wr.has_value()) break;
        reaped += wr.value();
    }
    return reaped;
}

}  // namespace

// Resource bound + semantic pairing: a tight submit/drain loop over many more
// ops than the configured capacity must NOT create any new worker, and every op
// must terminate with the real result. This is the Phase-E restatement of the
// original "unjoined workers bounded" regression.
SLUICE_TEST_CASE(tp_workers_persistent_and_bounded_under_load) {
    TempPath tp;
    int fd = ::open(tp.path().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    SLUICE_CHECK(fd >= 0);
    const std::byte seed[1] = {std::byte{0x5a}};
    SLUICE_CHECK(::write(fd, seed, 1) == 1);

    // Small capacity + small pool: the loop submits far more ops than capacity,
    // so slots are reused many times. worker_count is fixed at 2.
    constexpr std::size_t kCapacity = 8;
    constexpr std::size_t kWorkers = 2;
    constexpr std::size_t kOperations = 4096;
    constexpr std::size_t kBatch = kCapacity;
    ThreadPoolBackend backend(ThreadPoolConfig{kCapacity, kWorkers});
    SLUICE_CHECK_MSG(backend.workers_spawned_for_test() == kWorkers,
                     "pool size must equal configured worker_count at construction");

    std::vector<Completion<std::size_t>> cs(kBatch);
    std::vector<std::byte> bufs(kBatch);

    // Submit + drain in batches of kBatch. After EVERY batch the pool size must
    // still be kWorkers (no growth) and outstanding must be 0 (all drained).
    // Each batch reuses the same kBatch Completions (they are reset by poll's
    // reap->reset path of the previous batch's caller loop below).
    for (std::size_t base = 0; base < kOperations; base += kBatch) {
        for (std::size_t i = 0; i < kBatch; ++i) {
            cs[i].reset();  // return the slot, generation++; ready -> idle
            auto r = backend.submit_read(ReadOp{fd, bufs.data() + i, 1, 0}, cs[i]);
            SLUICE_CHECK(r.has_value());
        }
        std::size_t reaped = drain_all_ready(backend);
        SLUICE_CHECK_MSG(reaped == kBatch, "batch drain reaped the wrong number of ops");
        SLUICE_CHECK_MSG(backend.outstanding() == 0,
                         "ops remained outstanding after a batch drain");
        SLUICE_CHECK_MSG(backend.workers_spawned_for_test() == kWorkers,
                         "submit/drain created new workers (pool is not persistent)");
        // Each reaped op carries the real result (1 byte at offset 0).
        for (std::size_t i = 0; i < kBatch; ++i) {
            SLUICE_CHECK(cs[i].ready());
            SLUICE_CHECK(cs[i].result().has_value());
            SLUICE_CHECK(cs[i].result().value() == 1);
        }
    }
    SLUICE_CHECK_MSG(backend.workers_spawned_for_test() == kWorkers,
                     "final pool size must equal configured worker_count");
    ::close(fd);
}

// Same invariant for void ops (sync_data): the pool stays fixed, every op
// terminates.
SLUICE_TEST_CASE(tp_void_ops_keep_pool_bounded) {
    TempPath tp;
    int fd = ::open(tp.path().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    SLUICE_CHECK(fd >= 0);

    constexpr std::size_t kCapacity = 8;
    constexpr std::size_t kWorkers = 2;
    constexpr std::size_t kOperations = 256;
    constexpr std::size_t kBatch = kCapacity;
    ThreadPoolBackend backend(ThreadPoolConfig{kCapacity, kWorkers});
    SLUICE_CHECK(backend.workers_spawned_for_test() == kWorkers);

    std::vector<Completion<void>> cs(kBatch);
    for (std::size_t base = 0; base < kOperations; base += kBatch) {
        for (std::size_t i = 0; i < kBatch; ++i) {
            cs[i].reset();
            auto r = backend.submit_sync_data(SyncDataOp{fd}, cs[i]);
            SLUICE_CHECK(r.has_value());
        }
        std::size_t reaped = drain_all_ready(backend);
        SLUICE_CHECK_MSG(reaped == kBatch, "void batch drain reaped the wrong count");
        SLUICE_CHECK_MSG(backend.outstanding() == 0, "void ops remained outstanding");
        SLUICE_CHECK_MSG(backend.workers_spawned_for_test() == kWorkers,
                         "void submit/drain created new workers");
        for (std::size_t i = 0; i < kBatch; ++i) {
            SLUICE_CHECK(cs[i].ready());
            SLUICE_CHECK(cs[i].result().has_value());
        }
    }
    ::close(fd);
}

SLUICE_MAIN()
