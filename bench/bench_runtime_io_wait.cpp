// M1-A Runtime I/O wait comparison benchmark (brief §13).
//
// Measures the framework overhead of the winning Candidate A capability
// (RuntimeTaskContext::await_completion) against the low-level direct
// baseline (AsyncIoContext + Scheduler::await_completion_*), which is NOT
// exposed to the app and exists only to bound the incremental Runtime-layer
// cost.
//
// Backend: FakeAsyncBackend (auto_bytes) to isolate framework overhead from
// real-disk noise (brief §13: FakeBackend is the primary framework-overhead
// comparison; real-disk throughput is supporting evidence only).
//
// Test matrix: op counts {warmup, 10000, 100000}; pipeline depth {1,4,16};
// workers {1,2}. Measures wall-clock, ns/op, ops/sec. Structural facts
// (allocations/suspends/resumes) are recorded in the race doc; this bench
// reports the measured timing.
//
// The output is environment-sensitive; NO universal performance claim. Run in
// Release for tuning-grade numbers (Debug caps absolute throughput).
#include <sluice/async/application_runtime.hpp>
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/group.hpp>
#include <sluice/async/scheduler.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

using namespace sluice::async;
using std::chrono::steady_clock;

// Minimal test stack (mirrors the one in scheduler_progress_test). The
// scheduler needs a caller-owned stack to init a Fiber.
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

struct Result {
    std::uint64_t ops;
    std::uint64_t ns_total;
    double ns_per_op() const { return ops ? double(ns_total) / double(ops) : 0.0; }
    double ops_per_sec() const {
        return ns_total ? double(ops) * 1e9 / double(ns_total) : 0.0;
    }
};

// Print one measured cell as a CSV-ish line.
void print(const char* case_name, const char* mode, std::uint64_t depth,
           unsigned workers, const Result& r) {
    std::printf("%s,%s,depth=%llu,workers=%u,ops=%llu,ns_total=%llu,"
                "ns_per_op=%.2f,ops_per_sec=%.0f\n",
                case_name, mode,
                static_cast<unsigned long long>(depth), workers,
                static_cast<unsigned long long>(r.ops),
                static_cast<unsigned long long>(r.ns_total),
                r.ns_per_op(), r.ops_per_sec());
}

// ---------------------------------------------------------------------------
// Candidate A path: RuntimeTaskContext::await_completion (the public API).
// Times the task body's submit+await loop (start/end timestamps captured
// inside the task), NOT the Runtime startup/shutdown.
// ---------------------------------------------------------------------------
Result bench_candidate_a(std::size_t ops, std::size_t depth, unsigned workers) {
    RuntimeBuilder builder;
    auto* raw = new FakeAsyncBackend();
    raw->auto_bytes(4);  // each op completes with 4 bytes on poll
    builder.backend(std::unique_ptr<AsyncBackend>(raw));
    builder.workers(workers);
    auto rt = std::move(builder.build().value());
    (void)rt->start();

    // The task records its own start/end so we measure the submit+await loop,
    // not Runtime lifecycle. Pipeline depth = number of ops submitted before
    // awaiting the oldest.
    std::atomic<std::size_t> completed{0};
    std::atomic<std::uint64_t> t0_ns{0}, t1_ns{0};
    auto sub_r = rt->submit([&](RuntimeTaskContext& ctx) {
        std::vector<Completion<std::size_t>> pool(depth);
        std::vector<std::byte> buf(depth * 4);
        std::size_t done = 0;
        std::size_t inflight = 0;
        std::size_t buf_idx = 0;
        t0_ns.store(static_cast<std::uint64_t>(
            steady_clock::now().time_since_epoch().count()), std::memory_order::relaxed);
        while (done < ops) {
            while (inflight < depth && done + inflight < ops) {
                auto& c = pool[inflight];
                std::byte* dst = &buf[buf_idx * 4];
                buf_idx = (buf_idx + 1) % depth;
                if (!ctx.submit_read(ReadOp{-1, dst, 4, 0}, c).has_value()) break;
                ++inflight;
            }
            // Await + consume all in-flight in submit order; reset for reuse.
            for (std::size_t i = 0; i < inflight; ++i) {
                ctx.await_completion(pool[i]);
                pool[i].reset();
            }
            done += inflight;
            inflight = 0;
        }
        t1_ns.store(static_cast<std::uint64_t>(
            steady_clock::now().time_since_epoch().count()), std::memory_order::relaxed);
        completed.store(done, std::memory_order::release);
    });
    (void)sub_r;

    rt->request_stop();
    (void)rt->drain();
    (void)rt->join();

    Result r;
    r.ops = completed.load(std::memory_order::acquire);
    r.ns_total = (r.ops > 0) ? (t1_ns.load(std::memory_order::acquire) -
                                t0_ns.load(std::memory_order::acquire)) : 0;
    return r;
}

// ---------------------------------------------------------------------------
// Low-level baseline (NOT exposed to the app): AsyncIoContext + Scheduler::
// await_completion_* directly. This is the most direct existing mechanism.
// It exists only to bound the incremental Runtime-layer cost. The fiber body
// records its own start/end timestamps.
// ---------------------------------------------------------------------------
Result bench_baseline(std::size_t ops, std::size_t depth, unsigned workers) {
    if constexpr (!fiber_ctx::supported) {
        return {0, 0};
    }
    auto* raw = new FakeAsyncBackend();
    raw->auto_bytes(4);
    AsyncIoContext ctx{std::unique_ptr<AsyncBackend>(raw)};
    Scheduler sched(ctx);

    std::atomic<std::size_t> completed{0};
    std::atomic<std::uint64_t> t0_ns{0}, t1_ns{0};
    Fiber f;
    std::vector<Completion<std::size_t>> pool(depth);
    std::vector<std::byte> buf(depth * 4);
    FiberStack stack;

    f.set_entry([&](Fiber&) {
        std::size_t done = 0;
        std::size_t buf_idx = 0;
        t0_ns.store(static_cast<std::uint64_t>(
            steady_clock::now().time_since_epoch().count()), std::memory_order::relaxed);
        while (done < ops) {
            std::size_t inflight = 0;
            while (inflight < depth && done + inflight < ops) {
                auto& c = pool[inflight];
                std::byte* dst = &buf[buf_idx * 4];
                buf_idx = (buf_idx + 1) % depth;
                if (!ctx.submit_read(ReadOp{-1, dst, 4, 0}, c).has_value()) break;
                ++inflight;
            }
            for (std::size_t i = 0; i < inflight; ++i) {
                sched.await_completion_size(pool[i]);
                pool[i].reset();
            }
            done += inflight;
        }
        t1_ns.store(static_cast<std::uint64_t>(
            steady_clock::now().time_since_epoch().count()), std::memory_order::relaxed);
        completed.store(done, std::memory_order::release);
    });

    sched.init_fiber(f, stack.base(), stack.size());
    sched.spawn(f);
    sched.run(workers);

    Result r;
    r.ops = completed.load(std::memory_order::acquire);
    r.ns_total = (r.ops > 0) ? (t1_ns.load(std::memory_order::acquire) -
                                t0_ns.load(std::memory_order::acquire)) : 0;
    return r;
}

}  // namespace

int main() {
    std::printf("# M1-A Runtime I/O wait benchmark (FakeAsyncBackend)\n");
    std::printf("# case,mode,depth,workers,ops,ns_total,ns_per_op,ops_per_sec\n");

    const std::size_t op_counts[] = {10000, 100000};
    const std::size_t depths[] = {1, 4, 16};
    const unsigned workers_list[] = {1, 2};

    // Warmup (discarded).
    (void)bench_candidate_a(2000, 1, 1);
    if (fiber_ctx::supported) (void)bench_baseline(2000, 1, 1);

    for (std::size_t ops : op_counts) {
        for (std::size_t depth : depths) {
            for (unsigned w : workers_list) {
                auto ra = bench_candidate_a(ops, depth, w);
                print("candidate_a", "runtime_await", depth, w, ra);
                if (fiber_ctx::supported) {
                    auto rb = bench_baseline(ops, depth, w);
                    print("baseline", "scheduler_await", depth, w, rb);
                }
            }
        }
    }
    std::printf("# done\n");
    return 0;
}
