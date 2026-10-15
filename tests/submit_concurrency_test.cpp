// Concurrent submissions through one context from several threads. The
// public concurrency matrix allows submit from different threads with
// serialized acceptance: a second submitter must not establish its own
// reservation inside another submitter's reserve-to-accept window, live
// requests must never alias identities, and concurrent capacity accounting
// must be exact. Interleavings are forced with the backend submit pause
// gates and start barriers; no synchronization relies on sleeps.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(SLUICE_HAS_LIBURING)
#include <sluice/async/uring_backend.hpp>
#endif

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::FileOpen;
using sluice::IoError;
using sluice::Result;

#if defined(SLUICE_HAS_LIBURING)
using Backend = UringAsyncBackend;
#else
using Backend = ThreadPoolBackend;
#endif

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

std::unique_ptr<Backend> make_backend(std::size_t capacity) {
#if defined(SLUICE_HAS_LIBURING)
    return std::make_unique<Backend>(UringConfig{capacity, static_cast<unsigned>(capacity)});
#else
    return std::make_unique<Backend>(ThreadPoolConfig{capacity, 2});
#endif
}

bool uring_profile_unavailable() {
#if defined(SLUICE_HAS_LIBURING)
    try {
        return !Backend().available();
    } catch (...) {
        return true;
    }
#else
    return false;
#endif
}

template <class T> void drive_ready(AsyncIoContext& ctx, Request<T>& request) {
    while (!request.ready())
        (void)ctx.poll();
}

bool wait_counter_reaches(const std::atomic<int>& v, int expected) {
    for (int i = 0; i < 20000000 && v.load(std::memory_order_acquire) < expected; ++i)
        std::this_thread::yield();
    return v.load(std::memory_order_acquire) >= expected;
}

void wait_gate_paused(Backend::PreAcceptCommitPauseGate& gate) {
    std::atomic<bool>& paused = gate.paused;
    bool seen = paused.load(std::memory_order_acquire);
    while (!seen) {
        paused.wait(seen, std::memory_order_acquire);
        seen = paused.load(std::memory_order_acquire);
    }
}

void resume_gate(Backend::PreAcceptCommitPauseGate& gate) noexcept {
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
}

std::string temp_path(const char* tag) {
    std::string p = std::string("/tmp/sluice_submit_conc_") + tag + "_XXXXXX";
    std::vector<char> buf(p.begin(), p.end());
    buf.push_back('\0');
    const int fd = ::mkstemp(buf.data());
    if (fd < 0)
        return {};
    ::close(fd);
    ::unlink(buf.data());
    return std::string(buf.data());
}

constexpr std::size_t kRegion = 64;
constexpr std::size_t kRegions = 128;

std::vector<std::byte> region_body(std::size_t region) {
    std::vector<std::byte> out(kRegion);
    for (std::size_t i = 0; i < kRegion; ++i)
        out[i] = static_cast<std::byte>((region + i * 13 + 5) & 0xff);
    return out;
}

bool fill_regions(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return false;
    for (std::size_t r = 0; r < kRegions; ++r) {
        const std::vector<std::byte> body = region_body(r);
        if (std::fwrite(body.data(), 1, body.size(), f) != body.size()) {
            std::fclose(f);
            return false;
        }
    }
    std::fclose(f);
    return true;
}

// The serialized-acceptance window: one submitter is parked between its
// reservation and its commit; a second submitter must not be able to
// establish its own reservation inside that window. The core snapshot only
// observes the interleaving; the public consequences are asserted at the end
// of this case and throughout the next two.
bool acceptance_window_excludes_a_second_submitter() {
    const std::string path = temp_path("window");
    if (path.empty() || !fill_regions(path)) {
        check(false, "fixture created");
        return false;
    }
    auto opened = File::open(path, FileOpen{});
    check(opened.has_value(), "fixture open");
    if (!opened.has_value())
        return false;
    File file = std::move(opened.value());

    std::unique_ptr<Backend> backend = make_backend(8);
    Backend::PreAcceptCommitPauseGate gate;
    backend->set_pre_accept_commit_pause_gate(&gate);
    AsyncIoContext ctx(std::move(backend));

    std::vector<std::byte> low_buffer(kRegion, std::byte{0});
    std::vector<std::byte> high_buffer(kRegion, std::byte{0});
    std::optional<Result<Request<std::size_t>>> first_outcome;
    std::thread first([&] {
        first_outcome =
            ctx.submit_read(ReadOp{NativeFileRef{file}, low_buffer.data(), kRegion, 0});
    });
    wait_gate_paused(gate);

    std::atomic<bool> second_done{false};
    std::optional<Result<Request<std::size_t>>> second_outcome;
    std::thread second([&] {
        second_outcome = ctx.submit_read(
            ReadOp{NativeFileRef{file}, high_buffer.data(), kRegion, kRegion});
        second_done.store(true, std::memory_order_release);
    });

    bool second_inside_window = false;
    for (int i = 0; i < 1000000; ++i) {
        if (second_done.load(std::memory_order_acquire) ||
            ctx.context_core_for_test()->snapshot().reserved >= 2) {
            second_inside_window = true;
            break;
        }
        std::this_thread::yield();
    }
    check(!second_inside_window,
          "no second reservation or completion lands inside another submitter's window");

    resume_gate(gate);
    first.join();
    second.join();
    check(first_outcome.has_value() && first_outcome->has_value(),
          "the parked submitter accepts after release");
    check(second_outcome.has_value() && second_outcome->has_value(),
          "the second submitter accepts after the window closes");
    if (!first_outcome->has_value() || !second_outcome->has_value())
        return false;
    check(first_outcome->value().id() != second_outcome->value().id(),
          "the two requests carry distinct identities");

    Request<std::size_t> first_request = std::move(first_outcome->value());
    Request<std::size_t> second_request = std::move(second_outcome->value());
    drive_ready(ctx, first_request);
    drive_ready(ctx, second_request);
    auto first_observed = first_request.take_result();
    auto second_observed = second_request.take_result();
    const std::vector<std::byte> low = region_body(0);
    const std::vector<std::byte> high = region_body(1);
    check(first_observed.result.has_value() &&
              std::memcmp(low_buffer.data(), low.data(), kRegion) == 0,
          "the first region settles with its own payload");
    check(second_observed.result.has_value() &&
              std::memcmp(high_buffer.data(), high.data(), kRegion) == 0,
          "the second region settles with its own payload");
    (void)file.close();
    ::unlink(path.c_str());
    return true;
}

// The concurrent corner itself: several threads submit through one context
// from a start barrier. Every request must be accepted (capacity never
// saturates), identities must stay distinct while all are simultaneously
// live, and each request must settle with exactly its own region.
bool concurrent_submitters_get_distinct_settling_requests() {
    const std::string path = temp_path("corner");
    if (path.empty() || !fill_regions(path)) {
        check(false, "fixture created");
        return false;
    }
    auto opened = File::open(path, FileOpen{});
    check(opened.has_value(), "fixture open");
    if (!opened.has_value())
        return false;
    File file = std::move(opened.value());

    constexpr std::size_t kThreads = 4;
    constexpr std::size_t kPerThread = 8;
    constexpr std::size_t kSubmits = kThreads * kPerThread;
    constexpr std::size_t kCapacity = 64;

    sluice::AsyncStats stats;
    AsyncIoContext ctx(make_backend(kCapacity), &stats);

    std::atomic<int> waiting{0};
    std::atomic<bool> go{false};
    struct Slot {
        std::vector<std::byte> buffer;
        std::optional<Result<Request<std::size_t>>> outcome;
    };
    std::vector<std::vector<Slot>> per_thread(kThreads);
    for (std::size_t t = 0; t < kThreads; ++t)
        per_thread[t].resize(kPerThread);
    std::vector<std::thread> workers;
    for (std::size_t t = 0; t < kThreads; ++t) {
        for (std::size_t i = 0; i < kPerThread; ++i)
            per_thread[t][i].buffer.assign(kRegion, std::byte{0});
        workers.emplace_back([&, t] {
            waiting.fetch_add(1, std::memory_order_acq_rel);
            bool started = go.load(std::memory_order_acquire);
            while (!started) {
                go.wait(started, std::memory_order_acquire);
                started = go.load(std::memory_order_acquire);
            }
            for (std::size_t i = 0; i < kPerThread; ++i) {
                const std::size_t region = t * kPerThread + i;
                per_thread[t][i].outcome = ctx.submit_read(ReadOp{
                    NativeFileRef{file}, per_thread[t][i].buffer.data(), kRegion,
                    region * kRegion});
            }
        });
    }
    check(wait_counter_reaches(waiting, static_cast<int>(kThreads)),
          "all submitters reached the barrier");
    go.store(true, std::memory_order_release);
    go.notify_all();
    for (std::thread& w : workers)
        w.join();

    std::size_t accepted = 0;
    for (std::size_t t = 0; t < kThreads; ++t) {
        for (std::size_t i = 0; i < kPerThread; ++i) {
            auto& slot = per_thread[t][i];
            check(slot.outcome.has_value() && slot.outcome->has_value(),
                  "every concurrent submission is accepted while capacity holds");
            if (slot.outcome.has_value() && slot.outcome->has_value())
                ++accepted;
        }
    }
    check(accepted == kSubmits, "no submission is lost or unexpectedly rejected");

    for (std::size_t a = 0; a < kThreads; ++a) {
        for (std::size_t i = 0; i < kPerThread; ++i) {
            if (!per_thread[a][i].outcome.has_value() || !per_thread[a][i].outcome->has_value())
                continue;
            const RequestId lhs = per_thread[a][i].outcome->value().id();
            for (std::size_t b = a; b < kThreads; ++b) {
                for (std::size_t j = (b == a ? i + 1 : 0); j < kPerThread; ++j) {
                    if (!per_thread[b][j].outcome.has_value() ||
                        !per_thread[b][j].outcome->has_value())
                        continue;
                    check(lhs != per_thread[b][j].outcome->value().id(),
                          "simultaneously live requests never alias identities");
                }
            }
        }
    }

    for (std::size_t t = 0; t < kThreads; ++t) {
        for (std::size_t i = 0; i < kPerThread; ++i) {
            auto& slot = per_thread[t][i];
            if (!slot.outcome.has_value() || !slot.outcome->has_value())
                continue;
            Request<std::size_t> request = std::move(slot.outcome->value());
            drive_ready(ctx, request);
            auto observed = request.take_result();
            const std::vector<std::byte> body = region_body(t * kPerThread + i);
            check(observed.result.has_value() && observed.result.value() == kRegion &&
                      std::memcmp(slot.buffer.data(), body.data(), kRegion) == 0,
                  "each concurrent request settles with exactly its own region");
        }
    }
    check(stats.submit_calls == kSubmits, "submit accounting counts every concurrent call");
    check(stats.submitted_ops == kSubmits,
          "acceptance accounting counts every accepted request");
    check(stats.invalid_state_rejections == 0, "no illegal rejection spellings appear");
    check(ctx.outstanding() == 0, "every concurrent request retires");
    (void)file.close();
    ::unlink(path.c_str());
    return true;
}

// Concurrent submissions against a saturated profile: while nothing drives
// publication, slots cannot reclaim, so exactly the capacity is accepted and
// every other concurrent submission reads would_block with no other
// spelling and no residue.
bool concurrent_capacity_boundary_is_exact() {
    const std::string path = temp_path("capacity");
    if (path.empty() || !fill_regions(path)) {
        check(false, "fixture created");
        return false;
    }
    auto opened = File::open(path, FileOpen{});
    check(opened.has_value(), "fixture open");
    if (!opened.has_value())
        return false;
    File file = std::move(opened.value());

    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kCapacity = 4;
    constexpr std::size_t kExpectedAccepts = kCapacity;
    constexpr std::size_t kExpectedRejections = kThreads - kCapacity;

    sluice::AsyncStats stats;
    AsyncIoContext ctx(make_backend(kCapacity), &stats);

    std::atomic<int> waiting{0};
    std::atomic<bool> go{false};
    std::vector<std::vector<std::byte>> buffers(kThreads,
                                                std::vector<std::byte>(kRegion, std::byte{0}));
    std::vector<std::optional<Result<Request<std::size_t>>>> outcomes(kThreads);
    std::vector<std::thread> workers;
    for (std::size_t t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            waiting.fetch_add(1, std::memory_order_acq_rel);
            bool started = go.load(std::memory_order_acquire);
            while (!started) {
                go.wait(started, std::memory_order_acquire);
                started = go.load(std::memory_order_acquire);
            }
            outcomes[t] = ctx.submit_read(
                ReadOp{NativeFileRef{file}, buffers[t].data(), kRegion, t * kRegion});
        });
    }
    check(wait_counter_reaches(waiting, static_cast<int>(kThreads)),
          "all submitters reached the barrier");
    go.store(true, std::memory_order_release);
    go.notify_all();
    for (std::thread& w : workers)
        w.join();

    std::size_t accepted = 0;
    std::size_t rejected = 0;
    for (std::size_t t = 0; t < kThreads; ++t) {
        const auto& outcome = outcomes[t];
        check(outcome.has_value(), "every concurrent submission returns");
        if (!outcome.has_value())
            continue;
        if (outcome->has_value()) {
            ++accepted;
        } else {
            ++rejected;
            check(outcome->error().code == IoError::Code::would_block,
                  "a saturated profile spells concurrent rejection would_block only");
        }
    }
    check(accepted == kExpectedAccepts && rejected == kExpectedRejections,
          "concurrent capacity accounting is exact against the profile capacity");
    check(stats.queue_full_retries == kExpectedRejections,
          "saturation accounting counts every concurrent rejection");
    check(stats.submitted_ops == kExpectedAccepts,
          "acceptance accounting counts exactly the admitted set");

    const auto settled = ctx.shutdown(ShutdownPolicy::drain);
    check(settled.has_value() && settled.value() == ShutdownOutcome::completed,
          "the settled accepted set drains to completion");
    std::size_t consumed = 0;
    for (std::size_t t = 0; t < kThreads; ++t) {
        if (outcomes[t].has_value() && outcomes[t]->has_value()) {
            Request<std::size_t> request = std::move(outcomes[t]->value());
            auto observed = request.take_result();
            check(observed.result.has_value() && observed.result.value() == kRegion,
                  "each accepted request remains consumable after drain");
            ++consumed;
        }
    }
    check(consumed == kExpectedAccepts, "every accepted request is consumable");
    (void)file.close();
    ::unlink(path.c_str());
    return true;
}

}

int main() {
    if (uring_profile_unavailable()) {
        std::printf("NOT RUN: io_uring unavailable on this host (kernel/policy blocked)\n");
        return 0;
    }
    acceptance_window_excludes_a_second_submitter();
    concurrent_submitters_get_distinct_settling_requests();
    concurrent_capacity_boundary_is_exact();
    if (g_failures != 0) {
        std::fprintf(stderr, "submit concurrency oracle: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("submit concurrency oracle passed\n");
    return 0;
}
