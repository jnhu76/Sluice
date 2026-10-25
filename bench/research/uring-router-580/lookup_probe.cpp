// #580 R2 trigger probe (WSL2 research-only). Counts router-lookup attempts,
// probes, hits/misses, installs/retires and CQE mix on the production-shaped
// RequestScope + UringAsyncBackend path against the real kernel, plus
// seam-derived miss/replay scenarios that are explicitly marked SEAM in their
// output. This binary is instrumented (SLUICE_URING_LOOKUP_INSTRUMENT) and its
// timings are context only, never performance evidence.

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/request_scope.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/detail/file_semantics.hpp>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctime>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Mirrors the conformance-test build: internal seam access + instrumentation.
#include "uring_internal.hpp"
#include "uring_test_seams.hpp"

using namespace sluice;
using sluice::async::AsyncIoContext;
using sluice::async::RequestScope;
using sluice::async::UringConfig;

namespace {

struct Config {
    std::string scenario = "pipeline";
    std::size_t capacity = 16;
    std::size_t depth = 16;
    std::size_t ops = 20000;
    std::size_t payload = 4096;
    double cancel_ratio = 0.0;
    std::size_t inject_misses = 0;
    std::string fixture_dir = "/tmp";
};

Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](void) -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
        };
        if (a == "--scenario") c.scenario = next();
        else if (a == "--capacity") c.capacity = std::stoull(next());
        else if (a == "--depth") c.depth = std::stoull(next());
        else if (a == "--ops") c.ops = std::stoull(next());
        else if (a == "--payload") c.payload = std::stoull(next());
        else if (a == "--cancel-ratio") c.cancel_ratio = std::stod(next());
        else if (a == "--inject-misses") c.inject_misses = std::stoull(next());
        else if (a == "--fixture-dir") c.fixture_dir = next();
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); std::exit(2); }
    }
    return c;
}

double now_s() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

std::string fixture_path(const Config& c) {
    return c.fixture_dir + "/sluice580_probe_fixture_" + std::to_string(getpid()) + ".bin";
}

Result<File> make_fixture(const Config& c) {
    const std::size_t bytes = std::max<std::size_t>(c.payload * 64, 1u << 20);
    FileOpen mode;
    mode.access = FileAccess::read_write;
    mode.existence = FileExistence::create_new;
    mode.contents = FileInitialContents::truncate;
    auto opened = File::open(fixture_path(c), mode);
    if (!opened.has_value())
        return opened;
    std::vector<std::byte> buffer(c.payload);
    for (std::size_t i = 0; i < c.payload; ++i)
        buffer[i] = static_cast<std::byte>((i * 131u + 7u) & 0xffu);
    std::size_t written = 0;
    while (written < bytes) {
        const std::size_t chunk = std::min(c.payload, bytes - written);
        auto res = blocking::write_all_at(opened.value(), written,
                                          std::span<const std::byte>(buffer.data(), chunk));
        if (!res.has_value() || !res.value().complete())
            return opened;
        written += chunk;
    }
    (void)blocking::sync_data(opened.value());
    return opened;
}

struct Timing {
    double wall = 0;
    double user = 0;
    double sys = 0;
};

Timing time_window(double t0) {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    Timing t;
    t.wall = (double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9) - t0;
    t.user = double(ru.ru_utime.tv_sec) + double(ru.ru_utime.tv_usec) * 1e-6;
    t.sys = double(ru.ru_stime.tv_sec) + double(ru.ru_stime.tv_usec) * 1e-6;
    return t;
}

bool bytes_match(const std::byte* actual, std::size_t len, std::uint64_t base) {
    for (std::size_t i = 0; i < len; ++i) {
        const auto expected = static_cast<std::byte>(((base + i) * 131u + 7u) & 0xffu);
        if (actual[i] != expected)
            return false;
    }
    return true;
}

struct ProbeOutcome {
    bool ok = true;
    std::string failure;
    std::size_t completed_ops = 0;
    std::size_t bytes_verified = 0;
    std::size_t cancels_requested = 0;
    std::size_t cancels_won_before_execution = 0;
    std::size_t cancels_canceled_terminal = 0;
};

// Steady pipeline: keep `depth` reads in flight through RequestScope until
// ops requests completed; verify every buffer against the fixture pattern.
ProbeOutcome run_pipeline(AsyncIoContext& ctx, RequestScope& scope,
                          const File& file, const Config& c) {
    ProbeOutcome out;
    struct Inflight {
        async::ScopeTicket<std::size_t> ticket;
        std::uint64_t offset = 0;
        std::byte* buffer = nullptr;
    };
    std::vector<std::vector<std::byte>> buffers(c.depth,
                                                std::vector<std::byte>(c.payload));
    std::vector<Inflight> window;
    window.reserve(c.depth);
    std::size_t submitted = 0;
    const std::size_t max_block = std::max<std::size_t>(c.payload * 32, 1u << 20);
    while (out.completed_ops < c.ops) {
        while (window.size() < c.depth && submitted < c.ops) {
            const std::size_t slot = submitted % c.depth;
            const std::uint64_t offset = (submitted * c.payload) % max_block;
            async::ReadOp op;
            op.file = async::NativeFileRef{file};
            op.dst = buffers[slot].data();
            op.offset = offset;
            op.len = c.payload;
            auto ticket = scope.submit_read(op);
            if (!ticket.has_value()) {
                out.ok = false;
                out.failure = "submit failed: " + std::to_string(int(ticket.error().code));
                return out;
            }
            window.push_back(Inflight{ticket.value(), offset, buffers[slot].data()});
            ++submitted;
        }
        if (window.empty()) {
            out.ok = false;
            out.failure = "pipeline stalled";
            return out;
        }
        auto& oldest = window.front();
        auto waited = scope.wait_for(oldest.ticket, std::chrono::milliseconds(5000));
        if (!waited.has_value() || waited.value() != async::ScopeWaitStatus::ready) {
            out.ok = false;
            out.failure = "wait not ready";
            return out;
        }
        auto observed = scope.take(oldest.ticket);
        if (observed.readiness != async::RequestReadiness::ready ||
            !observed.result.has_value() || observed.result.value() != c.payload ||
            !bytes_match(oldest.buffer, c.payload, oldest.offset)) {
            out.ok = false;
            out.failure = "read oracle mismatch";
            return out;
        }
        out.bytes_verified += c.payload;
        ++out.completed_ops;
        window.erase(window.begin());
    }
    return out;
}

// Churn: alternate the window between depth and 1 so router entries retire and
// reinstall repeatedly while capacity stays fixed.
ProbeOutcome run_churn(AsyncIoContext& ctx, RequestScope& scope,
                       const File& file, const Config& c) {
    Config inner = c;
    ProbeOutcome out;
    std::size_t remaining = c.ops;
    while (remaining > 0) {
        inner.ops = std::min(remaining, c.depth);
        const ProbeOutcome partial = run_pipeline(ctx, scope, file, inner);
        if (!partial.ok)
            return partial;
        out.completed_ops += partial.completed_ops;
        out.bytes_verified += partial.bytes_verified;
        remaining -= inner.ops;
        inner.depth = inner.depth == 1 ? c.depth : 1;
        inner.ops = std::min(remaining, inner.depth);
        if (remaining == 0)
            break;
        const ProbeOutcome shallow = run_pipeline(ctx, scope, file, inner);
        if (!shallow.ok)
            return shallow;
        out.completed_ops += shallow.completed_ops;
        out.bytes_verified += shallow.bytes_verified;
        remaining -= inner.ops;
        inner.depth = c.depth;
    }
    return out;
}

// Cancel mix: reads at depth; cancel a fraction of tickets right after submit
// through the public AsyncIoContext::cancel and record dispositions.
ProbeOutcome run_cancel_mix(AsyncIoContext& ctx, RequestScope& scope,
                            const File& file, const Config& c) {
    ProbeOutcome out;
    std::vector<std::vector<std::byte>> buffers(c.depth, std::vector<std::byte>(c.payload));
    struct Open {
        async::ScopeTicket<std::size_t> ticket;
        std::uint64_t offset = 0;
        std::byte* buffer = nullptr;
        bool canceled = false;
    };
    std::vector<Open> window;
    std::size_t submitted = 0;
    const std::size_t max_block = std::max<std::size_t>(c.payload * 32, 1u << 20);
    while (out.completed_ops < c.ops) {
        while (window.size() < c.depth && submitted < c.ops) {
            const std::size_t slot = submitted % c.depth;
            const std::uint64_t offset = (submitted * c.payload) % max_block;
            async::ReadOp op;
            op.file = async::NativeFileRef{file};
            op.dst = buffers[slot].data();
            op.offset = offset;
            op.len = c.payload;
            auto ticket = scope.submit_read(op);
            if (!ticket.has_value()) {
                out.ok = false;
                out.failure = "submit failed";
                return out;
            }
            Open entry{ticket.value(), offset, buffers[slot].data(), false};
            if (c.cancel_ratio > 0 && (submitted % 1000) < c.cancel_ratio * 1000) {
                const auto disposition = ctx.cancel(ticket.value().id());
                ++out.cancels_requested;
                if (disposition.has_value() &&
                    disposition.value() == async::CancelDisposition::won_before_execution) {
                    ++out.cancels_won_before_execution;
                    entry.canceled = true;
                }
            }
            window.push_back(entry);
            ++submitted;
        }
        auto& oldest = window.front();
        auto waited = scope.wait_for(oldest.ticket, std::chrono::milliseconds(5000));
        if (!waited.has_value() || waited.value() != async::ScopeWaitStatus::ready) {
            out.ok = false;
            out.failure = "wait not ready";
            return out;
        }
        auto observed = scope.take(oldest.ticket);
        if (observed.readiness != async::RequestReadiness::ready) {
            out.ok = false;
            out.failure = "take not ready";
            return out;
        }
        if (observed.result.has_value()) {
            if (observed.result.value() != c.payload ||
                !bytes_match(oldest.buffer, c.payload, oldest.offset)) {
                out.ok = false;
                out.failure = "read oracle mismatch";
                return out;
            }
            out.bytes_verified += c.payload;
        } else {
            ++out.cancels_canceled_terminal;
        }
        ++out.completed_ops;
        window.erase(window.begin());
    }
    return out;
}

// Metadata mix: file_info requests through the context (Request path).
ProbeOutcome run_metadata(AsyncIoContext& ctx, const File& file, const Config& c) {
    ProbeOutcome out;
    std::vector<async::Request<FileInfo>> window;
    std::size_t submitted = 0;
    while (out.completed_ops < c.ops) {
        while (window.size() < c.depth && submitted < c.ops) {
            async::FileInfoOp op;
            op.file = async::NativeFileRef{file};
            auto req = ctx.submit_file_info(op);
            if (!req.has_value()) {
                out.ok = false;
                out.failure = "metadata submit failed";
                return out;
            }
            window.push_back(std::move(req).value());
            ++submitted;
        }
        auto& front = window.front();
        while (!front.ready())
            (void)ctx.poll();
        auto observed = front.take_result();
        if (observed.readiness != async::RequestReadiness::ready ||
            !observed.result.has_value() || observed.result.value().size == 0) {
            out.ok = false;
            out.failure = "metadata oracle mismatch";
            return out;
        }
        ++out.completed_ops;
        window.erase(window.begin());
    }
    return out;
}

void emit(const Config& c, const std::string& scenario_tag, const ProbeOutcome& out,
          const Timing& t) {
    namespace r = sluice::async::research;
    const r::RouterCounters& rc = r::g_router_counters;
    auto site = [&](unsigned s) -> const r::LookupCounters& { return r::g_lookup_counters[s]; };
    auto pct = [&](const r::LookupCounters& l, double p) -> double {
        const std::uint64_t n = l.attempts.load(std::memory_order_relaxed);
        if (n == 0)
            return 0;
        const std::uint64_t target = std::uint64_t(double(n) * p);
        std::uint64_t seen = 0;
        for (std::size_t b = 1; b <= 65; ++b) {
            seen += l.probe_hist[b].load(std::memory_order_relaxed);
            if (b == 65) {
                std::uint64_t over = l.probe_sum.load(std::memory_order_relaxed);
                const std::uint64_t hits = l.hits.load(std::memory_order_relaxed);
                if (hits > 0)
                    over = l.probe_max.load(std::memory_order_relaxed);
                return double(over);
            }
            if (seen >= target && seen > 0)
                return double(b);
        }
        return 0;
    };
    auto ld = [&](std::uint64_t v) { return double(v); };
    std::printf(
        "{\"scenario\":\"%s\",\"tag\":\"%s\",\"ok\":%s,\"failure\":\"%s\","
        "\"capacity\":%zu,\"depth\":%zu,\"ops\":%zu,\"payload\":%zu,"
        "\"completed_ops\":%zu,\"bytes_verified\":%zu,"
        "\"cancels_requested\":%zu,\"cancels_won_pre\":%zu,\"cancels_terminal\":%zu,"
        "\"wall_s\":%.6f,\"user_s\":%.6f,\"sys_s\":%.6f,"
        "\"installs\":%llu,\"retires\":%llu,"
        "\"cq_operation\":%llu,\"cq_control\":%llu,\"cq_user0\":%llu,"
        "\"cancels_prepared\":%llu,\"cancel_intents_deferred\":%llu,",
        c.scenario.c_str(), scenario_tag.c_str(), out.ok ? "true" : "false",
        out.failure.c_str(), rc.router_capacity.load(std::memory_order_relaxed), c.depth,
        c.ops, c.payload, out.completed_ops, out.bytes_verified, out.cancels_requested,
        out.cancels_won_before_execution, out.cancels_canceled_terminal, t.wall, t.user,
        t.sys, (unsigned long long)rc.installs.load(std::memory_order_relaxed),
        (unsigned long long)rc.retires.load(std::memory_order_relaxed),
        (unsigned long long)rc.cq_operation.load(std::memory_order_relaxed),
        (unsigned long long)rc.cq_control.load(std::memory_order_relaxed),
        (unsigned long long)rc.cq_user_zero_dropped.load(std::memory_order_relaxed),
        (unsigned long long)rc.cancels_prepared.load(std::memory_order_relaxed),
        (unsigned long long)rc.cancel_intents_deferred.load(std::memory_order_relaxed));
    const char* names[5] = {"op_cqe", "control_cqe", "mark_consumed", "cancel_by_handle",
                            "poison_class_a"};
    std::printf("\"sites\":{");
    for (unsigned s = 0; s < 5; ++s) {
        const r::LookupCounters& l = site(s);
        const std::uint64_t n = l.attempts.load(std::memory_order_relaxed);
        std::printf(
            "%s\"%s\":{\"attempts\":%llu,\"hits\":%llu,\"misses\":%llu,"
            "\"avg_probes\":%.3f,\"p50_probes\":%.1f,\"p95_probes\":%.1f,"
            "\"p99_probes\":%.1f,\"max_probes\":%llu,\"avg_live_at_lookup\":%.3f}",
            s ? "," : "", names[s], (unsigned long long)n,
            (unsigned long long)l.hits.load(std::memory_order_relaxed),
            (unsigned long long)l.misses.load(std::memory_order_relaxed),
            n ? ld(l.probe_sum.load(std::memory_order_relaxed)) / double(n) : 0.0,
            pct(l, 0.50), pct(l, 0.95), pct(l, 0.99),
            (unsigned long long)l.probe_max.load(std::memory_order_relaxed),
            n ? ld(l.live_at_attempt_sum.load(std::memory_order_relaxed)) / double(n)
              : 0.0);
    }
    std::printf("}}\n");
}

} // namespace

int main(int argc, char** argv) {
    Config c = parse_args(argc, argv);

    auto fixture = make_fixture(c);
    if (!fixture.has_value()) {
        std::fprintf(stderr, "fixture creation failed\n");
        return 1;
    }
    File file = std::move(fixture).value();

    UringConfig config;
    config.request_capacity = c.capacity;
    config.queue_depth = static_cast<unsigned>(std::max<std::size_t>(c.capacity, 8));

    const double t0 = now_s();
    ProbeOutcome out;
    std::string tag = "real-kernel";
    {
        auto made = async::make_uring_context(config, nullptr);
        if (!made.has_value()) {
            std::fprintf(stderr, "uring context failed\n");
            return 1;
        }
        AsyncIoContext ctx = std::move(made).value();
        const std::size_t scope_cap = std::max(c.depth, std::size_t(1));
        if (c.scenario == "pipeline" || c.scenario == "churn" || c.scenario == "cancel") {
            RequestScope scope(ctx, scope_cap, async::ScopeCleanupPolicy::drain);
            if (c.scenario == "pipeline")
                out = run_pipeline(ctx, scope, file, c);
            else if (c.scenario == "churn")
                out = run_churn(ctx, scope, file, c);
            else
                out = run_cancel_mix(ctx, scope, file, c);
            auto fin = scope.finish();
            if (!fin.has_value()) {
                out.ok = false;
                out.failure = "scope finish failed";
            }
        } else if (c.scenario == "metadata") {
            out = run_metadata(ctx, file, c);
        } else if (c.scenario == "miss-inject") {
            // SEAM scenario: real submit, then replay retired cookies through
            // inject_cqe_for_test to produce deterministic stale-CQE misses.
            tag = "seam-inject";
            auto owned = std::make_unique<async::UringAsyncBackend>(
                config, async::UringBackendSubmitTestHooks{});
            auto* raw = owned.get();
            auto ctx2 = async::detail::make_context(std::move(owned));
            out.ok = true;
            std::size_t rounds = std::max<std::size_t>(c.ops / (c.depth ? c.depth : 1), 1);
            rounds = std::min(rounds, std::size_t(2000));
            std::vector<std::byte> buffer(c.payload);
            for (std::size_t r = 0; r < rounds && out.ok; ++r) {
                async::ReadOp op;
                op.file = async::NativeFileRef{file};
                op.dst = buffer.data();
                op.offset = (r * c.payload) % std::max<std::size_t>(c.payload * 32, 1u << 20);
                op.len = c.payload;
                auto req = ctx2.submit_read(op);
                if (!req.has_value()) {
                    out.ok = false;
                    out.failure = "submit failed";
                    break;
                }
                auto value = std::move(req).value();
                while (!value.ready())
                    (void)ctx2.poll();
                (void)value.take_result();
                ++out.completed_ops;
                out.bytes_verified += c.payload;
            }
            const std::uint64_t next = raw->peek_next_cookie_for_test();
            const std::size_t misses = c.inject_misses > 0 ? c.inject_misses : 1000;
            for (std::size_t i = 0; i < misses; ++i) {
                const std::uint64_t stale = (next <= i + 1) ? 1 : (next - 2 - (i % (next - 1)));
                raw->inject_cqe_for_test(stale, 1);
            }
        } else {
            std::fprintf(stderr, "unknown scenario %s\n", c.scenario.c_str());
            return 2;
        }
    }
    const Timing t = time_window(t0);
    emit(c, tag, out, t);
    (void)unlink(fixture_path(c).c_str());
    return out.ok ? 0 : 1;
}
