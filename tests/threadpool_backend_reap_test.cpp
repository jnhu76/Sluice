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
// counts workers_ entries that have not been joined. Pre-fix this equals the
// total op count after a full submit/reap cycle; post-fix it must be zero.
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

}  // namespace

SLUICE_TEST_CASE(tp_reap_unjoined_workers_bounded_after_full_reap) {
    TempPath tp;
    int fd = ::open(tp.path().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    SLUICE_CHECK(fd >= 0);
    const std::byte seed[1] = {std::byte{0x5a}};
    SLUICE_CHECK(::write(fd, seed, 1) == 1);

    constexpr std::size_t OPS = 4096;
    ThreadPoolBackend backend;
    std::vector<Completion<std::size_t>> cs(OPS);
    // One byte per op: concurrent workers write their own slot (the caller
    // contract forbids sharing a buffer across outstanding ops).
    std::vector<std::byte> bufs(OPS);

    for (std::size_t i = 0; i < OPS; ++i) {
        auto r = backend.submit_read(ReadOp{fd, bufs.data() + i, 1, 0}, cs[i]);
        SLUICE_CHECK(r.has_value());
    }

    // Reap everything through the real reaper (wait_one -> poll). Each reaped
    // result must join its worker post-fix; pre-fix every worker stays
    // unjoined until the destructor.
    std::size_t reaped = 0;
    while (backend.outstanding() > 0) {
        auto wr = backend.wait_one();
        SLUICE_CHECK(wr.has_value());
        reaped += wr.value();
    }
    SLUICE_CHECK(reaped == OPS);

    // Every completion carries the real result (1 byte at offset 0).
    for (auto& c : cs) {
        SLUICE_CHECK(c.ready());
        SLUICE_CHECK(c.result().has_value());
        SLUICE_CHECK(c.result().value() == 1);
    }

    // THE REGRESSION: no worker may remain unreaped after its result was
    // reaped. Pre-fix this is OPS (4096 unjoined zombies); post-fix poll()
    // joined every reaped worker.
    SLUICE_CHECK_MSG(backend.unjoined_workers_for_test() == 0,
                     "reaped ops left unjoined worker threads behind (zombie "
                     "accumulation under kernel task limits)");
    ::close(fd);
}

SLUICE_TEST_CASE(tp_reap_void_ops_bounded_too) {
    TempPath tp;
    int fd = ::open(tp.path().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    SLUICE_CHECK(fd >= 0);

    constexpr std::size_t OPS = 256;
    ThreadPoolBackend backend;
    std::vector<Completion<void>> cs(OPS);
    for (std::size_t i = 0; i < OPS; ++i) {
        auto r = backend.submit_sync_data(SyncDataOp{fd}, cs[i]);
        SLUICE_CHECK(r.has_value());
    }
    std::size_t reaped = 0;
    while (backend.outstanding() > 0) {
        auto wr = backend.wait_one();
        SLUICE_CHECK(wr.has_value());
        reaped += wr.value();
    }
    SLUICE_CHECK(reaped == OPS);
    for (auto& c : cs) {
        SLUICE_CHECK(c.ready());
        SLUICE_CHECK(c.result().has_value());
    }
    SLUICE_CHECK_MSG(backend.unjoined_workers_for_test() == 0,
                     "reaped void ops left unjoined worker threads behind");
    ::close(fd);
}

SLUICE_MAIN()
