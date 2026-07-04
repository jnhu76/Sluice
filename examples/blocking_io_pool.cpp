// blocking_io_pool: BlockingIoPool execution-helper demo (sluice-CORE-024S §7).
//
// Demonstrates G9: BlockingIoPool is a thread-pool execution helper for blocking
// work, NOT an async runtime. It runs ordinary blocking operations on fixed
// std::thread workers, isolating that work from the caller's thread. No IoContext,
// no completion model, no async.
//
// Build/run: xmake build blocking_io_pool && xmake run blocking_io_pool
#include "support/blocking_io_pool.hpp"

#include <atomic>
#include <cstdio>
#include <cstddef>

int main() {
    sluice::bench::BlockingIoPool pool(4);
    std::printf("pool workers: %zu\n", pool.thread_count());

    std::atomic<int> done{0};
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i) {
        pool.submit([&done] {
            // A "blocking" unit of work (here: trivial). On a real workload this
            // would be a blocking read/write/sync against a file.
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }
    pool.wait_all();   // blocks until every submitted job completes
    std::printf("jobs completed: %d / %d\n", done.load(), N);
    pool.shutdown();   // idempotent; also runs in the destructor if skipped
    return done.load() == N ? 0 : 1;
}
