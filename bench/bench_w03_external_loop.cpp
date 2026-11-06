// W-03 external event loop: the driver thread parks in epoll_wait and the
// submitter parks on its own condition variable; neither thread busy-polls.

#include "bench_common.hpp"
#include "r3_ready_batch.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/r5_tail_trace.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/measurement.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <sys/epoll.h>
#include <unistd.h>

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileExistence;
using sluice::FileOpen;
using sluice::IoError;
using sluice::async::AsyncIoContext;
using sluice::AsyncStats;
using sluice::async::NativeFileRef;
using sluice::async::ReadOp;
using sluice::async::Request;
using sluice::async::RequestReadiness;
using sluice::async::ShutdownPolicy;
using sluice::async::ThreadPoolConfig;

constexpr std::size_t kInstrumentedSampleCap = 200000;
constexpr std::size_t kFixtureBytes = 64u << 20;

FileOpen rw_open_existing() {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    mode.existence = FileExistence::open_existing;
    return mode;
}

std::string describe(const sluice::IoError& e) {
    return std::string(sluice::to_string(e.code)) + "(errno=" + std::to_string(e.os_errno) + ")";
}

bool write_fixture(const std::string& path, std::string& failure) {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    mode.existence = FileExistence::create_if_missing;
    mode.contents = sluice::FileInitialContents::truncate;
    auto opened = File::open(path, mode);
    if (!opened.has_value()) {
        failure = "create failed: " + describe(opened.error());
        return false;
    }
    File file = std::move(opened).value();
    constexpr std::size_t kChunk = 1u << 20;
    std::vector<std::byte> buffer(kChunk);
    std::size_t written = 0;
    while (written < kFixtureBytes) {
        const std::size_t chunk = std::min(kChunk, kFixtureBytes - written);
        bench::fill_pattern(buffer.data(), chunk, written);
        auto res = sluice::blocking::write_all_at(
            file, written, std::span<const std::byte>(buffer.data(), chunk));
        if (!res.has_value() || !res.value().complete()) {
            failure = "fixture write failed";
            return false;
        }
        written += chunk;
    }
    if (!sluice::blocking::sync_data(file).has_value()) {
        failure = "fixture sync failed";
        return false;
    }
    return true;
}

struct BackendChoice {
    std::string name;
    bool available = true;
    std::string unavailable_reason;
};

std::optional<AsyncIoContext> make_context(const BackendChoice& choice, std::size_t depth,
                                            std::size_t workers, std::string& failure,
                                            AsyncStats* stats) {
    if (choice.name == "threadpool") {
        ThreadPoolConfig config;
        config.request_capacity = depth;
        config.worker_count = workers;
        try {
            return sluice::async::make_threadpool_context(config, stats);
        } catch (const std::exception& error) {
            failure = error.what();
            return std::nullopt;
        }
    }
    try {
        sluice::async::UringConfig config;
        config.request_capacity = depth;
        config.queue_depth = static_cast<unsigned>(depth);
        auto made = sluice::async::make_uring_context(config, stats);
        if (!made.has_value()) {
            failure = "io_uring backend not available in this build profile";
            return std::nullopt;
        }
        return std::move(made).value();
    } catch (const std::exception& error) {
        failure = error.what();
        return std::nullopt;
    }
}


struct LoopParams {
    const BackendChoice* backend = nullptr;
    std::size_t block_size = 4096;
    std::size_t request_count = 20000;
    std::size_t depth = 16;
    std::size_t workers = 4;
    // 0 means "same as depth".
    std::size_t max_refill = 0;
    std::string label_suffix;
};

// Shared between the driver thread (progress owner) and the submitter. The
// completion counter is what prevents a lost wake at this level: the submitter
// re-checks it under the mutex before parking, and the driver publishes it after
// every drive batch.
struct SharedLoop {
    std::mutex mutex;
    std::condition_variable cv;
    std::uint64_t completions = 0;
    std::uint64_t notifications = 0;
    std::uint64_t empty_wakes = 0;
    std::uint64_t passes = 0;
    std::uint64_t retry_scheduled = 0;
    // Facts of the owner's most recent drain loop (kept from its final
    // pass), published for the state-aware arms; zero new polls are issued
    // for them. owner_pass_sampled turns true once any pass has run and is
    // never cleared: a liveness witness, not freshness.
    std::uint64_t last_pass_completed = 0;
    bool last_pass_immediate_work = false;
    bool owner_pass_sampled = false;
    bool driver_attached = false;
    bool stop = false;
    bool driver_failed = false;
    std::string driver_failure;
    std::string shutdown_failure;
};

struct PhaseResult {
    bool ok = true;
    std::string failure;
    double wall_s = 0.0;
    std::size_t ops = 0;
    std::size_t bytes = 0;
    std::uint64_t notifications = 0;
    std::uint64_t empty_wakes = 0;
    std::uint64_t passes = 0;
    std::uint64_t retry_scheduled = 0;
    std::uint64_t submits = 0;
    bench::r3::Counters r3;
    std::vector<double> latencies;
    AsyncStats stats_delta;
};

void driver_thread(AsyncIoContext& ctx, int epoll_fd, int notification_fd, SharedLoop& shared) {
    auto owner = ctx.claim_progress_owner();
    if (!owner.has_value()) {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.driver_failed = true;
        shared.driver_failure = "claim_progress_owner failed";
        shared.cv.notify_all();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.driver_attached = true;
        shared.cv.notify_all();
    }

    bool retry_pending = false;
    bool stopping = false;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            stopping = shared.stop;
        }
        if (stopping) {
            break;
        }
        epoll_event events[4];
        const int timeout_ms = retry_pending ? 1 : -1;
        const int ready = ::epoll_wait(epoll_fd, events, 4, timeout_ms);
        if (ready > 0) {
            for (int i = 0; i < ready; ++i) {
                if (events[i].data.fd == notification_fd) {
                    sluice::async::detail::tailtrace::note_wake();
                    ctx.acknowledge_progress_notification();
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    ++shared.notifications;
                }
            }
        } else if (ready == 0) {
            std::lock_guard<std::mutex> lock(shared.mutex);
            ++shared.empty_wakes;
        }

        std::size_t batch_completions = 0;
        bool retry_remains = false;
        bool pass_failed = false;
        std::size_t last_pass_completed = 0;
        bool last_pass_immediate_work = false;
        bool saw_pass = false;
        // A pass that owes another transport attempt is handled by the next
        // epoll_wait timeout, so a retry never spins and never strands it.
        for (std::size_t guard = 0; guard < 1000000; ++guard) {
            auto pass = ctx.poll_progress();
            if (!pass.has_value()) {
                pass_failed = true;
                break;
            }
            batch_completions += pass.value().completed;
            retry_remains = pass.value().dispatch_retry_remains;
            last_pass_completed = pass.value().completed;
            last_pass_immediate_work = pass.value().immediate_work_remains;
            saw_pass = true;
            if (!pass.value().immediate_work_remains) {
                break;
            }
        }
        retry_pending = retry_remains;
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            ++shared.passes;
            shared.completions += batch_completions;
            if (retry_remains) {
                ++shared.retry_scheduled;
            }
            if (saw_pass) {
                shared.last_pass_completed = last_pass_completed;
                shared.last_pass_immediate_work = last_pass_immediate_work;
                shared.owner_pass_sampled = true;
            }
            if (pass_failed) {
                shared.driver_failed = true;
                shared.driver_failure = "poll_progress failed";
            }
        }
        shared.cv.notify_all();
    }

    // Retire the external registration first, then shut down while this thread
    // still holds the progress owner.
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, notification_fd, nullptr) != 0) {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.shutdown_failure = "EPOLL_CTL_DEL failed";
    }
    ctx.detach_progress_host();
    const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
    if (!outcome.has_value()) {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.shutdown_failure = "shutdown failed";
    }
}

PhaseResult run_external_phase(const LoopParams& params, const std::string& path,
                               bool instrument, bool verify, bool stats_enabled,
                               bench::r3::Arm arm, const char* diag_tag = nullptr) {
    PhaseResult phase;

    auto opened = File::open(path, rw_open_existing());
    if (!opened.has_value()) {
        phase.ok = false;
        phase.failure = "open failed: " + describe(opened.error());
        return phase;
    }
    File file = std::move(opened).value();

    AsyncStats stats;
    std::string backend_failure;
    auto context = make_context(*params.backend, params.depth, params.workers, backend_failure,
                                stats_enabled ? &stats : nullptr);
    if (!context.has_value()) {
        phase.ok = false;
        phase.failure = "NOT_RUN_WITH_REASON: " + backend_failure;
        return phase;
    }
    AsyncIoContext ctx = std::move(*context);

    const int epoll_fd = ::epoll_create1(0);
    if (epoll_fd < 0) {
        phase.ok = false;
        phase.failure = "epoll_create1 failed";
        return phase;
    }
    const int notification_fd = ctx.progress_notification_fd();
    if (notification_fd < 0) {
        phase.ok = false;
        phase.failure = "progress_notification_fd returned -1";
        ::close(epoll_fd);
        return phase;
    }
    epoll_event registration{};
    registration.events = EPOLLIN;
    registration.data.fd = notification_fd;
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, notification_fd, &registration) != 0) {
        phase.ok = false;
        phase.failure = "EPOLL_CTL_ADD failed";
        ::close(epoll_fd);
        return phase;
    }

    SharedLoop shared;
    std::thread driver(driver_thread, std::ref(ctx), epoll_fd, notification_fd,
                       std::ref(shared));
    {
        std::unique_lock<std::mutex> lock(shared.mutex);
        shared.cv.wait_for(lock, std::chrono::seconds(10),
                           [&] { return shared.driver_attached || shared.driver_failed; });
    }

    std::optional<bench::r3::Adapter> adapter;
    if (arm != bench::r3::Arm::a0)
        adapter.emplace(ctx, arm);

    // Diagnostic runs only: SLUICE_595_R4_DIAG_LINES=<path> appends one
    // structured line per budget decision. Never set during timed runs.
    static std::FILE* diag_stream = [] {
        const char* path = std::getenv("SLUICE_595_R4_DIAG_LINES");
        if (path == nullptr || *path == '\0') {
            return static_cast<std::FILE*>(nullptr);
        }
        return std::fopen(path, "a");
    }();
    bench::r3::Adapter::TraceSink trace_sink;
    if (diag_stream != nullptr && adapter.has_value()) {
        if (diag_tag != nullptr) {
            std::fprintf(diag_stream, "# phase %s arm=%s\n", diag_tag,
                         bench::r3::arm_name(arm));
        }
        adapter->set_trace_sink(&trace_sink);
    }

    const std::size_t blocks = (kFixtureBytes - params.block_size) / params.block_size;
    std::vector<std::vector<std::byte>> buffers(
        params.depth, std::vector<std::byte>(params.block_size));
    for (std::size_t i = 0; i < params.depth; ++i) {
        bench::fill_pattern(buffers[i].data(), params.block_size, 0);
    }

    struct Inflight {
        Request<std::size_t> request;
        std::size_t slot = 0;
        std::uint64_t offset = 0;
        std::uint32_t sequence = 0;
        // Unified latency origin: the instant the ReadOp for this item is fully
        // built, before any arm-specific submission work. Both arms record it at
        // the same semantic point so latencies include prep, scan and submit.
        double ready_ns = 0.0;
    };

    AsyncStats before = stats;
    std::deque<Inflight> window;
    std::size_t submitted = 0;
    std::size_t settled = 0;
    std::uint64_t watermark = 0;
    bool failed = false;
    std::string failure;

    const std::size_t max_refill =
        params.max_refill != 0 ? params.max_refill : params.depth;
    std::vector<ReadOp> ready;
    std::vector<double> ready_ns;
    std::vector<Request<std::size_t>> accepted;
    if (adapter.has_value()) {
        ready.reserve(params.depth);
        ready_ns.reserve(params.depth);
        accepted.reserve(params.depth);
    }
    const bench::TimePoint start = bench::now();
    if (instrument) {
        sluice::async::detail::tailtrace::begin_phase();
    }
    while (settled < params.request_count && !failed) {
        while (window.size() < params.depth && submitted < params.request_count) {
            const std::size_t want = std::min(
                {params.depth - window.size(), params.request_count - submitted, max_refill});
            const std::size_t base = submitted;
            if (adapter.has_value()) {
                ready.clear();
                ready_ns.clear();
                ready.reserve(want);
                for (std::size_t k = 0; k < want; ++k) {
                    const std::size_t sequence = base + k;
                    const std::size_t slot = sequence % params.depth;
                    ReadOp op;
                    op.file = NativeFileRef(file);
                    op.dst = buffers[slot].data();
                    op.len = params.block_size;
                    op.offset = static_cast<std::uint64_t>(sequence % blocks) * params.block_size;
                    ready.push_back(op);
                    const std::int64_t t0_ns =
                        instrument ? sluice::async::detail::tailtrace::now_ns_i() : 0;
                    ready_ns.push_back(instrument ? static_cast<double>(t0_ns) : 0.0);
                    if (instrument) {
                        sluice::async::detail::tailtrace::note_built(
                            static_cast<std::uint32_t>(sequence),
                            static_cast<std::uint32_t>(window.size()), t0_ns);
                    }
                }
                bench::r3::DecisionFacts facts;
                facts.caller_window = window.size();
                if (adapter->reads_owner_pass_facts()) {
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    facts.last_pass_completed = shared.last_pass_completed;
                    facts.last_pass_immediate_work = shared.last_pass_immediate_work;
                    facts.owner_pass_sampled = shared.owner_pass_sampled;
                }
                auto outcome = adapter->submit(std::span<const ReadOp>(ready.data(), ready.size()),
                                               accepted, facts);
                phase.r3.entry_calls += outcome.counters.entry_calls;
                phase.r3.group_calls += outcome.counters.group_calls;
                phase.r3.single_calls += outcome.counters.single_calls;
                phase.r3.accepted += outcome.counters.accepted;
                phase.r3.prefix_max =
                    std::max(phase.r3.prefix_max, outcome.counters.prefix_max);
                phase.r3.suffix_single_calls += outcome.counters.suffix_single_calls;
                phase.r3.flush_attempts += outcome.counters.flush_attempts;
                phase.r3.ineligible_fallbacks += outcome.counters.ineligible_fallbacks;
                phase.r3.gate_stops += outcome.counters.gate_stops;
                phase.r3.rejected_calls += outcome.counters.rejected_calls;
                phase.r3.untouched += outcome.counters.untouched;
                phase.r3.r4_batch_presentations += outcome.counters.r4_batch_presentations;
                phase.r3.r4_cap_reentries += outcome.counters.r4_cap_reentries;
                phase.r3.r4_chunk_reentries += outcome.counters.r4_chunk_reentries;
                phase.r3.r4_immediate_decisions += outcome.counters.r4_immediate_decisions;
                phase.r3.r4_authority_exits += outcome.counters.r4_authority_exits;
                phase.r3.r4_quantum1 += outcome.counters.r4_quantum1;
                phase.r3.r4_quantum2 += outcome.counters.r4_quantum2;
                phase.r3.r4_quantum8 += outcome.counters.r4_quantum8;
                phase.r3.r4_active_zero_decisions += outcome.counters.r4_active_zero_decisions;
                phase.r3.r4_pressure_decisions += outcome.counters.r4_pressure_decisions;
                phase.r3.r4_shadow_decisions += outcome.counters.r4_shadow_decisions;
                phase.r3.r4_shadow_outstanding_sum +=
                    outcome.counters.r4_shadow_outstanding_sum;
                phase.r3.r4_shadow_window_sum += outcome.counters.r4_shadow_window_sum;
                if (!trace_sink.records.empty()) {
                    std::string line;
                    for (const bench::r3::DecisionRecord& record : trace_sink.records) {
                        bench::r3::format_decision_record(record, line);
                        std::fprintf(diag_stream, "%s\n", line.c_str());
                    }
                    std::fflush(diag_stream);
                    trace_sink.records.clear();
                }
                const std::size_t accepted_count = accepted.size();
                for (std::size_t k = 0; k < accepted_count; ++k) {
                    const std::size_t sequence = base + k;
                    window.push_back(
                        Inflight{std::move(accepted[k]), sequence % params.depth,
                                 static_cast<std::uint64_t>(sequence % blocks) * params.block_size,
                                 static_cast<std::uint32_t>(sequence), ready_ns[k]});
                }
                phase.submits += accepted_count;
                submitted += accepted_count;
                if (outcome.error.has_value()) {
                    failure = "submit_read rejected: " + describe(*outcome.error);
                    failed = true;
                    break;
                }
                if (accepted_count != want) {
                    failure = "research adapter disposed " + std::to_string(accepted_count) +
                              " of " + std::to_string(want) + " ready reads";
                    failed = true;
                    break;
                }
            } else {
                for (std::size_t k = 0; k < want; ++k) {
                    const std::size_t sequence = base + k;
                    const std::size_t slot = sequence % params.depth;
                    ReadOp op;
                    op.file = NativeFileRef(file);
                    op.dst = buffers[slot].data();
                    op.len = params.block_size;
                    op.offset =
                        static_cast<std::uint64_t>(sequence % blocks) * params.block_size;
                    const std::int64_t op_ready_ns_i =
                        instrument ? sluice::async::detail::tailtrace::now_ns_i() : 0;
                    const double op_ready_ns =
                        instrument ? static_cast<double>(op_ready_ns_i) : 0.0;
                    if (instrument) {
                        sluice::async::detail::tailtrace::note_built(
                            static_cast<std::uint32_t>(sequence),
                            static_cast<std::uint32_t>(window.size()), op_ready_ns_i);
                    }
                    ++phase.submits;
                    auto result = ctx.submit_read(op);
                    if (!result.has_value()) {
                        failure = "submit_read rejected: " + describe(result.error());
                        failed = true;
                        break;
                    }
                    window.push_back(
                        Inflight{std::move(result).value(), slot,
                                 static_cast<std::uint64_t>(sequence % blocks) * params.block_size,
                                 static_cast<std::uint32_t>(sequence), op_ready_ns});
                    ++submitted;
                }
                if (failed) {
                    break;
                }
            }
            if (params.max_refill != 0) {
                break;
            }
        }
        if (failed) {
            break;
        }
        if (window.empty()) {
            failure = "empty window while work remained";
            failed = true;
            break;
        }

        {
            std::unique_lock<std::mutex> lock(shared.mutex);
            const bool woke = shared.cv.wait_for(
                lock, std::chrono::seconds(20), [&] {
                    return shared.completions > watermark || shared.driver_failed;
                });
            if (!woke) {
                failure = "completion wait timed out (no publication observed)";
                failed = true;
                break;
            }
            watermark = shared.completions;
            if (shared.driver_failed) {
                failure = "driver failure: " + shared.driver_failure;
                failed = true;
                break;
            }
        }

        while (!window.empty() && window.front().request.ready()) {
            Inflight& front = window.front();
            const auto observed = front.request.take_result();
            if (observed.readiness != RequestReadiness::ready || !observed.result.has_value()) {
                failure = "settled result was not successful";
                failed = true;
                break;
            }
            if (observed.result.value() != params.block_size) {
                failure = "short read: " + std::to_string(observed.result.value());
                failed = true;
                break;
            }
            if (verify) {
                const auto mismatch = bench::verify_pattern(buffers[front.slot].data(),
                                                            params.block_size, front.offset);
                if (!mismatch.ok) {
                    failure = "byte mismatch at absolute " +
                              std::to_string(mismatch.first_bad_position) + " (" +
                              std::to_string(mismatch.bad_segments) + " segments)";
                    failed = true;
                    break;
                }
            }
            if (instrument) {
                const std::int64_t t5_ns = sluice::async::detail::tailtrace::now_ns_i();
                phase.latencies.push_back(static_cast<double>(t5_ns) - front.ready_ns);
                sluice::async::detail::tailtrace::note_taken(front.sequence, t5_ns);
            }
            phase.bytes += observed.result.value();
            ++phase.ops;
            ++settled;
            window.pop_front();
        }
    }
    const bench::TimePoint end = bench::now();
    phase.wall_s = bench::seconds_between(start, end);
    if (instrument) {
        sluice::async::detail::tailtrace::end_phase(diag_tag);
    }

    if (failed) {
        phase.ok = false;
        phase.failure = failure;
    }

    // stop must be observable before request_stop wakes the driver: the
    // driver re-checks it only between epoll_wait returns, and a wake
    // consumed while it is still false parks the driver forever.
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.stop = true;
    }
    shared.cv.notify_all();
    ctx.request_stop(ShutdownPolicy::drain);
    driver.join();
    ::close(epoll_fd);

    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        phase.notifications = shared.notifications;
        phase.empty_wakes = shared.empty_wakes;
        phase.passes = shared.passes;
        phase.retry_scheduled = shared.retry_scheduled;
        if (!shared.shutdown_failure.empty() && phase.ok) {
            phase.ok = false;
            phase.failure = shared.shutdown_failure;
        }
        if (!shared.driver_failure.empty() && phase.ok) {
            phase.ok = false;
            phase.failure = "driver: " + shared.driver_failure;
        }
    }

    phase.stats_delta.submit_calls = stats.submit_calls - before.submit_calls;
    phase.stats_delta.submitted_ops = stats.submitted_ops - before.submitted_ops;
    phase.stats_delta.poll_calls = stats.poll_calls - before.poll_calls;
    phase.stats_delta.wait_calls = stats.wait_calls - before.wait_calls;
    phase.stats_delta.completed_ops = stats.completed_ops - before.completed_ops;
    phase.stats_delta.canceled_ops = stats.canceled_ops - before.canceled_ops;
    phase.stats_delta.completion_errors = stats.completion_errors - before.completion_errors;
    phase.stats_delta.short_completions = stats.short_completions - before.short_completions;
    phase.stats_delta.max_outstanding = stats.max_outstanding;
    phase.stats_delta.queue_full_retries = stats.queue_full_retries - before.queue_full_retries;
    phase.stats_delta.invalid_state_rejections =
        stats.invalid_state_rejections - before.invalid_state_rejections;
    return phase;
}

bench::CaseMeta make_meta(const LoopParams& params) {
    bench::CaseMeta meta;
    meta.case_id = "W03-" + params.backend->name + "-" +
                   (params.block_size == 4096 ? "4k" : "64k") + "-d" +
                   std::to_string(params.depth) +
                   (params.backend->name == "threadpool" ? "-w" + std::to_string(params.workers)
                                                         : "") +
                   params.label_suffix;
    meta.workload = "W-03";
    meta.backend = params.backend->name;
    meta.dataset = std::to_string(kFixtureBytes >> 20) + "MiB file, " +
                   std::to_string(params.block_size) + "B positional reads";
    meta.cache_policy = "page_cache_resident";
    meta.filesystem = "tmpfs";
    meta.notes = "app-owned epoll loop on the borrowed progress notification fd; "
                 "driver parks in epoll_wait, submitter parks on its own condition variable";
    meta.block_size = params.block_size;
    meta.request_count = params.request_count;
    meta.concurrency = params.depth;
    meta.queue_depth = params.depth;
    meta.worker_count = params.backend->name == "threadpool" ? params.workers : 0;
    return meta;
}

void fill_stats(bench::Record& record, const AsyncStats& s) {
    record.stats_submit_calls = s.submit_calls;
    record.stats_submitted_ops = s.submitted_ops;
    record.stats_poll_calls = s.poll_calls;
    record.stats_wait_calls = s.wait_calls;
    record.stats_completed_ops = s.completed_ops;
    record.stats_canceled_ops = s.canceled_ops;
    record.stats_completion_errors = s.completion_errors;
    record.stats_short_completions = s.short_completions;
    record.stats_max_outstanding = s.max_outstanding;
    record.stats_queue_full_retries = s.queue_full_retries;
    record.stats_invalid_state = s.invalid_state_rejections;
}

int run_one_repetition(const bench::EnvConfig& cfg, const LoopParams& params,
                       const std::string& path, std::size_t order_index, std::size_t rep,
                       bool measured, bench::r3::Arm arm) {
    const bench::CaseMeta meta = make_meta(params);
    bench::Record record = bench::make_record(meta, cfg, rep, measured, order_index);
    record.r3_arm = static_cast<std::uint64_t>(arm);
    record.notes += std::string("; r3_arm=") + bench::r3::arm_name(arm);

    if (!params.backend->available) {
        record.ok = false;
        record.failure = "NOT_RUN_WITH_REASON: " + params.backend->unavailable_reason;
        bench::emit_record(record);
        bench::print_record(record);
        return 1;
    }

    const double cpu_before = bench::process_cpu_seconds();
    const PhaseResult phase = run_external_phase(
        params, path, false, false, cfg.stats_enabled, arm,
        (meta.case_id + "/rep" + std::to_string(rep) +
         (measured ? "/measured" : "/warmup"))
            .c_str());
    const double cpu_after = bench::process_cpu_seconds();

    if (!phase.ok) {
        record.ok = false;
        record.failure = phase.failure;
        bench::emit_record(record);
        bench::print_record(record);
        return 1;
    }
    record.wall_s = phase.wall_s;
    record.ops = static_cast<double>(phase.ops);
    record.bytes = static_cast<double>(phase.bytes);
    record.ops_per_sec = phase.wall_s > 0.0 ? record.ops / phase.wall_s : 0.0;
    record.mib_per_sec =
        phase.wall_s > 0.0 ? (record.bytes / (1024.0 * 1024.0)) / phase.wall_s : 0.0;
    record.cpu_s = cpu_after - cpu_before;
    record.cpu_util = phase.wall_s > 0.0 ? record.cpu_s / phase.wall_s : 0.0;
    record.rss_kb = bench::resident_kb();
    record.peak_rss_kb = bench::peak_rss_kb();
    record.submits = phase.submits;
    record.notifications = phase.notifications;
    record.empty_wakes = phase.empty_wakes;
    record.passes = phase.passes;
    record.extra_a = phase.retry_scheduled;
    record.r3_entry_calls = phase.r3.entry_calls;
    record.r3_group_calls = phase.r3.group_calls;
    record.r3_single_calls = phase.r3.single_calls;
    record.r3_accepted = phase.r3.accepted;
    record.r3_prefix_max = phase.r3.prefix_max;
    record.r3_suffix_single_calls = phase.r3.suffix_single_calls;
    record.r3_flush_attempts = phase.r3.flush_attempts;
    record.r3_ineligible_fallbacks = phase.r3.ineligible_fallbacks;
    record.r3_gate_stops = phase.r3.gate_stops;
    record.r3_untouched = phase.r3.untouched;
    record.r4_batch_presentations = phase.r3.r4_batch_presentations;
    record.r4_cap_reentries = phase.r3.r4_cap_reentries;
    record.r4_chunk_reentries = phase.r3.r4_chunk_reentries;
    record.r4_immediate_decisions = phase.r3.r4_immediate_decisions;
    record.r4_authority_exits = phase.r3.r4_authority_exits;
    record.r4_quantum1 = phase.r3.r4_quantum1;
    record.r4_quantum2 = phase.r3.r4_quantum2;
    record.r4_quantum8 = phase.r3.r4_quantum8;
    record.r4_active_zero_decisions = phase.r3.r4_active_zero_decisions;
    record.r4_pressure_decisions = phase.r3.r4_pressure_decisions;
    record.r4_shadow_decisions = phase.r3.r4_shadow_decisions;
    record.r4_shadow_outstanding_sum = phase.r3.r4_shadow_outstanding_sum;
    record.r4_shadow_window_sum = phase.r3.r4_shadow_window_sum;

    std::optional<std::string> stats_failure =
        bench::stats_wiring_failure(phase.stats_delta, cfg.stats_enabled);
    if (!stats_failure.has_value() && cfg.stats_enabled) {
        if (phase.stats_delta.submit_calls != phase.submits ||
            phase.stats_delta.submitted_ops != phase.submits) {
            stats_failure = "stats wiring oracle: submit counters do not match the "
                            "submissions issued by this phase";
        } else if (phase.stats_delta.completed_ops != phase.ops) {
            stats_failure = "stats wiring oracle: completed_ops does not match the "
                            "operations settled by this phase";
        } else if (phase.stats_delta.poll_calls == 0) {
            stats_failure = "stats wiring oracle: poll_calls is zero on a phase driven "
                            "through poll_progress";
        } else if (phase.stats_delta.wait_calls != 0) {
            stats_failure = "stats wiring oracle: the external driver recorded wait "
                            "calls although it never parks in wait_progress";
        } else if (phase.stats_delta.queue_full_retries != 0 ||
                   phase.stats_delta.invalid_state_rejections != 0) {
            stats_failure = "stats wiring oracle: a successful phase recorded rejected "
                            "submissions";
        } else if (phase.stats_delta.max_outstanding == 0 ||
                   phase.stats_delta.max_outstanding > params.depth) {
            stats_failure = "stats wiring oracle: max_outstanding outside the phase "
                            "window";
        }
    }
    if (stats_failure.has_value()) {
        record.ok = false;
        record.failure = stats_failure.value();
        bench::emit_record(record);
        bench::print_record(record);
        return 1;
    }
    fill_stats(record, phase.stats_delta);

    int failed = 0;

    const PhaseResult oracle = run_external_phase(
        params, path, false, true, cfg.stats_enabled, arm,
        (meta.case_id + "/rep" + std::to_string(rep) + "/oracle").c_str());
    if (!oracle.ok) {
        record.ok = false;
        record.failure = "correctness oracle: " + oracle.failure;
        failed = 1;
    }

    const PhaseResult instrumented = run_external_phase(
        params, path, true, false, cfg.stats_enabled, arm,
        (meta.case_id + "/rep" + std::to_string(rep) + "/instrumented").c_str());
    if (!instrumented.ok) {
        record.ok = false;
        record.failure = "latency pass: " + instrumented.failure;
        failed = 1;
    } else {
        record.latency = bench::compute_stats(instrumented.latencies);
    }

    bench::emit_record(record);
    bench::print_record(record);
    return failed;
}

} // namespace

int main() {
    const bench::EnvConfig cfg = bench::env_config();
    std::printf("========================================================\n");
    std::printf("G0 W-03 external event loop benchmark\n");
    std::printf("warmup=%zu measured_repetitions=%zu stats_wiring=%s\n", cfg.warmup,
                cfg.measured, cfg.stats_enabled ? "on" : "off");
    std::printf("========================================================\n");

    bench::ensure_dir(cfg.tmp_dir);
    const std::string path = cfg.tmp_dir + "/external.bin";
    std::string failure;
    if (!write_fixture(path, failure)) {
        std::fprintf(stderr, "fixture creation failed: %s\n", failure.c_str());
        return 1;
    }

    const BackendChoice threadpool{"threadpool", true, {}};
    BackendChoice uring{"uring", true, {}};
    {
        std::string reason;
        auto context = make_context(uring, 8, 0, reason, nullptr);
        if (!context.has_value()) {
            uring.available = false;
            uring.unavailable_reason = reason;
            std::fprintf(stderr, "io_uring backend unavailable: %s\n", reason.c_str());
        }
    }

    std::vector<LoopParams> cases;
    auto add = [&](const BackendChoice& backend, std::size_t depth, std::size_t workers) {
        LoopParams p;
        p.backend = &backend;
        p.block_size = 4096;
        p.request_count = 20000;
        p.depth = depth;
        p.workers = workers;
        cases.push_back(p);
    };
    add(threadpool, 4, 4);
    add(uring, 4, 0);
    add(threadpool, 16, 4);
    add(uring, 16, 0);
    add(threadpool, 64, 4);
    add(uring, 64, 0);

    if (bench::r3::research_cases_enabled()) {
        auto add_r3 = [&](std::size_t block_size, std::size_t request_count, std::size_t depth,
                          std::size_t max_refill, const char* suffix) {
            LoopParams p;
            p.backend = &uring;
            p.block_size = block_size;
            p.request_count = request_count;
            p.depth = depth;
            p.workers = 0;
            p.max_refill = max_refill;
            p.label_suffix = suffix;
            cases.push_back(p);
        };
        add_r3(4096, 2, 2, 2, "-r3d2pos");
        add_r3(4096, 20000, 2, 1, "-r3d2serial");
        add_r3(4096, 20000, 3, 3, "-r3d3");
        add_r3(4096, 64, 16, 16, "-r3d16cap");
        add_r3(65536, 2000, 16, 16, "-r3d16-64k");
    }

    const bench::r3::Arm arm = bench::r3::arm_from_env();
    std::printf("research arm=%s (SLUICE_595_R3_ARM), research cases=%s\n",
                bench::r3::arm_name(arm),
                bench::r3::research_cases_enabled() ? "on" : "off");

    int failed = bench::oracle_self_test_case(cfg);
    for (std::size_t round = 0; round < cfg.warmup + cfg.measured; ++round) {
        const bool measured = round >= cfg.warmup;
        const std::vector<std::size_t> order = bench::rotation_order(cases.size(), round);
        for (std::size_t position = 0; position < order.size(); ++position) {
            const LoopParams& params = cases[order[position]];
            const bench::CaseMeta meta = make_meta(params);
            if (!bench::case_selected(meta.case_id)) {
                continue;
            }
            failed += run_one_repetition(cfg, params, path, position,
                                        measured ? round - cfg.warmup : round, measured, arm);
        }
    }

    std::printf("\n=== W-03 complete, failing repetitions=%d ===\n", failed);
    return failed == 0 ? 0 : 1;
}
