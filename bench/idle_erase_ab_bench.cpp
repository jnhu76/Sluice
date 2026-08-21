// idle_erase_ab_bench — Issue #161 repair hot-path cost check.
//
// The contribution-identity repair changed the three genuine idle-count
// erase sites from a plain release store(0) to exchange(0) + a conditional
// dance_epoch_ fetch_add (locked RMWs on x86). This bench measures the
// end-to-end Scheduler cost of that class of write on the route/pop/dance
// hot path so the repair's cost can be compared against its baseline
// (baseline tree: store(0) at the route-publication site; repaired tree:
// exchange+bump at all three sites).
//
// Workload: K trivial short fibers pre-spawned per round, then
// Scheduler::run(W) to completion — measured span is ONLY run(). Each
// round exercises the route-publication erase (the startup distribution
// routes every fiber through route_runnable_locked), the pop path (site-1
// erase), fiber start/finish, and the terminating idle dance (W
// contributions). Worker counts {1,2,4,8}; R rounds per count; the
// summary reports the MEDIAN round. FakeAsyncBackend isolates scheduler
// machinery from real-disk noise.
//
// A/B protocol (same session, same machine, Release builds): build this
// bench on the baseline tree and on the repaired tree, run both, compare
// ns/fiber at each worker count. The output is environment-sensitive; NO
// universal performance claim — this is a repair-cost check, not an
// optimization claim (§16.7).
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

using namespace sluice::async;
using std::chrono::steady_clock;

// Minimal test stack (mirrors the async internal tests' shape; the
// scheduler needs a caller-owned stack to init a Fiber).
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

constexpr std::size_t kFibers = 1024;  // per round (64 KiB stacks -> 64 MiB)
constexpr unsigned kRounds = 9;        // per worker count; median reported

double median_ns_per_fiber(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void bench(unsigned workers) {
    std::vector<double> ns_per_fiber;
    ns_per_fiber.reserve(kRounds);
    for (unsigned round = 0; round < kRounds; ++round) {
        AsyncIoContext ctx(std::make_unique<FakeAsyncBackend>());
        Scheduler sched(ctx);

        std::vector<Fiber> fibers(kFibers);
        std::vector<FiberStack> stacks(kFibers);
        for (std::size_t i = 0; i < kFibers; ++i) {
            // Trivial body: the pop/route/start/finish machinery dominates.
            fibers[i].set_entry([](Fiber&) {});
            if (!sched.init_fiber(fibers[i], stacks[i].base(),
                                  stacks[i].size())) {
                std::fprintf(stderr, "init_fiber failed\n");
                std::exit(1);
            }
            sched.spawn(fibers[i]);
        }

        const auto t0 = steady_clock::now();
        sched.run(workers);
        const auto t1 = steady_clock::now();

        for (std::size_t i = 0; i < kFibers; ++i) {
            if (fibers[i].state() != FiberState::done) {
                std::fprintf(stderr, "fiber %zu not done\n", i);
                std::exit(1);
            }
        }
        const double ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count();
        ns_per_fiber.push_back(ns / double(kFibers));
        std::printf("idle_erase_ab,workers=%u,round=%u,fibers=%zu,"
                    "ns_total=%.0f,ns_per_fiber=%.2f,fibers_per_sec=%.0f\n",
                    workers, round, kFibers, ns, ns / double(kFibers),
                    double(kFibers) * 1e9 / ns);
    }
    std::printf("idle_erase_ab MEDIAN,workers=%u,fibers=%zu,"
                "ns_per_fiber=%.2f,fibers_per_sec=%.0f\n",
                workers, kFibers, median_ns_per_fiber(ns_per_fiber),
                1e9 / median_ns_per_fiber(ns_per_fiber));
}

}  // namespace

int main() {
    std::printf("idle_erase_ab_bench: fibers/round=%zu rounds=%zu "
                "(median reported)\n",
                kFibers, std::size_t{kRounds});
    for (unsigned w : {1u, 2u, 4u, 8u}) {
        bench(w);
    }
    return 0;
}
