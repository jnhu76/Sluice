// Production BlockingIoPool throughput/scalability bench (sluice-CORE-024S).
//
// Measures pool throughput (tasks/sec) as worker_count scales, for two task
// types: a CPU-light task (counter increment) and a blocking I/O-shaped task
// (short sleep). Gives the concurrency-performance skill MEASURED data instead
// of hypotheses (Per.1/Per.6: don't optimize without measurement).
//
// CSV to stdout: worker_count,task_type,task_count,total_ms,tasks_per_sec
//
// NO universal performance claim. Results are environment-sensitive (machine,
// core count, scheduler). The PURPOSE is to surface the shape: does throughput
// scale with workers, plateau, or regress? Where is the knee?
//
// Build/run: xmake build pool_throughput_bench && xmake run pool_throughput_bench
#include "bench_common.hpp"

#include <sluice/blocking_io_pool.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// Run N tasks through a pool with W workers; return elapsed ns. Each task does
// `work` (a callable). Tasks are fire-and-forget; the pool's drain-and-join at
// destruction guarantees completion.
std::uint64_t run_with_pool(std::size_t workers, std::size_t tasks,
                            const std::function<void()>& work) {
    auto pr = sluice::make_blocking_io_pool(sluice::BlockingIoPoolOptions{
        .worker_count = workers, .max_queue_depth = std::max<std::size_t>(workers * 4, 16)});
    if (!pr.has_value()) {
        return 0;
    }
    auto pool = std::move(pr.value());
    auto t0 = now_ns();
    for (std::size_t i = 0; i < tasks; ++i) {
        auto t = pool->submit(work);
        (void)t;
    }
    // Drain+join before stopping the clock so elapsed time includes completion.
    pool.reset();
    return now_ns() - t0;
}

void emit(const char* task_type, std::size_t workers, std::size_t tasks, std::uint64_t elapsed_ns) {
    double ms = static_cast<double>(elapsed_ns) / 1'000'000.0;
    double tps = (ms > 0) ? (static_cast<double>(tasks) / (ms / 1000.0)) : 0.0;
    std::printf("%zu,%s,%zu,%.3f,%.0f\n", workers, task_type, tasks, ms, tps);
}

} // namespace

int main() {
    std::printf("worker_count,task_type,task_count,total_ms,tasks_per_sec\n");

    const std::size_t hw = std::max<std::size_t>(2U, std::thread::hardware_concurrency());
    const std::size_t light_tasks = 20000;
    const std::size_t io_tasks = 4000;

    // CPU-light task: a relaxed atomic increment (minimal work per task).
    std::atomic<long> sink{0};
    auto light = [&sink] { sink.fetch_add(1, std::memory_order_relaxed); };

    // Blocking I/O-shaped task: a short yield/sleep simulating a blocking syscall.
    std::atomic<long> sink2{0};
    auto io_like = [&sink2] {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        sink2.fetch_add(1, std::memory_order_relaxed);
    };

    std::fprintf(stderr, "hw_concurrency=%zu\n", hw);

    // Sweep worker counts: 1, 2, 4, ..., up to 2*hw.
    std::vector<std::size_t> counts = {1, 2, 4};
    for (std::size_t w = 8; w <= hw * 2; w *= 2) {
        counts.push_back(w);
    }
    // Ensure hw itself is in the sweep (the natural saturation point).
    counts.push_back(hw);
    // Dedup + sort.
    std::sort(counts.begin(), counts.end());
    counts.erase(std::unique(counts.begin(), counts.end()), counts.end());

    for (std::size_t w : counts) {
        auto e = run_with_pool(w, light_tasks, light);
        emit("light", w, light_tasks, e);
    }
    for (std::size_t w : counts) {
        auto e = run_with_pool(w, io_tasks, io_like);
        emit("io_like", w, io_tasks, e);
    }

    std::fprintf(stderr, "note: pool throughput results are environment-sensitive "
                         "(machine/core count/scheduler). NO universal claim. The shape "
                         "(scale/plateau/regress + the knee near hw_concurrency) is the "
                         "robust signal.\n");
    (void)sink.load();
    (void)sink2.load();
    return 0;
}
