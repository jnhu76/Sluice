// Issue #67 P1 follow-up regression: AsyncStats fields are plain std::uint64_t
// (caller-owned, never atomic — see include/sluice/measurement.hpp), and
// access_mtx_ is their single serialized accounting domain (AGENTS.md §4.1,
// §13.1 leaf domain). The split-wait fix moved the PARK out of access_mtx_ but
// a follow-on review found that stats accounting had also leaked out:
//   - wait_calls was bumped BEFORE acquiring access_mtx_ in wait_one();
//   - completed_ops was bumped AFTER releasing access_mtx_ on every reap.
// Two wait_one() callers (or a wait_one() and a poll()) therefore raced on the
// same field with no lock. The existing drain-starvation regression proved the
// park/reachability contract but never produced a non-zero completed_ops under
// real concurrency — the producing poll ran after both participants had
// serialized — so TSan stayed green. This test forces the actual race classes:
//   (A) multiple wait_one() callers concurrently, all sharing one AsyncStats;
//   (B) wait_one() racing against concurrent poll() on the same context.
// Under the pre-fix code this is a C++ data race on wait_calls/completed_ops
// (TSan flags it). Under the fix every accounting access is inside access_mtx_
// and the final counters are exact.
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace sluice::async;
using sluice::AsyncStats;
using sluice::IoError;
using sluice::Result;

namespace {

constexpr auto kWaitTimeout = std::chrono::seconds(10);

class TempPath {
public:
    explicit TempPath(const char* tag) {
        path_ = (std::filesystem::temp_directory_path() /
                 ("sluice_stats_race_" + std::string(tag) + "_" +
                  std::to_string(::getpid()) + "_" +
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

int open_temp(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "open_temp failed\n");
        std::exit(1);
    }
    return fd;
}

// Seed N bytes at offset 0 so each submitted read returns 1 real byte.
void seed_file(int fd, std::size_t n) {
    std::vector<std::byte> buf(n, std::byte{0xA7});
    while (!buf.empty()) {
        ssize_t w = ::pwrite(fd, buf.data(), buf.size(), 0);
        if (w <= 0) {
            std::fprintf(stderr, "seed_file pwrite failed\n");
            std::exit(1);
        }
        buf.erase(buf.begin(), buf.begin() + w);
    }
}

}  // namespace

SLUICE_MAIN()

// Bounded join of one thread: polls a join flag with short yields until the
// deadline, then blocks in join() once the flag is set. We cannot use a raw
// t.join() in the failure path (it would hang the test); instead the worker
// always reaches its own stop condition and sets done, and the main thread
// only blocks once that is observed. Used so a stuck park surfaces as a clean
// timeout failure instead of an unbounded hang.
bool join_bounded(std::thread& t, std::atomic<bool>& done,
                  std::chrono::steady_clock::time_point deadline) {
    if (!t.joinable()) return true;
    while (!done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    t.join();
    return true;
}

// Race class A: many wait_one() callers concurrently, sharing one AsyncStats.
// Each caller submits one read first (so wait_one has real work to reap), then
// waits. The pre-fix code bumped wait_calls outside access_mtx_, so concurrent
// wait_one() callers raced on wait_calls. The fix puts every accounting access
// inside access_mtx_; the final counters are exact.
SLUICE_TEST_CASE(wait_one_concurrent_callers_no_stats_race) {
    constexpr std::size_t kCallers = 4;
    constexpr std::size_t kOps = kCallers;

    TempPath tp("A");
    int fd = open_temp(tp.path());
    seed_file(fd, kOps);

    // workers == kOps so every submitted read can be reaped without needing a
    // parked waiter to first wake and poll — keeps the liveness proof
    // unambiguous while still exercising the wait_calls/completed_ops race.
    ThreadPoolConfig cfg;
    cfg.request_capacity = 8;
    cfg.worker_count = kOps;
    AsyncStats stats;
    AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>(cfg), &stats);

    std::vector<std::byte> buf(kOps, std::byte{});
    std::vector<Completion<std::size_t>> completions(kOps);

    // Each caller submits one read then calls wait_one() in a loop until ITS
    // own Completion is ready. Multiple wait_one() callers concurrently is the
    // load-bearing race (all bump wait_calls, two that reap also bump
    // completed_ops). A caller that observe a 0 (empty reap / control wake)
    // just re-enters — under contention some calls legitimately return 0 while
    // another participant did the actual reap.
    std::atomic<std::size_t> submitted{0};
    std::vector<std::atomic<bool>> done_flags(kCallers);
    for (auto& f : done_flags) f.store(false, std::memory_order_release);
    std::vector<std::thread> callers;
    callers.reserve(kCallers);
    for (std::size_t i = 0; i < kCallers; ++i) {
        callers.emplace_back([&, i] {
            ReadOp op{fd, buf.data() + i, 1, static_cast<std::uint64_t>(i)};
            if (ctx.submit_read(op, completions[i]).has_value()) {
                submitted.fetch_add(1, std::memory_order_release);
                // Loop on wait_one until our own Completion reaches ready. The
                // race this drives is on wait_calls / completed_ops, not on
                // the wait_one return value, so observe completions[i].ready()
                // as the per-caller termination condition.
                while (!completions[i].ready()) {
                    auto r = ctx.wait_one();
                    if (!r.has_value()) break;
                }
            }
            done_flags[i].store(true, std::memory_order_release);
        });
    }

    // Bounded join: a stuck park surfaces as a clean timeout failure, not an
    // unbounded hang. The race this test targets is on stats fields, not
    // liveness.
    const char* fail_msg = nullptr;
    const auto join_deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    for (std::size_t i = 0; i < kCallers; ++i) {
        if (!join_bounded(callers[i], done_flags[i], join_deadline)) {
            fail_msg = "wait_one caller did not finish in time";
            break;
        }
    }

    if (fail_msg == nullptr) {
        SLUICE_CHECK(submitted.load(std::memory_order_acquire) == kOps);

        // Drain any straggler via poll so completed_ops reflects every reap.
        // poll() is the other writer of completed_ops; doing this here also
        // exercises the wait_one-vs-poll accounting consistency under the same
        // lock.
        const auto drain_deadline =
            std::chrono::steady_clock::now() + kWaitTimeout;
        while (ctx.outstanding() != 0) {
            if (std::chrono::steady_clock::now() >= drain_deadline) {
                fail_msg = "did not drain to zero in time";
                break;
            }
            ctx.poll();
            std::this_thread::yield();
        }
    }

    // If any caller failed to join we cannot make exact counter assertions.
    // Detach any stragglers so the SLUICE_FAIL return path (which runs
    // destructors) does not call std::terminate on a joinable thread.
    if (fail_msg != nullptr) {
        for (auto& t : callers) {
            if (t.joinable()) t.detach();
        }
        SLUICE_FAIL(fail_msg);
    }

    for (std::size_t i = 0; i < kOps; ++i) {
        SLUICE_CHECK(completions[i].ready());
        SLUICE_CHECK(completions[i].result().has_value());
        SLUICE_CHECK(completions[i].result().value() == 1);
    }

    // Exact-value assertions: the accounting domain is access_mtx_, so the
    // final counters must be deterministic, not merely "no hang".
    //   - wait_calls == kCallers (each caller called wait_one exactly once)
    //   - submitted_ops == kOps
    //   - completed_ops == kOps (each submitted read reaped exactly once)
    // A racy counter would be lower (lost increment) or, for completed_ops,
    // potentially higher (double count). The pre-fix race made wait_calls
    // non-deterministic under concurrency; the fix makes these exact.
    SLUICE_CHECK(stats.wait_calls == kCallers);
    SLUICE_CHECK(stats.submitted_ops == kOps);
    SLUICE_CHECK(stats.completed_ops == kOps);

    // Release slots before the context goes out of scope (quiescent teardown
    // requires slot_in_use == 0).
    for (std::size_t i = 0; i < kOps; ++i) {
        completions[i].reset();
    }
    ::close(fd);
}

// Race class B: wait_one() racing concurrent poll() on the same context, both
// touching completed_ops. The pre-fix code released access_mtx_ BEFORE bumping
// completed_ops in the split-wait reap path, so a concurrent poll()'s
// completed_ops += n (under access_mtx_) raced with the just-released
// completed_ops += n in wait_one(). The fix does the bump inside the lock.
SLUICE_TEST_CASE(wait_one_and_poll_concurrent_no_stats_race) {
    constexpr std::size_t kOps = 4;

    TempPath tp("B");
    int fd = open_temp(tp.path());
    seed_file(fd, kOps);

    ThreadPoolConfig cfg;
    cfg.request_capacity = 8;
    cfg.worker_count = 2;
    AsyncStats stats;
    AsyncIoContext ctx(std::make_unique<ThreadPoolBackend>(cfg), &stats);

    std::vector<std::byte> buf(kOps, std::byte{});
    std::vector<Completion<std::size_t>> completions(kOps);

    for (std::size_t i = 0; i < kOps; ++i) {
        ReadOp op{fd, buf.data() + i, 1, static_cast<std::uint64_t>(i)};
        SLUICE_CHECK(ctx.submit_read(op, completions[i]).has_value());
    }

    // One participant drives wait_one (split-wait reap path: poll under
    // access_mtx_, bump completed_ops), while another drives poll() (also
    // under access_mtx_, also bumping completed_ops). The two race on the same
    // field unless both hold the same lock for the bump.
    std::atomic<bool> stop{false};
    std::atomic<bool> waiter_done{false};
    std::atomic<bool> poller_done{false};
    std::thread waiter([&] {
        while (!stop.load(std::memory_order_acquire)) {
            auto r = ctx.wait_one();
            if (!r.has_value()) break;
        }
        waiter_done.store(true, std::memory_order_release);
    });
    std::thread poller([&] {
        while (!stop.load(std::memory_order_acquire)) {
            ctx.poll();
            std::this_thread::yield();
        }
        poller_done.store(true, std::memory_order_release);
    });

    // Let the race run until everything is reaped, then stop both participants.
    const char* fail_msg = nullptr;
    const auto reap_deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (ctx.outstanding() != 0) {
        if (std::chrono::steady_clock::now() >= reap_deadline) {
            fail_msg = "did not reap all ops in time";
            break;
        }
        ctx.poll();
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);

    const auto join_deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    bool waiter_joined = join_bounded(waiter, waiter_done, join_deadline);
    bool poller_joined = join_bounded(poller, poller_done, join_deadline);
    if (fail_msg == nullptr && (!waiter_joined || !poller_joined)) {
        fail_msg = "participant did not finish after stop signal";
    }
    if (!waiter_joined && waiter.joinable()) waiter.detach();
    if (!poller_joined && poller.joinable()) poller.detach();
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);

    for (std::size_t i = 0; i < kOps; ++i) {
        SLUICE_CHECK(completions[i].ready());
        SLUICE_CHECK(completions[i].result().has_value());
        SLUICE_CHECK(completions[i].result().value() == 1);
        completions[i].reset();
    }

    // Exact-value assertion: completed_ops MUST equal the number of reaped
    // ops. Under the pre-fix split-wait code the concurrent poll() race could
    // lose increments (TSan flags the non-atomic write/write); the fix makes
    // this exact because every reap-and-bump is one critical section.
    SLUICE_CHECK(stats.submitted_ops == kOps);
    SLUICE_CHECK(stats.completed_ops == kOps);

    ::close(fd);
}
