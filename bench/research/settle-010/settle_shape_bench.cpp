// ELIDE-010 settlement-shape driver (research branch only, never master).
// One in-flight positional read per freshly abandoned RequestScope: the
// destructor's settle_and_release_ convergence loop runs while the operation
// is still outstanding, which is the shape the candidate removes one backend
// progress pass from. Four cases: {threadpool, io_uring} x {drain,
// cancel_then_drain}. Links the same release archives the bench targets use.

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
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using sluice::File;
using sluice::FileAccess;
using sluice::FileExistence;
using sluice::FileInitialContents;
using sluice::FileOpen;
using sluice::AsyncStats;
using sluice::IoError;
using sluice::async::AsyncIoContext;
using sluice::async::NativeFileRef;
using sluice::async::ReadOp;
using sluice::async::RequestScope;
using sluice::async::ScopeCleanupPolicy;
using sluice::async::ShutdownPolicy;
using sluice::async::ThreadPoolConfig;

constexpr std::size_t kFixtureBytes = 8u << 20;
constexpr std::size_t kBlockSize = 4096;
constexpr std::size_t kScopeCapacity = 64;
constexpr std::size_t kWorkers = 4;

struct BackendChoice {
    std::string name;
    bool available = true;
    std::string unavailable_reason;
};

struct CaseParams {
    const BackendChoice* backend = nullptr;
    ScopeCleanupPolicy policy = ScopeCleanupPolicy::drain;
    std::size_t ops = 20000;
};

std::string describe(const sluice::IoError& e) {
    return std::string(sluice::to_string(e.code)) + "(errno=" + std::to_string(e.os_errno) + ")";
}

bool write_fixture(const std::string& path, std::string& failure) {
    FileOpen mode;
    mode.access = FileAccess::read_write;
    mode.existence = FileExistence::open_existing;
    auto opened = File::open(path, mode);
    if (opened.has_value()) {
        return true;
    }
    mode.existence = FileExistence::create_if_missing;
    mode.contents = sluice::FileInitialContents::truncate;
    opened = File::open(path, mode);
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

std::optional<AsyncIoContext> make_context(const BackendChoice& choice, std::string& failure,
                                           AsyncStats* stats) {
    if (choice.name == "threadpool") {
        ThreadPoolConfig config;
        config.request_capacity = kScopeCapacity;
        config.worker_count = kWorkers;
        try {
            return sluice::async::make_threadpool_context(config, stats);
        } catch (const std::exception& error) {
            failure = error.what();
            return std::nullopt;
        }
    }
    try {
        sluice::async::UringConfig config;
        config.request_capacity = kScopeCapacity;
        config.queue_depth = static_cast<unsigned>(kScopeCapacity);
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

struct PhaseResult {
    bool ok = true;
    std::string failure;
    double wall_s = 0.0;
    std::size_t ops = 0;
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

PhaseResult run_pass(const CaseParams& params, const std::string& path, bool instrument,
                     bool verify, bool stats_enabled) {
    PhaseResult phase;

    auto opened = File::open(path, [] {
        FileOpen mode;
        mode.access = FileAccess::read_write;
        mode.existence = FileExistence::open_existing;
        return mode;
    }());
    if (!opened.has_value()) {
        phase.ok = false;
        phase.failure = "open failed: " + describe(opened.error());
        return phase;
    }
    File file = std::move(opened).value();

    AsyncStats stats;
    std::string backend_failure;
    auto context = make_context(*params.backend, backend_failure, stats_enabled ? &stats : nullptr);
    if (!context.has_value()) {
        phase.ok = false;
        phase.failure = "NOT_RUN_WITH_REASON: " + backend_failure;
        return phase;
    }
    AsyncIoContext ctx = std::move(*context);

    const std::size_t blocks = (kFixtureBytes - kBlockSize) / kBlockSize;
    std::vector<std::byte> buffer(kBlockSize, std::byte{0});
    const bool drain = params.policy == ScopeCleanupPolicy::drain;

    const AsyncStats before = stats;
    const bench::TimePoint start = bench::now();
    for (std::size_t op = 0; op < params.ops; ++op) {
        const std::uint64_t offset =
            static_cast<std::uint64_t>(op % blocks) * kBlockSize;
        const double submitted_ns = instrument ? bench::now_ns() : 0.0;
        {
            RequestScope scope(ctx, kScopeCapacity, params.policy);
            auto ticket =
                scope.submit_read(ReadOp{NativeFileRef{file}, buffer.data(), buffer.size(), offset});
            if (!ticket.has_value()) {
                phase.ok = false;
                phase.failure = "submit failed at operation " + std::to_string(op) + ": " +
                                describe(ticket.error());
                return phase;
            }
            // Scope ends here: settle_and_release_ runs against the in-flight op.
        }
        if (instrument) {
            phase.latencies.push_back(bench::now_ns() - submitted_ns);
        }
        if (verify && drain) {
            const auto mismatch =
                bench::verify_pattern(buffer.data(), kBlockSize, offset);
            if (!mismatch.ok) {
                phase.ok = false;
                phase.failure = "byte mismatch at absolute " +
                                std::to_string(mismatch.first_bad_position) +
                                " (operation " + std::to_string(op) + ")";
                return phase;
            }
        }
    }
    const bench::TimePoint end = bench::now();
    phase.wall_s = bench::seconds_between(start, end);
    phase.ops = params.ops;

    const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
    if (!outcome.has_value()) {
        phase.ok = false;
        phase.failure = "shutdown failed";
        return phase;
    }
    phase.stats_delta = stats_diff(stats, before);
    return phase;
}

bench::CaseMeta make_meta(const CaseParams& params) {
    bench::CaseMeta meta;
    meta.case_id = "S10-" + params.backend->name + "-" +
                   (params.policy == ScopeCleanupPolicy::drain ? "drain" : "cancel") + "-c64";
    meta.workload = "S10-settle-shape";
    meta.backend = params.backend->name;
    meta.dataset = std::to_string(kFixtureBytes >> 20) + "MiB file, " +
                   std::to_string(kBlockSize) + "B positional reads, one per abandoned scope";
    meta.cache_policy = "page_cache_resident";
    meta.filesystem = "tmpfs";
    meta.notes = params.policy == ScopeCleanupPolicy::drain
                     ? "per-op destructor settlement against an in-flight read (drain)"
                     : "per-op destructor settlement after cancel sweep (cancel_then_drain)";
    meta.block_size = kBlockSize;
    meta.request_count = params.ops;
    meta.concurrency = 1;
    meta.queue_depth = kScopeCapacity;
    meta.worker_count = params.backend->name == "threadpool" ? kWorkers : 0;
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

int run_one_repetition(const bench::EnvConfig& cfg, const CaseParams& params,
                       const std::string& path, std::size_t rep, bool measured) {
    const bench::CaseMeta meta = make_meta(params);
    bench::Record record = bench::make_record(meta, cfg, rep, measured, 0);

    if (!params.backend->available) {
        record.ok = false;
        record.failure = "NOT_RUN_WITH_REASON: " + params.backend->unavailable_reason;
        bench::emit_record(record);
        bench::print_record(record);
        return 1;
    }

    const double cpu_before = bench::process_cpu_seconds();
    const PhaseResult phase = run_pass(params, path, false, false, cfg.stats_enabled);
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
    record.ops_per_sec = phase.wall_s > 0.0 ? record.ops / phase.wall_s : 0.0;
    record.cpu_s = cpu_after - cpu_before;
    record.cpu_util = phase.wall_s > 0.0 ? record.cpu_s / phase.wall_s : 0.0;
    record.rss_kb = bench::resident_kb();
    record.peak_rss_kb = bench::peak_rss_kb();
    fill_stats(record, phase.stats_delta);

    if (cfg.stats_enabled) {
        if (record.stats_submit_calls != static_cast<std::uint64_t>(phase.ops) ||
            record.stats_submitted_ops != static_cast<std::uint64_t>(phase.ops)) {
            record.ok = false;
            record.failure = "stats wiring oracle: submit counters do not match the "
                             "operations submitted by this phase";
            bench::emit_record(record);
            bench::print_record(record);
            return 1;
        }
        if (record.stats_completion_errors != 0 || record.stats_short_completions != 0) {
            record.ok = false;
            record.failure = "stats wiring oracle: completion errors or short completions "
                             "on a settlement pass";
            bench::emit_record(record);
            bench::print_record(record);
            return 1;
        }
    }

    int failed = 0;
    const PhaseResult oracle = run_pass(params, path, false, true, cfg.stats_enabled);
    if (!oracle.ok) {
        record.ok = false;
        record.failure = "correctness oracle: " + oracle.failure;
        failed = 1;
    }

    const PhaseResult instrumented = run_pass(params, path, true, false, cfg.stats_enabled);
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
    std::printf("ELIDE-010 S10 settlement-shape benchmark\n");
    std::printf("warmup=%zu measured_repetitions=%zu stats_wiring=%s\n", cfg.warmup,
                cfg.measured, cfg.stats_enabled ? "on" : "off");
    std::printf("========================================================\n");

    bench::ensure_dir(cfg.tmp_dir);
    const std::string path = cfg.tmp_dir + "/settle-shape.bin";
    std::string failure;
    if (!write_fixture(path, failure)) {
        std::fprintf(stderr, "fixture creation failed: %s\n", failure.c_str());
        return 1;
    }

    const BackendChoice threadpool{"threadpool", true, {}};
    BackendChoice uring{"uring", true, {}};
    {
        std::string reason;
        std::optional<AsyncIoContext> probe = make_context(uring, reason, nullptr);
        if (!probe.has_value()) {
            uring.available = false;
            uring.unavailable_reason = reason;
            std::fprintf(stderr, "io_uring backend unavailable: %s\n", reason.c_str());
        }
    }

    std::vector<CaseParams> cases;
    const BackendChoice* backends[] = {&threadpool, &uring};
    for (const BackendChoice* backend : backends) {
        for (ScopeCleanupPolicy policy :
             {ScopeCleanupPolicy::drain, ScopeCleanupPolicy::cancel_then_drain}) {
            cases.push_back(CaseParams{backend, policy, 20000});
        }
    }

    int failed = bench::oracle_self_test_case(cfg);
    for (std::size_t round = 0; round < cfg.warmup + cfg.measured; ++round) {
        const bool measured = round >= cfg.warmup;
        for (const CaseParams& params : cases) {
            const bench::CaseMeta meta = make_meta(params);
            if (!bench::case_selected(meta.case_id)) {
                continue;
            }
            failed += run_one_repetition(cfg, params, path,
                                         measured ? round - cfg.warmup : round, measured);
        }
    }

    std::printf("\n=== S10 complete, failing repetitions=%d ===\n", failed);
    return failed == 0 ? 0 : 1;
}
