// ThreadPoolBackend worker-reaping regression (Version B CI gate, 2026-08-01).
//
// Root cause: ThreadPoolBackend spawns one worker thread per op and joined
// workers ONLY in the destructor. A high-op-count copy (buf=1, N=100003 ->
// ~200k ops per depth) therefore accumulated ~200k unreaped (zombie) kernel
// threads. On kernels that count unreaped threads against task limits
// (RLIMIT_NPROC / threads-max decrement only at release_task/join — standard
// Linux kernels such as the GitHub runner's), thread creation eventually
// fails with EAGAIN, std::thread throws std::system_error, and the
// spawn-failure path resolves the op as backend_error: the copy fails
// mid-run (sluice_copy_pipeline_integration_test / pipeline_integration_
// buffer_sizes, 2 x "pipelined copy returned an error"). Locally the WSL2
// kernel does not count unreaped threads, so the workload passes — the bug
// only surfaced on the CI runner.
//
// Fix under test: poll()/wait_one() join each worker as its result is
// reaped, so the number of unreaped workers stays bounded by the number of
// outstanding ops instead of growing with the total op count.
//
// The seam: unjoined_workers_for_test() (SLUICE_ASYNC_INTERNAL_TESTING only)
// counts workers_ entries that have not been joined. The proof is checked
// MID-RUN: submits and full drains are interleaved in small batches, and
// after EVERY batch drain the unjoined count must already be zero. Pre-fix
// this equals the batch size after the first drain and grows linearly with
// the op count; post-fix it is zero after every batch. The end-of-run check
// alone cannot tell "joined as we go" from "joined once everything was
// drained", so the batched mid-stream checks are the regression.
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

// Drain every currently-ready result through the real reaper (wait_one ->
// poll) and return how many results were drained. wait_one() blocks until at
// least one result is ready, so the loop ends exactly when nothing remains
// outstanding — no sleeps, no timeouts. An error result (ThreadPoolBackend
// never produces one) breaks the loop; the caller's exact-count check then
// fails loudly instead of hanging.
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

SLUICE_TEST_CASE(tp_reap_unjoined_workers_bounded_after_full_reap) {
    TempPath tp;
    int fd = ::open(tp.path().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    SLUICE_CHECK(fd >= 0);
    const std::byte seed[1] = {std::byte{0x5a}};
    SLUICE_CHECK(::write(fd, seed, 1) == 1);

    constexpr std::size_t kOperations = 4096;
    constexpr std::size_t kBatch = 64;
    ThreadPoolBackend backend;
    std::vector<Completion<std::size_t>> cs(kOperations);
    // One byte per op: concurrent workers write their own slot (the caller
    // contract forbids sharing a buffer across outstanding ops).
    std::vector<std::byte> bufs(kOperations);

    // Submit and fully drain in interleaved batches of kBatch. After EVERY
    // batch drain the unjoined-worker count must already be back to zero: an
    // implementation that only joins at destruction — or only after the
    // final drain — leaves kBatch zombies after the first batch and grows
    // linearly with the op count (the kernel task-limit exhaustion that
    // broke CI). Small batches bound live threads to kBatch, so the proof is
    // deterministic without exhausting Linux thread limits or waiting on a
    // timeout.
    for (std::size_t i = 0; i < kOperations; ++i) {
        auto r = backend.submit_read(ReadOp{fd, bufs.data() + i, 1, 0}, cs[i]);
        SLUICE_CHECK(r.has_value());

        if ((i + 1) % kBatch == 0) {
            std::size_t reaped = drain_all_ready(backend);
            SLUICE_CHECK_MSG(reaped == kBatch,
                             "batch drain reaped the wrong number of ops");
            SLUICE_CHECK_MSG(backend.unjoined_workers_for_test() == 0,
                             "reaped ops left unjoined workers behind mid-run "
                             "(worker handles grow linearly with submits)");
        }
    }
    SLUICE_CHECK_MSG(backend.outstanding() == 0,
                     "ops remained outstanding after the final drain");

    // Every completion carries the real result (1 byte at offset 0).
    for (auto& c : cs) {
        SLUICE_CHECK(c.ready());
        SLUICE_CHECK(c.result().has_value());
        SLUICE_CHECK(c.result().value() == 1);
    }

    // THE REGRESSION, restated at the end: no worker may remain unreaped.
    // Pre-fix this is OPS (4096 unjoined zombies); post-fix poll() joined
    // every reaped worker as it drained.
    SLUICE_CHECK_MSG(backend.unjoined_workers_for_test() == 0,
                     "reaped ops left unjoined worker threads behind (zombie "
                     "accumulation under kernel task limits)");
    ::close(fd);
}

SLUICE_TEST_CASE(tp_reap_void_ops_bounded_too) {
    TempPath tp;
    int fd = ::open(tp.path().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    SLUICE_CHECK(fd >= 0);

    constexpr std::size_t kOperations = 256;
    constexpr std::size_t kBatch = 64;
    ThreadPoolBackend backend;
    std::vector<Completion<void>> cs(kOperations);
    for (std::size_t i = 0; i < kOperations; ++i) {
        auto r = backend.submit_sync_data(SyncDataOp{fd}, cs[i]);
        SLUICE_CHECK(r.has_value());

        if ((i + 1) % kBatch == 0) {
            std::size_t reaped = drain_all_ready(backend);
            SLUICE_CHECK_MSG(reaped == kBatch,
                             "batch drain reaped the wrong number of ops");
            SLUICE_CHECK_MSG(backend.unjoined_workers_for_test() == 0,
                             "reaped void ops left unjoined workers behind "
                             "mid-run");
        }
    }
    SLUICE_CHECK_MSG(backend.outstanding() == 0,
                     "void ops remained outstanding after the final drain");
    for (auto& c : cs) {
        SLUICE_CHECK(c.ready());
        SLUICE_CHECK(c.result().has_value());
    }
    SLUICE_CHECK_MSG(backend.unjoined_workers_for_test() == 0,
                     "reaped void ops left unjoined worker threads behind");
    ::close(fd);
}

SLUICE_MAIN()
