// W-02 bounded pipeline: a sliding window of in-flight positional reads; each
// in-flight request owns its own destination buffer.

#include "bench_common.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request_scope.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/measurement.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

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
using sluice::async::RequestObservation;
using sluice::async::RequestReadiness;
using sluice::async::RequestScope;
using sluice::async::ScopeCleanupPolicy;
using sluice::async::ScopeTicket;
using sluice::async::ScopeWaitStatus;
using sluice::async::ShutdownPolicy;
using sluice::async::ThreadPoolConfig;

constexpr std::size_t kInstrumentedSampleCap = 200000;
constexpr std::size_t kScratchBytes = 2u << 20;
constexpr std::size_t kFixtureBytes = 64u << 20;
constexpr std::size_t kSaturationRounds = 200;
constexpr auto kWaitDeadline = std::chrono::seconds(20);

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
    std::string name; // "threadpool" | "uring"
    bool available = true;
    std::string unavailable_reason;
};

std::optional<AsyncIoContext> make_context(const BackendChoice& choice, std::size_t depth,
                                            std::size_t workers, std::string& failure) {
    if (choice.name == "threadpool") {
        ThreadPoolConfig config;
        config.request_capacity = depth;
        config.worker_count = workers;
        try {
            return sluice::async::make_threadpool_context(config);
        } catch (const std::exception& error) {
            failure = error.what();
            return std::nullopt;
        }
    }
    try {
        sluice::async::UringConfig config;
        config.request_capacity = depth;
        config.queue_depth = static_cast<unsigned>(depth);
        auto made = sluice::async::make_uring_context(config);
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


struct PipelineParams {
    const BackendChoice* backend = nullptr;
    std::size_t block_size = 4096;
    std::size_t request_count = 20000;
    std::size_t depth = 16;
    std::size_t workers = 4;
    bool saturating = false;
    // Scope tracking capacity; 0 means "same as depth". Varying it alone
    // isolates the scope's per-take slot scan from the pipeline concurrency.
    std::size_t scope_capacity = 0;
    // Backend request-slot capacity; 0 means "same as depth". Varying it alone
    // isolates the request core's slot scans from the pipeline concurrency.
    std::size_t core_capacity = 0;
    // Diagnostic: consume whichever request is ready first instead of the oldest.
    bool any_ready_consume = false;
    std::string label_suffix;
};

struct PipelineCounters {
    std::uint64_t submits = 0;
    std::uint64_t would_block = 0;
    std::uint64_t waits = 0;
    std::uint64_t accepted_at_saturation = 0;
    std::uint64_t rejected_at_saturation = 0;
};

struct PhaseResult {
    bool ok = true;
    std::string failure;
    double wall_s = 0.0;
    std::size_t ops = 0;
    std::size_t bytes = 0;
    PipelineCounters counters;
    std::vector<double> latencies;
    AsyncStats stats_delta;
};

AsyncStats stats_diff(const AsyncStats& after, const AsyncStats& before) {
    AsyncStats d;
    d.submit_calls = after.submit_calls - before.submit_calls;
    d.submitted_ops = after.submitted_ops - before.submitted_ops;
    d.poll_calls = after.poll_calls - before.poll_calls;
    d.wait_calls = after.wait_calls - before.wait_calls;
    d.completed_ops = after.completed_ops - before.completed_ops;
    d.canceled_ops = after.canceled_ops - before.canceled_ops;
    d.completion_errors = after.completion_errors - before.completion_errors;
    d.short_completions = after.short_completions - before.short_completions;
    d.max_outstanding = after.max_outstanding;
    d.queue_full_retries = after.queue_full_retries - before.queue_full_retries;
    d.invalid_state_rejections =
        after.invalid_state_rejections - before.invalid_state_rejections;
    return d;
}

// One bounded sliding-window pipeline phase; `instrument` adds per-operation
// submit/settle timestamps and `verify` checks returned bytes against the oracle.
PhaseResult run_pipeline_phase(const PipelineParams& params, const std::string& path,
                               bool instrument, bool verify) {
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
    const std::size_t backend_capacity =
        params.core_capacity != 0
            ? params.core_capacity
            : std::max(params.depth,
                       params.scope_capacity != 0 ? params.scope_capacity : params.depth);
    auto context =
        make_context(*params.backend, backend_capacity, params.workers, backend_failure);
    if (!context.has_value()) {
        phase.ok = false;
        phase.failure = "NOT_RUN_WITH_REASON: " + backend_failure;
        return phase;
    }
    AsyncIoContext ctx = std::move(*context);

    const std::size_t blocks = (kFixtureBytes - params.block_size) / params.block_size;
    std::vector<std::vector<std::byte>> buffers(
        params.depth, std::vector<std::byte>(params.block_size));
    for (std::size_t i = 0; i < params.depth; ++i) {
        bench::fill_pattern(buffers[i].data(), params.block_size, 0);
    }

    struct Inflight {
        ScopeTicket<std::size_t> ticket;
        std::size_t slot = 0;
        std::uint64_t offset = 0;
        double submitted_ns = 0.0;
    };

    PhaseResult result;
    if (params.saturating) {
        // Backpressure shape: fill the bound until admission refuses, then settle
        // the accepted window; rejection at the bound is counted, not failed.
        const double start = bench::now_ns() * 1e-9;
        for (std::size_t round = 0; round < kSaturationRounds; ++round) {
            const std::size_t scope_capacity =
        params.scope_capacity != 0 ? params.scope_capacity : params.depth;
    RequestScope scope(ctx, scope_capacity, ScopeCleanupPolicy::drain);
            std::vector<Inflight> inflight;
            std::size_t sequence = round * params.depth;
            for (;;) {
                const std::size_t index = sequence % params.depth;
                const std::uint64_t offset =
                    static_cast<std::uint64_t>(sequence % blocks) * params.block_size;
                ReadOp op;
                op.file = NativeFileRef(file);
                op.dst = buffers[index].data();
                op.len = params.block_size;
                op.offset = offset;
                ++result.counters.submits;
                auto ticket = scope.submit_read(op);
                if (!ticket.has_value()) {
                    if (ticket.error().code == IoError::Code::would_block) {
                        ++result.counters.would_block;
                        ++result.counters.rejected_at_saturation;
                        break;
                    }
                    result.ok = false;
                    result.failure = "submit failed: " + describe(ticket.error());
                    return result;
                }
                ++result.counters.accepted_at_saturation;
                inflight.push_back(Inflight{ticket.value(), index, offset, 0.0});
                ++sequence;
            }
            for (auto& entry : inflight) {
                ++result.counters.waits;
                auto waited = scope.wait_for(entry.ticket, kWaitDeadline);
                if (!waited.has_value()) {
                    result.ok = false;
                    result.failure = "wait failed: " + describe(waited.error());
                    return result;
                }
                if (waited.value() != ScopeWaitStatus::ready) {
                    result.ok = false;
                    result.failure = "wait did not reach ready";
                    return result;
                }
                const auto observed = scope.take(entry.ticket);
                if (observed.readiness != RequestReadiness::ready ||
                    !observed.result.has_value()) {
                    result.ok = false;
                    result.failure = "settled result was not successful";
                    return result;
                }
                if (verify) {
                    const auto mismatch = bench::verify_pattern(buffers[entry.slot].data(),
                                                               params.block_size, entry.offset);
                    if (!mismatch.ok) {
                        result.ok = false;
                        result.failure = "byte mismatch at absolute " +
                                         std::to_string(mismatch.first_bad_position);
                        return result;
                    }
                }
                ++result.ops;
            result.bytes += observed.result.value();
        }
            auto finished = scope.finish();
            if (!finished.has_value()) {
                result.ok = false;
                result.failure = "scope finish failed";
                return result;
            }
        }
        const double end = bench::now_ns() * 1e-9;
        result.wall_s = end - start;
        const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
        if (!outcome.has_value()) {
            result.ok = false;
            result.failure = "shutdown failed";
            return result;
        }
        result.stats_delta = stats;
        return result;
    }

    AsyncStats before = stats;
    const std::size_t scope_capacity =
        params.scope_capacity != 0 ? params.scope_capacity : params.depth;
    RequestScope scope(ctx, scope_capacity, ScopeCleanupPolicy::drain);
    std::deque<Inflight> window;
    std::size_t submitted = 0;
    std::size_t completed = 0;
    // A destination buffer becomes reusable only once the request holding it has
    // settled: consumers may take results out of submission order.
    std::vector<std::size_t> free_slots(params.depth);
    for (std::size_t i = 0; i < params.depth; ++i) {
        free_slots[i] = i;
    }

    const bench::TimePoint start = bench::now();
    while (completed < params.request_count) {
        while (window.size() < params.depth && submitted < params.request_count) {
            const std::size_t slot = free_slots.back();
            free_slots.pop_back();
            const std::uint64_t offset = static_cast<std::uint64_t>(submitted % blocks) *
                                         params.block_size;
            ReadOp op;
            op.file = NativeFileRef(file);
            op.dst = buffers[slot].data();
            op.len = params.block_size;
            op.offset = offset;
            ++result.counters.submits;
            auto ticket = scope.submit_read(op);
            if (!ticket.has_value()) {
                if (ticket.error().code == IoError::Code::would_block) {
                    ++result.counters.would_block;
                    break;
                }
                result.ok = false;
                result.failure = "submit failed: " + describe(ticket.error());
                return result;
            }
            window.push_back(
                Inflight{ticket.value(), slot, offset,
                         instrument ? bench::now_ns() : 0.0});
            ++submitted;
        }
        if (window.empty()) {
            result.ok = false;
            result.failure = "pipeline stalled with an empty window at operation " +
                             std::to_string(completed);
            return result;
        }
        // Default consumes in submission order; the diagnostic consumes whichever
        // request reports published first.
        std::size_t victim = window.size();
        if (params.any_ready_consume) {
            for (std::size_t index = 0; index < window.size(); ++index) {
                if (ctx.lookup(window[index].ticket.id()) == RequestReadiness::ready) {
                    victim = index;
                    break;
                }
            }
        }
        RequestObservation<std::size_t> observed;
        if (victim == window.size()) {
            Inflight& oldest = window.front();
            ++result.counters.waits;
            auto waited = scope.wait_for(oldest.ticket, kWaitDeadline);
            if (!waited.has_value()) {
                result.ok = false;
                result.failure = "wait failed: " + describe(waited.error());
                return result;
            }
            if (waited.value() != ScopeWaitStatus::ready) {
                result.ok = false;
                result.failure = "wait outcome was not ready at operation " +
                                 std::to_string(completed);
                return result;
            }
            observed = scope.take(oldest.ticket);
            victim = 0;
        } else {
            observed = scope.take(window[victim].ticket);
        }
        if (observed.readiness != RequestReadiness::ready) {
            result.ok = false;
            result.failure = "take did not observe a ready result at operation " +
                             std::to_string(completed);
            return result;
        }
        Inflight& entry = window[victim];
        if (!observed.result.has_value()) {
            result.ok = false;
            result.failure = "operation failed: " + describe(observed.result.error());
            return result;
        }
        if (observed.result.value() != params.block_size) {
            result.ok = false;
            result.failure = "short read: " + std::to_string(observed.result.value()) + " of " +
                             std::to_string(params.block_size);
            return result;
        }
        if (instrument) {
            result.latencies.push_back(bench::now_ns() - entry.submitted_ns);
        }
        if (verify) {
            const auto mismatch = bench::verify_pattern(buffers[entry.slot].data(),
                                                       params.block_size, entry.offset);
            if (!mismatch.ok) {
                result.ok = false;
                result.failure = "byte mismatch at absolute " +
                                 std::to_string(mismatch.first_bad_position) +
                                 " (operation " + std::to_string(completed) + ", buffer slot " +
                                 std::to_string(entry.slot) + ", offset " +
                                 std::to_string(entry.offset) + ")";
                return result;
            }
        }
        result.bytes += observed.result.value();
        ++result.ops;
        ++completed;
        free_slots.push_back(entry.slot);
        window.erase(window.begin() + static_cast<std::ptrdiff_t>(victim));
    }
    const bench::TimePoint end = bench::now();
    result.wall_s = bench::seconds_between(start, end);

    auto finished = scope.finish();
    if (!finished.has_value()) {
        result.ok = false;
        result.failure = "scope finish failed";
        return result;
    }
    const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
    if (!outcome.has_value()) {
        result.ok = false;
        result.failure = "shutdown failed";
        return result;
    }
    result.stats_delta = stats_diff(stats, before);
    return result;
}

bench::CaseMeta make_meta(const PipelineParams& params) {
    bench::CaseMeta meta;
    const std::string bound = std::to_string(params.depth);
    meta.case_id = "W02-" + params.backend->name + "-" +
                   (params.block_size == 4096 ? "4k" : "64k") + "-d" + bound +
                   (params.backend->name == "threadpool" ? "-w" + std::to_string(params.workers)
                                                         : "") +
                   (params.saturating ? "-saturate" : "") + params.label_suffix;
    meta.workload = "W-02";
    meta.backend = params.backend->name;
    meta.dataset = std::to_string(kFixtureBytes >> 20) + "MiB file, " +
                   std::to_string(params.block_size) + "B positional reads";
    meta.cache_policy = "page_cache_resident";
    meta.filesystem = "tmpfs";
    meta.notes = params.saturating
                     ? "submit until admission refuses, then settle; backpressure counted"
                     : "bounded sliding window; unconsumed results never exceed the bound";
    meta.block_size = params.block_size;
    meta.request_count = params.saturating ? kSaturationRounds * params.depth
                                           : params.request_count;
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

bool uring_constructible(const BackendChoice& choice, std::string& reason) {
    std::string failure;
    auto context = make_context(choice, 8, 1, failure);
    if (!context.has_value()) {
        reason = failure.empty() ? "construction failed" : failure;
        return false;
    }
    return true;
}

int run_one_repetition(const bench::EnvConfig& cfg, const PipelineParams& params,
                       const std::string& path, std::size_t order_index, std::size_t rep,
                       bool measured) {
    const bench::CaseMeta meta = make_meta(params);
    bench::Record record = bench::make_record(meta, cfg, rep, measured, order_index);

    if (!params.backend->available) {
        record.ok = false;
        record.failure = "NOT_RUN_WITH_REASON: " + params.backend->unavailable_reason;
        bench::emit_record(record);
        bench::print_record(record);
        return 1;
    }

    const double cpu_before = bench::process_cpu_seconds();
    const PhaseResult phase = run_pipeline_phase(params, path, false, false);
    const double cpu_after = bench::process_cpu_seconds();

    int failed = 0;
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
    record.submits = phase.counters.submits;
    record.would_block = phase.counters.would_block;
    record.waits = phase.counters.waits;
    record.extra_a = phase.counters.accepted_at_saturation;
    record.extra_b = phase.counters.rejected_at_saturation;
    fill_stats(record, phase.stats_delta);

    const PhaseResult oracle = run_pipeline_phase(params, path, false, true);
    if (!oracle.ok) {
        record.ok = false;
        record.failure = "correctness oracle: " + oracle.failure;
        failed = 1;
    }

    const PhaseResult instrumented = run_pipeline_phase(params, path, true, false);
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
    std::printf("G0 W-02 bounded RequestScope pipeline benchmark\n");
    std::printf("warmup=%zu measured_repetitions=%zu\n", cfg.warmup, cfg.measured);
    std::printf("========================================================\n");

    bench::ensure_dir(cfg.tmp_dir);
    const std::string path = cfg.tmp_dir + "/pipeline.bin";
    std::string failure;
    if (!write_fixture(path, failure)) {
        std::fprintf(stderr, "fixture creation failed: %s\n", failure.c_str());
        return 1;
    }

    const BackendChoice threadpool{"threadpool", true, {}};
    BackendChoice uring{"uring", true, {}};
    {
        std::string reason;
        if (!uring_constructible(uring, reason)) {
            uring.available = false;
            uring.unavailable_reason = reason;
            std::fprintf(stderr, "io_uring backend unavailable: %s\n", reason.c_str());
        }
    }

    std::vector<PipelineParams> cases;
    auto add_tp = [&](std::size_t block, std::size_t count, std::size_t depth,
                      std::size_t workers, bool saturating) {
        PipelineParams p;
        p.backend = &threadpool;
        p.block_size = block;
        p.request_count = count;
        p.depth = depth;
        p.workers = workers;
        p.saturating = saturating;
        cases.push_back(p);
    };
    auto add_uring = [&](std::size_t block, std::size_t count, std::size_t depth,
                         bool saturating) {
        PipelineParams p;
        p.backend = &uring;
        p.block_size = block;
        p.request_count = count;
        p.depth = depth;
        p.workers = 0;
        p.saturating = saturating;
        cases.push_back(p);
    };

    add_tp(4096, 20000, 1, 1, false);
    add_uring(4096, 20000, 1, false);
    add_tp(4096, 20000, 4, 4, false);
    add_uring(4096, 20000, 4, false);
    add_tp(4096, 20000, 16, 4, false);
    add_uring(4096, 20000, 16, false);
    add_tp(4096, 20000, 64, 4, false);
    add_uring(4096, 20000, 64, false);
    add_tp(4096, 20000, 16, 1, false);
    add_tp(4096, 20000, 16, 8, false);
    add_tp(65536, 2000, 16, 4, false);
    add_uring(65536, 2000, 16, false);
    add_tp(4096, 16, 16, 4, true);
    add_uring(4096, 16, 16, true);

    // Diagnostics vary one capacity at a time while the pipeline depth is fixed.
    {
        PipelineParams p;
        p.backend = &threadpool;
        p.block_size = 4096;
        p.request_count = 20000;
        p.depth = 4;
        p.workers = 4;
        p.scope_capacity = 64;
        p.core_capacity = 4;
        p.label_suffix = "-scope64";
        cases.push_back(p);
    }
    {
        PipelineParams p;
        p.backend = &threadpool;
        p.block_size = 4096;
        p.request_count = 20000;
        p.depth = 16;
        p.workers = 4;
        p.scope_capacity = 64;
        p.core_capacity = p.depth;
        p.label_suffix = "-scope64";
        cases.push_back(p);
    }
    // Opt-in: a default rebuild reproduces the frozen case list exactly.
    if (std::getenv("SLUICE_BENCH_DIAGNOSTICS") != nullptr) {
        PipelineParams p;
        p.backend = &threadpool;
        p.block_size = 4096;
        p.request_count = 20000;
        p.depth = 16;
        p.workers = 4;
        p.scope_capacity = 16;
        p.core_capacity = 64;
        p.label_suffix = "-core64";
        cases.push_back(p);

        PipelineParams ordered;
        ordered.backend = &threadpool;
        ordered.block_size = 4096;
        ordered.request_count = 20000;
        ordered.depth = 16;
        ordered.workers = 4;
        ordered.scope_capacity = 16;
        ordered.core_capacity = 16;
        ordered.any_ready_consume = true;
        ordered.label_suffix = "-anyready";
        cases.push_back(ordered);

        PipelineParams ordered_sat;
        ordered_sat.backend = &threadpool;
        ordered_sat.block_size = 4096;
        ordered_sat.request_count = 20000;
        ordered_sat.depth = 64;
        ordered_sat.workers = 4;
        ordered_sat.scope_capacity = 64;
        ordered_sat.core_capacity = 64;
        ordered_sat.any_ready_consume = true;
        ordered_sat.label_suffix = "-anyready";
        cases.push_back(ordered_sat);
    }

    int failed = bench::oracle_self_test_case(cfg);
    for (std::size_t round = 0; round < cfg.warmup + cfg.measured; ++round) {
        const bool measured = round >= cfg.warmup;
        const std::vector<std::size_t> order = bench::rotation_order(cases.size(), round);
        for (std::size_t position = 0; position < order.size(); ++position) {
            const PipelineParams& params = cases[order[position]];
            const bench::CaseMeta meta = make_meta(params);
            if (!bench::case_selected(meta.case_id)) {
                continue;
            }
            failed += run_one_repetition(cfg, params, path, position,
                                        measured ? round - cfg.warmup : round, measured);
        }
    }

    std::printf("\n=== W-02 complete, failing repetitions=%d ===\n", failed);
    return failed == 0 ? 0 : 1;
}
