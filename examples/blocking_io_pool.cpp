// blocking_io_pool: production bounded OS-thread pool demo (sluice-CORE-024S §7).
//
// Demonstrates the PRODUCTION sluice::BlockingIoPool (NOT the bench adapter):
//   - bounded fixed worker count + bounded queue (backpressure / rejection)
//   - submit() returns a Task<T>; get() surfaces the return value or rethrows
//   - try_submit() is non-blocking (rejects with would_block when full)
//   - submit-after-shutdown is REJECTED (invalid_state) — no silent drop
//   - shutdown() drains already-submitted work and joins (idempotent)
//
// BlockingIoPool is a sync execution helper for blocking work, NOT an async
// runtime: no IoContext, no completion model, no coroutine, no io_uring.
//
// Build/run: xmake build blocking_io_pool && xmake run blocking_io_pool
#include <sluice/blocking_io_pool.hpp>
#include <sluice/result.hpp>

#include <exception>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

int main() {
    try {
        // 1. Construct via the factory (rejects 0/0).
        auto pr = sluice::make_blocking_io_pool(
            sluice::BlockingIoPoolOptions{.worker_count = 4, .max_queue_depth = 64});
        if (!pr.has_value()) {
            std::fprintf(stderr, "pool construction failed\n");
            return 2;
        }
        auto pool = std::move(pr.value());
        std::printf("pool workers: %zu\n", pool->worker_count());

        // 2. Submit tasks that return a value; the value surfaces via Task::get().
        std::vector<sluice::Task<int>> tasks;
        for (int i = 0; i < 10; ++i) {
            auto t = pool->submit([i] {
                // A blocking unit of work (here: trivial). On a real workload
                // this would be a blocking read/write/sync against a file.
                return i * i;
            });
            if (!t.has_value()) {
                std::fprintf(stderr, "submit rejected: %d\n", static_cast<int>(t.error().code));
                return 1;
            }
            tasks.emplace_back(std::move(t.value()));
        }

        // 3. Reap return values.
        int sum = 0;
        for (auto& t : tasks) {
            sum += t.get(); // blocks until the task completes
        }
        std::printf("sum of squares 0..9 = %d (expected 285)\n", sum);

        // 4. Task exceptions are surfaced via get() (rethrown).
        auto bad = pool->submit([]() -> int { throw std::runtime_error("boom"); });
        try {
            (void)bad.value().get();
            std::fprintf(stderr, "expected exception not thrown\n");
            return 1;
        } catch (const std::runtime_error& e) {
            std::printf("task exception surfaced: %s\n", e.what());
        }

        // 5. submit-after-shutdown is REJECTED (invalid_state), not a silent drop.
        pool->shutdown(); // idempotent; also runs in the destructor if skipped.
        auto rej = pool->submit([] { return 0; });
        if (rej.has_value()) {
            std::fprintf(stderr, "expected submit-after-shutdown rejection\n");
            return 1;
        }
        std::printf("submit-after-shutdown rejected (correct)\n");

        return (sum == 285) ? 0 : 1;
    } catch (const std::exception& e) {
        // No exception escapes main (bugprone-exception-escape). Task::get()
        // calls above can rethrow; surface them as a clean exit.
        std::fprintf(stderr, "unexpected exception: %s\n", e.what());
        return 1;
    }
}
