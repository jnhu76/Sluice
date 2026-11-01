// ELIDE-007 wait_one entry-shape driver (research branch only, never master).
// One accepted positional read in flight at a time; each operation enters
// wait_one exactly once and leaves through its completion, so the entry
// critical sections under study run once per operation. Three preregistered
// cells: {uring, threadpool} 4 KiB depth 16, plus the threadpool 64 KiB
// regression-watch cell. Links the same release archives the bench targets
// use.

#include "bench_common.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
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
#include <optional>
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
using sluice::async::Request;
using sluice::async::ShutdownPolicy;

constexpr std::size_t kFixtureBytes = 8u << 20;
constexpr std::size_t kOps = 20000;

std::string describe(const sluice::IoError& e) {
    return std::string(sluice::to_string(e.code)) + "(errno=" + std::to_string(e.os_errno) + ")";
}

struct Cell {
    std::string case_id;
    std::string backend;
    std::size_t block_size;
    std::size_t depth;
    std::size_t workers;
    bool available = true;
    std::string unavailable_reason;
};

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

struct CellResult {
    bool ok = true;
    bool not_run = false;
    std::string failure;
    double wall_s = 0.0;
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
    d.invalid_state_rejections = after.invalid_state_rejections - before.invalid_state_rejections;
    return d;
}

std::optional<AsyncIoContext> make_cell_context(const Cell& cell, AsyncStats* stats,
                                                std::string& failure) {
    try {
        if (cell.backend == "threadpool") {
            sluice::async::ThreadPoolConfig config;
            config.request_capacity = cell.depth;
            config.worker_count = cell.workers;
            return sluice::async::make_threadpool_context(config, stats);
        }
        sluice::async::UringConfig config;
        config.request_capacity = cell.depth;
        config.queue_depth = static_cast<unsigned>(cell.depth);
        auto made = sluice::async::make_uring_context(config, stats);
        if (!made.has_value()) {
            failure = "io_uring context rejected: " + describe(made.error());
            return std::nullopt;
        }
        return std::move(made).value();
    } catch (const std::exception& error) {
        failure = error.what();
        return std::nullopt;
    }
}

CellResult run_pass(const Cell& cell, const std::string& path, std::size_t ops,
                    bool instrument, bool verify, AsyncStats* stats) {
    CellResult result;

    auto opened = File::open(path, [] {
        FileOpen mode;
        mode.access = FileAccess::read_only;
        mode.existence = FileExistence::open_existing;
        return mode;
    }());
    if (!opened.has_value()) {
        result.ok = false;
        result.failure = "open failed: " + describe(opened.error());
        return result;
    }
    File file = std::move(opened).value();

    AsyncStats local_stats;
    AsyncStats& stats_ref = stats != nullptr ? *stats : local_stats;
    std::string backend_failure;
    auto context = make_cell_context(cell, &stats_ref, backend_failure);
    if (!context.has_value()) {
        result.ok = false;
        result.not_run = cell.backend == "uring";
        result.failure = "NOT_RUN_WITH_REASON: " + backend_failure;
        return result;
    }
    AsyncIoContext ctx = std::move(*context);

    const std::size_t blocks = kFixtureBytes / cell.block_size;
    std::vector<std::byte> buffer(cell.block_size, std::byte{0});

    const AsyncStats before = stats_ref;
    const bench::TimePoint start = bench::now();
    for (std::size_t op = 0; op < ops; ++op) {
        const std::uint64_t offset = static_cast<std::uint64_t>(op % blocks) * cell.block_size;
        const double submitted_ns = instrument ? bench::now_ns() : 0.0;
        auto ticket = ctx.submit_read(
            ReadOp{NativeFileRef{file}, buffer.data(), buffer.size(), offset});
        if (!ticket.has_value()) {
            result.ok = false;
            result.failure = "submit failed at operation " + std::to_string(op) + ": " +
                             describe(ticket.error());
            return result;
        }
        Request<std::size_t> held = std::move(ticket).value();
        const auto waited = ctx.wait_one();
        if (!waited.has_value() ||
            waited.value().kind != AsyncIoContext::ProgressWaitOutcome::Kind::progress ||
            waited.value().completed != 1) {
            result.ok = false;
            result.failure = "wait_one did not report one completion at operation " +
                             std::to_string(op);
            return result;
        }
        if (instrument) {
            result.latencies.push_back(bench::now_ns() - submitted_ns);
        }
        for (int i = 0; i < 200000 && held.valid() && !held.ready(); ++i)
            (void)ctx.poll();
        if (!held.valid() || !held.ready()) {
            result.ok = false;
            result.failure = "request binding not published after wait_one at operation " +
                             std::to_string(op);
            return result;
        }
        if (verify) {
            const auto mismatch = bench::verify_pattern(buffer.data(), cell.block_size, offset);
            if (!mismatch.ok) {
                result.ok = false;
                result.failure = "byte mismatch at absolute " +
                                 std::to_string(mismatch.first_bad_position) +
                                 " (operation " + std::to_string(op) + ")";
                return result;
            }
        }
        held.discard();
    }
    const bench::TimePoint end = bench::now();
    result.wall_s = bench::seconds_between(start, end);

    const auto outcome = ctx.shutdown(ShutdownPolicy::drain);
    if (!outcome.has_value()) {
        result.ok = false;
        result.failure = "shutdown failed";
        return result;
    }
    result.stats_delta = stats_diff(stats_ref, before);
    return result;
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

bench::CaseMeta make_meta(const Cell& cell, std::size_t ops) {
    bench::CaseMeta meta;
    meta.case_id = cell.case_id;
    meta.workload = "W7-wait-one-entry";
    meta.backend = cell.backend;
    meta.dataset = std::to_string(kFixtureBytes >> 20) + "MiB file, " +
                   std::to_string(cell.block_size) + "B positional reads, one in flight";
    meta.cache_policy = "page_cache_resident";
    meta.filesystem = "tmpfs";
    meta.notes = "sequential submit + one unbounded wait_one per operation";
    meta.block_size = cell.block_size;
    meta.request_count = ops;
    meta.concurrency = 1;
    meta.queue_depth = cell.depth;
    meta.worker_count = cell.workers;
    return meta;
}

int run_one_repetition(const bench::EnvConfig& cfg, const Cell& cell, const std::string& path,
                       std::size_t ops, std::size_t rep, bool measured) {
    const bench::CaseMeta meta = make_meta(cell, ops);
    bench::Record record = bench::make_record(meta, cfg, rep, measured, 0);

    if (!cell.available) {
        record.ok = false;
        record.failure = "NOT_RUN_WITH_REASON: " + cell.unavailable_reason;
        bench::emit_record(record);
        bench::print_record(record);
        return 0;
    }

    AsyncStats pass_stats;
    AsyncStats* stats = cfg.stats_enabled ? &pass_stats : nullptr;
    const double cpu_before = bench::process_cpu_seconds();
    const CellResult phase = run_pass(cell, path, ops, false, false, stats);
    const double cpu_after = bench::process_cpu_seconds();

    if (!phase.ok) {
        record.ok = false;
        record.failure = phase.failure;
        bench::emit_record(record);
        bench::print_record(record);
        return phase.not_run ? 0 : 1;
    }
    record.wall_s = phase.wall_s;
    record.ops = static_cast<double>(ops);
    record.ops_per_sec = phase.wall_s > 0.0 ? record.ops / phase.wall_s : 0.0;
    record.cpu_s = cpu_after - cpu_before;
    record.cpu_util = phase.wall_s > 0.0 ? record.cpu_s / phase.wall_s : 0.0;
    record.rss_kb = bench::resident_kb();
    record.peak_rss_kb = bench::peak_rss_kb();
    fill_stats(record, phase.stats_delta);

    if (cfg.stats_enabled) {
        if (record.stats_wait_calls != static_cast<std::uint64_t>(ops)) {
            record.ok = false;
            record.failure = "cadence guard: wait_calls/op != 1.0";
            bench::emit_record(record);
            bench::print_record(record);
            return 1;
        }
        if (record.stats_submit_calls != static_cast<std::uint64_t>(ops) ||
            record.stats_submitted_ops != static_cast<std::uint64_t>(ops) ||
            record.stats_completed_ops != static_cast<std::uint64_t>(ops)) {
            record.ok = false;
            record.failure = "stats wiring oracle: submit/completion counters do not match "
                             "the operations issued by this phase";
            bench::emit_record(record);
            bench::print_record(record);
            return 1;
        }
        if (record.stats_completion_errors != 0 || record.stats_short_completions != 0 ||
            record.stats_queue_full_retries != 0 || record.stats_invalid_state != 0) {
            record.ok = false;
            record.failure = "stats wiring oracle: rejected submissions or completion "
                             "failures on a clean sequential phase";
            bench::emit_record(record);
            bench::print_record(record);
            return 1;
        }
        if (record.stats_max_outstanding != 1) {
            record.ok = false;
            record.failure = "stats wiring oracle: max_outstanding outside the one-in-flight "
                             "window";
            bench::emit_record(record);
            bench::print_record(record);
            return 1;
        }
    }

    int failed = 0;
    const CellResult oracle = run_pass(cell, path, ops, false, true, stats);
    if (!oracle.ok) {
        record.ok = false;
        record.failure = "correctness oracle: " + oracle.failure;
        failed = 1;
    }

    const CellResult instrumented = run_pass(cell, path, ops, true, false, stats);
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
    std::printf("ELIDE-007 W7 wait_one entry-shape benchmark\n");
    std::printf("warmup=%zu measured_repetitions=%zu stats_wiring=%s\n", cfg.warmup,
                cfg.measured, cfg.stats_enabled ? "on" : "off");
    std::printf("========================================================\n");

    bench::ensure_dir(cfg.tmp_dir);
    const std::string path = cfg.tmp_dir + "/wait-one-007.bin";
    std::string failure;
    if (!write_fixture(path, failure)) {
        std::fprintf(stderr, "fixture creation failed: %s\n", failure.c_str());
        return 1;
    }

    std::vector<Cell> cells;
    cells.push_back({"waitone-threadpool-4k-d16-w4", "threadpool", 4096, 16, 4, true, {}});
    cells.push_back({"waitone-threadpool-64k-d16-w4", "threadpool", 65536, 16, 4, true, {}});
    {
        Cell uring{"waitone-uring-4k-d16", "uring", 4096, 16, 0, true, {}};
        std::string reason;
        AsyncStats probe_stats;
        auto probe = make_cell_context(uring, &probe_stats, reason);
        if (!probe.has_value()) {
            uring.available = false;
            uring.unavailable_reason = "io_uring backend not available in this build profile";
            std::fprintf(stderr, "io_uring backend unavailable: %s\n", reason.c_str());
        }
        cells.push_back(std::move(uring));
    }

    int failed = bench::oracle_self_test_case(cfg);
    for (std::size_t round = 0; round < cfg.warmup + cfg.measured; ++round) {
        const bool measured = round >= cfg.warmup;
        for (const Cell& cell : cells) {
            const bench::CaseMeta meta = make_meta(cell, kOps);
            if (!bench::case_selected(meta.case_id)) {
                continue;
            }
            failed += run_one_repetition(cfg, cell, path, kOps,
                                         measured ? round - cfg.warmup : round, measured);
        }
    }

    std::printf("\n=== W7 complete, failing repetitions=%d ===\n", failed);
    return failed == 0 ? 0 : 1;
}
