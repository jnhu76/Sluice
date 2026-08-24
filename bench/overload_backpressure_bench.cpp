// overload_backpressure_bench — #199 (V6) sustained-overload backpressure
// measurement for bounded arena admission.
//
// The resource placed under sustained overload is the RequestArena ADMISSION
// CAPACITY (AGENTS.md §12 resource distinction: request_capacity is not
// worker count, not queue depth, not pipeline depth). FakeAsyncBackend keeps
// every accepted op outstanding until the driver explicitly completes it, so
// the window stays full deterministically — no worker noise, no disk noise,
// single thread. The bench measures, per capacity point:
//
//   - the admission-refusal path: latency of every would_block rejection
//     while the window is full (burst of M attempts per round, all on a
//     probe Completion that must stay idle for the whole run);
//   - the accepted path UNDER overload: latency of refill submits that land
//     in slots just reclaimed by completion+reset (K per round, each MUST be
//     accepted — this is the resource-bound distinction: the bound that
//     fired was admission capacity, and it is reclaimed after completions);
//   - bench-side in-flight high-water (must equal capacity exactly);
//   - RSS sampled every N rounds from /proc/self/status (a series, so a
//     growth trend is visible, not hidden behind a single number);
//   - recovery: post-sustained drain wall time and a post-drain admission
//     probe (one fresh submit must be accepted);
//   - static probes: sizeof of the production identity/public types.
//
// Output: one JSON object on stdout; the perf-attribution runner wraps it
// with the environment fingerprint + binary provenance (schema 2, kind
// "overload"). Raw per-attempt samples are emitted so the validator can
// recompute percentiles — a hand-typed table must not pass.
//
// Environment-sensitive Release evidence (WSL2 host limitation is recorded by
// the runner): NO absolute-speed claims, no universal thresholds (§16.7).
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/request_arena.hpp>
#include <sluice/async/fake_backend.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;
using std::chrono::steady_clock;

long long now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

// /proc/self/status VmRSS in kB (0 = unavailable; the series records the
// probe outcome honestly rather than dropping the sample).
long read_rss_kb() {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    long kb = 0;
    while (std::fgets(line, sizeof line, f)) {
        if (std::sscanf(line, "VmRSS: %ld kB", &kb) == 1) break;
    }
    std::fclose(f);
    return kb;
}

struct RssPoint {
    long round;
    long rss_kb;
};

bool refusal(const Result<void>& r) {
    return !r.has_value() && r.error().code == IoError::Code::would_block;
}

[[noreturn]] void fatal(const char* what) {
    std::fprintf(stderr, "overload_backpressure_bench: fatal: %s\n", what);
    std::exit(1);
}

void emit_samples(const std::vector<long long>& v) {
    std::printf("[");
    for (std::size_t i = 0; i < v.size(); ++i) {
        std::printf("%s%lld", i ? "," : "", v[i]);
    }
    std::printf("]");
}

// Nearest-rank percentile over a copy (matches the validator's method).
long long pct(std::vector<long long> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    std::size_t idx = static_cast<std::size_t>(p / 100.0 * static_cast<double>(v.size()));
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t capacity = 64, rounds = 400, burst = 32, complete_k = 8,
                rss_every = 50;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::size_t& out) {
            if (i + 1 >= argc) fatal("missing flag value");
            out = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        };
        if (a == "--capacity") next(capacity);
        else if (a == "--rounds") next(rounds);
        else if (a == "--burst") next(burst);
        else if (a == "--complete-k") next(complete_k);
        else if (a == "--rss-every") next(rss_every);
        else { std::fprintf(stderr, "unknown flag %s\n", a.c_str()); return 2; }
    }
    if (capacity < 1 || rounds < 1 || burst < 1 || complete_k < 1) {
        fatal("parameters must be >= 1");
    }
    if (complete_k > capacity) fatal("--complete-k must be <= --capacity");

    auto backend = std::make_unique<FakeAsyncBackend>(capacity);
    FakeAsyncBackend* fake = backend.get();
    AsyncIoContext ctx(std::move(backend));

    // capacity window Completions + one dedicated refusal probe. Non-movable
    // type: deque constructs every element in place.
    std::deque<Completion<std::size_t>> pool(capacity + 1);
    std::vector<Completion<std::size_t>*> window(capacity);
    for (std::size_t i = 0; i < capacity; ++i) window[i] = &pool[i];
    Completion<std::size_t>* probe = &pool[capacity];
    std::byte buf[16];

    std::vector<long long> accept_samples;   // refill submits under overload
    std::vector<long long> refuse_samples;   // burst rejections
    std::vector<long long> fill0_samples;    // first-round cold fill
    accept_samples.reserve(rounds * complete_k);
    refuse_samples.reserve(rounds * burst);

    std::size_t in_flight = 0, high_water = 0;
    std::size_t refill_accepts = 0, refusals = 0;
    std::deque<Completion<std::size_t>*> accepted_order;  // FIFO = fake's oldest order
    std::deque<Completion<std::size_t>*> freed;           // reset, awaiting reuse

    auto submit_one = [&](Completion<std::size_t>* c, std::vector<long long>* sample) {
        long long t0 = now_ns();
        Result<void> r = ctx.submit_read(ReadOp{-1, buf, sizeof buf, 0}, *c);
        long long dt = now_ns() - t0;
        if (sample) sample->push_back(dt);
        if (r.has_value()) {
            ++in_flight;
            high_water = high_water > in_flight ? high_water : in_flight;
            accepted_order.push_back(c);
            return true;
        }
        if (!refusal(r)) fatal("unexpected rejection code (not would_block)");
        return false;
    };

    // Cold fill: every window member accepted; probe refused at full window.
    for (auto* c : window) {
        if (!submit_one(c, &fill0_samples)) fatal("cold fill rejected below capacity");
    }
    {
        long long t0 = now_ns();
        Result<void> r = ctx.submit_read(ReadOp{-1, buf, sizeof buf, 0}, *probe);
        long long dt = now_ns() - t0;
        if (r.has_value()) fatal("probe accepted on a full window");
        ++refusals;
        refuse_samples.push_back(dt);
    }
    if (in_flight != capacity) fatal("fill did not reach capacity");

    std::vector<RssPoint> rss;
    rss.push_back({0, read_rss_kb()});

    // Sustained overload rounds: burst refusals -> complete K -> refill K.
    for (std::size_t r = 1; r <= rounds; ++r) {
        for (std::size_t b = 0; b < burst; ++b) {
            long long t0 = now_ns();
            Result<void> res = ctx.submit_read(ReadOp{-1, buf, sizeof buf, 0}, *probe);
            long long dt = now_ns() - t0;
            if (!refusal(res)) fatal("burst attempt accepted on a full window");
            ++refusals;
            refuse_samples.push_back(dt);
        }
        for (std::size_t k = 0; k < complete_k; ++k) {
            fake->complete_oldest_with_bytes(8);
            ctx.poll();
            Completion<std::size_t>* c = accepted_order.front();
            accepted_order.pop_front();
            if (!c->ready()) fatal("completion not ready after complete+poll");
            c->reset();  // caller lifecycle: releases the slot (generation++)
            --in_flight;
            freed.push_back(c);
        }
        for (std::size_t k = 0; k < complete_k; ++k) {
            Completion<std::size_t>* c = freed.front();
            freed.pop_front();
            if (!submit_one(c, &accept_samples)) {
                fatal("refill rejected after reclaim (capacity not reclaimed)");
            }
            ++refill_accepts;
        }
        if (in_flight != capacity) fatal("round ended below capacity");
        if (r % rss_every == 0) {
            rss.push_back({static_cast<long>(r), read_rss_kb()});
        }
    }
    if (rss.empty() || rss.back().round != static_cast<long>(rounds)) {
        rss.push_back({static_cast<long>(rounds), read_rss_kb()});
    }

    // Recovery: drain everything, measure wall time, prove admission returns.
    long long t_drain0 = now_ns();
    while (in_flight > 0) {
        fake->complete_oldest_with_bytes(8);
        ctx.poll();
        Completion<std::size_t>* c = accepted_order.front();
        accepted_order.pop_front();
        c->reset();
        --in_flight;
    }
    long long drain_ns = now_ns() - t_drain0;
    long long t_probe0 = now_ns();
    Result<void> post = ctx.submit_read(ReadOp{-1, buf, sizeof buf, 0}, *probe);
    long long probe_ns = now_ns() - t_probe0;
    if (!post.has_value()) fatal("post-drain admission probe rejected");
    fake->complete_oldest_with_bytes(8);
    ctx.poll();
    if (!probe->ready()) fatal("probe not ready after post-drain completion");
    probe->reset();

    for (auto* c : window) {
        if (!c->idle()) fatal("window completion not idle at exit");
    }
    if (!probe->idle()) fatal("probe not idle at exit");
    if (!freed.empty()) fatal("freed list not empty at exit");

    // ---- emit artifact JSON (stdout) ----
    std::printf("{\n");
    std::printf("  \"capacity\": %zu,\n", capacity);
    std::printf("  \"rounds\": %zu,\n", rounds);
    std::printf("  \"burst\": %zu,\n", burst);
    std::printf("  \"complete_k\": %zu,\n", complete_k);
    std::printf("  \"rss_every\": %zu,\n", rss_every);
    std::printf("  \"static\": {\n");
    std::printf("    \"sizeof_slot_handle\": %zu,\n", sizeof(detail::SlotHandle));
    std::printf("    \"sizeof_completion_size_t\": %zu,\n",
                sizeof(Completion<std::size_t>));
    std::printf("    \"sizeof_completion_void\": %zu,\n", sizeof(Completion<void>));
    std::printf("    \"sizeof_request_handle\": %zu,\n", sizeof(RequestHandle));
    std::printf("    \"sizeof_read_op\": %zu\n", sizeof(ReadOp));
    std::printf("  },\n");
    std::printf("  \"accounting\": {\n");
    std::printf("    \"refill_accepts\": %zu,\n", refill_accepts);
    std::printf("    \"refusals\": %zu,\n", refusals);
    std::printf("    \"expected_refusals\": %zu,\n", rounds * burst + 1);
    std::printf("    \"expected_refills\": %zu,\n", rounds * complete_k);
    std::printf("    \"high_water_inflight\": %zu,\n", high_water);
    std::printf("    \"final_inflight\": %zu,\n", in_flight);
    std::printf("    \"drain_ns\": %lld,\n", drain_ns);
    std::printf("    \"post_drain_probe_ns\": %lld,\n", probe_ns);
    std::printf("    \"post_drain_probe_accepted\": true\n");
    std::printf("  },\n");
    std::printf("  \"accept\": {\n");
    std::printf("    \"n\": %zu,\n", accept_samples.size());
    std::printf("    \"p50_ns\": %lld,\n", pct(accept_samples, 50));
    std::printf("    \"p95_ns\": %lld,\n", pct(accept_samples, 95));
    std::printf("    \"p99_ns\": %lld,\n", pct(accept_samples, 99));
    std::printf("    \"samples_ns\": ");
    emit_samples(accept_samples);
    std::printf("\n  },\n");
    std::printf("  \"refuse\": {\n");
    std::printf("    \"n\": %zu,\n", refuse_samples.size());
    std::printf("    \"p50_ns\": %lld,\n", pct(refuse_samples, 50));
    std::printf("    \"p95_ns\": %lld,\n", pct(refuse_samples, 95));
    std::printf("    \"p99_ns\": %lld,\n", pct(refuse_samples, 99));
    std::printf("    \"samples_ns\": ");
    emit_samples(refuse_samples);
    std::printf("\n  },\n");
    std::printf("  \"initial_fill\": {\n");
    std::printf("    \"n\": %zu,\n", fill0_samples.size());
    std::printf("    \"p50_ns\": %lld\n", pct(fill0_samples, 50));
    std::printf("  },\n");
    std::printf("  \"rss_series_kb\": [");
    for (std::size_t i = 0; i < rss.size(); ++i) {
        std::printf("%s[%ld,%ld]", i ? "," : "", rss[i].round, rss[i].rss_kb);
    }
    std::printf("]\n}\n");
    return 0;
}
