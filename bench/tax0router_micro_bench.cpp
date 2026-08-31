// TAX-0 router-fix candidate shootout (#255) — Layer A: router LIFECYCLE
// microbench. Synthetic, deterministic, no kernel/device I/O, no per-op
// allocation: every candidate consumes the EXACT same logical trace
// (cookie sequence, active depth D, capacity C, completion order, insert /
// lookup / erase counts) generated purely from (pattern, D, C, cycles) —
// never from candidate behavior.
//
// Measured path: the REAL router lifecycle surface of the internal-testing
// build —
//   install  router_install_cookie_for_test()   (free-list pop + no-wrap
//             cookie + RouterEntry install + R3 table insert)
//   lookup   find_live_router_cookie_for_test() (the EXACT production
//             find_live_router_cookie_ under the selected candidate)
//   retire   router_retire_cookie_for_test()    (the EXACT production
//             retire_router_entry_ incl. R3 table erase)
// so R3 pays insert+lookup+erase and R0/R1/R2 pay placement/free-list +
// scan work — never find() alone.
//
// Patterns (frozen):
//   P0 steady LIFO/reuse   — completion = reverse install order (normal
//                            steady state; mirrors EXP-0)
//   P1 deterministic permutation — per-window splitmix64 shuffle, seed
//                            derived from the campaign seed + window index
//   P2 high occupancy      — D == C (whole router live), LIFO completion
//
// Output: one JSON object on stdout. Exit 0 only when the accounting gates
// hold (equal insert/lookup/erase counts, exact hit/miss shape, active
// depth maintained). Driven by scripts/bench/perf-attribution.py
// `tax0routermicro` (kind tax0routermicro).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sluice/async/uring_backend.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)

namespace {

// Campaign seed for pattern derivation ("RTRS"; frozen in the task spec).
constexpr std::uint64_t kCampaignSeed = 0x52545253ull;

[[noreturn]] void bench_fatal(const char* what) {
    std::fprintf(stderr, "tax0router_micro_bench: fatal: %s\n", what);
    std::exit(3);
}

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Deterministic in-place Fisher-Yates from a seeded splitmix64 stream:
// the completion permutation is a PURE function of (seed, window index, D)
// — identical for every candidate.
std::vector<std::uint32_t> window_permutation(std::uint64_t seed,
                                              std::uint64_t window,
                                              std::uint32_t d) {
    std::vector<std::uint32_t> order(d);
    for (std::uint32_t i = 0; i < d; ++i)
        order[i] = i;
    std::uint64_t state = splitmix64(seed ^ (window * 0x9E3779B97F4A7C15ull));
    for (std::uint32_t i = d; i > 1; --i) {
        state = splitmix64(state);
        const std::uint32_t j = static_cast<std::uint32_t>(state % i);
        std::swap(order[i - 1], order[j]);
    }
    return order;
}

enum class Pattern { P0_lifo, P1_permutation, P2_full };
enum class Candidate { r0, r1, r2, r3 };

struct Config {
    Candidate candidate = Candidate::r0;
    Pattern pattern = Pattern::P0_lifo;
    std::size_t depth = 8;
    std::size_t capacity = 8;
    std::size_t windows = 20000; // one window = D installs + D lookups + D retires
    std::uint64_t seed = kCampaignSeed;
};

bool config_valid(const Config& c, std::string& err) {
    if (c.depth == 0 || c.depth > 4096) {
        err = "--depth must be in [1, 4096]";
        return false;
    }
    if (c.capacity < c.depth || c.capacity > 65536) {
        err = "--capacity must be in [depth, 65536]";
        return false;
    }
    if (c.pattern == Pattern::P2_full && c.capacity != c.depth) {
        err = "P2 (high occupancy) requires capacity == depth";
        return false;
    }
    if (c.windows == 0 || c.windows > 100000000ull) {
        err = "--windows must be in [1, 1e8]";
        return false;
    }
    return true;
}

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

int main(int argc, char** argv) {
    using namespace sluice::async;
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](const char* opt) -> const char* {
            if (i + 1 >= argc)
                bench_fatal((std::string(opt) + " requires a value").c_str());
            return argv[++i];
        };
        auto parse_size = [](const char* s, std::size_t& out) {
            std::size_t v = 0;
            for (const char* p = s; *p; ++p) {
                if (*p < '0' || *p > '9')
                    return false;
                v = v * 10 + static_cast<std::size_t>(*p - '0');
            }
            out = v;
            return true;
        };
        if (a == "--candidate") {
            std::string s(next("--candidate"));
            if (s == "r0") cfg.candidate = Candidate::r0;
            else if (s == "r1") cfg.candidate = Candidate::r1;
            else if (s == "r2") cfg.candidate = Candidate::r2;
            else if (s == "r3") cfg.candidate = Candidate::r3;
            else bench_fatal("--candidate must be r0|r1|r2|r3");
        } else if (a == "--pattern") {
            std::string s(next("--pattern"));
            if (s == "P0") cfg.pattern = Pattern::P0_lifo;
            else if (s == "P1") cfg.pattern = Pattern::P1_permutation;
            else if (s == "P2") cfg.pattern = Pattern::P2_full;
            else bench_fatal("--pattern must be P0|P1|P2");
        } else if (a == "--depth") {
            if (!parse_size(next("--depth"), cfg.depth))
                bench_fatal("--depth: invalid number");
        } else if (a == "--capacity") {
            if (!parse_size(next("--capacity"), cfg.capacity))
                bench_fatal("--capacity: invalid number");
        } else if (a == "--windows") {
            std::uint64_t v = 0;
            const char* s = next("--windows");
            for (const char* p = s; *p; ++p) {
                if (*p < '0' || *p > '9') bench_fatal("--windows: invalid");
                v = v * 10 + static_cast<std::uint64_t>(*p - '0');
            }
            cfg.windows = v;
        } else if (a == "--seed") {
            std::uint64_t v = 0;
            const char* s = next("--seed");
            for (const char* p = s; (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'); ++p)
                v = v * 16 + static_cast<std::uint64_t>(
                                 (*p <= '9') ? (*p - '0') : (*p - 'a' + 10));
            cfg.seed = v;
        } else {
            bench_fatal("unknown argument");
        }
    }
    std::string cfg_err;
    if (!config_valid(cfg, cfg_err))
        bench_fatal(cfg_err.c_str());

    const std::size_t D = cfg.depth;
    const std::size_t C = cfg.capacity;

    // A REAL backend provides the router surface (a real ring is required
    // by construction; the micro-op path performs NO I/O). The ring exists
    // only to satisfy the backend's construction contract.
    UringAsyncBackend backend{UringConfig{C, 8}};
    if (!backend.available())
        bench_fatal("uring backend did not initialize a real ring");
    const auto mode_of = [](Candidate c) {
        switch (c) {
        case Candidate::r0: return UringAsyncBackend::RouterFixModeForTest::production_baseline;
        case Candidate::r1: return UringAsyncBackend::RouterFixModeForTest::reverse_scan;
        case Candidate::r2: return UringAsyncBackend::RouterFixModeForTest::low_placement_forward;
        case Candidate::r3: return UringAsyncBackend::RouterFixModeForTest::bounded_cookie_table;
        }
        bench_fatal("unreachable candidate");
    };
    backend.set_router_fix_mode_for_test(mode_of(cfg.candidate));

    // Live-window bookkeeping (bench-side trace state; NOT the measured
    // path): cookie values and router indices of the live window.
    std::vector<std::uint64_t> live_cookie(D);
    std::vector<std::size_t> live_index(D);
    std::size_t live_count = 0;
    std::uint64_t next_cookie_value = 1; // no-wrap counter mirror (assert-only)

    std::uint64_t install_ns = 0, lookup_ns = 0, retire_ns = 0;
    std::uint64_t inserts = 0, lookups = 0, erases = 0;
    // Structural witness accumulated bench-side (the kind-folded production
    // counters only update at production callsites; the seam lookup path
    // updates last_call_iterations, which this accumulates per op).
    std::uint64_t lookup_iterations = 0;

    backend.reset_router_scan_diagnostics_for_test();
    const std::uint64_t t_all0 = now_ns();

    for (std::uint64_t w = 0; w < cfg.windows; ++w) {
        // Completion order for this window: PURE function of (pattern,
        // seed, window, D) — identical across candidates. The order is
        // computed over WINDOW POSITIONS before any candidate runs (it is
        // the same array for every candidate by construction).
        std::vector<std::uint32_t> order;
        if (cfg.pattern == Pattern::P1_permutation)
            order = window_permutation(cfg.seed, w, static_cast<std::uint32_t>(D));
        else
            order.resize(D), std::iota(order.begin(), order.end(), 0u);
        // P0/P2: LIFO — reverse install order.
        if (cfg.pattern != Pattern::P1_permutation)
            std::reverse(order.begin(), order.end());

        // Phase A: install up to depth D (steady state: the window starts
        // empty because the previous window retired everything).
        const std::uint64_t t0 = now_ns();
        while (live_count < D) {
            const std::size_t idx = backend.router_install_cookie_for_test();
            live_cookie[live_count] = next_cookie_value;
            live_index[live_count] = idx;
            ++next_cookie_value;
            ++live_count;
            ++inserts;
        }
        const std::uint64_t t1 = now_ns();
        install_ns += t1 - t0;

        // Phase B: look up every live cookie in completion order (hit).
        const std::uint64_t t2 = now_ns();
        for (std::uint32_t k = 0; k < D; ++k) {
            const std::size_t pos = order[k];
            const std::size_t found =
                backend.find_live_router_cookie_for_test(live_cookie[pos]);
            if (found != live_index[pos])
                bench_fatal("lookup resolved a wrong router entry");
            lookup_iterations +=
                backend.router_scan_diagnostics_for_test().last_call_iterations;
            ++lookups;
        }
        const std::uint64_t t3 = now_ns();
        lookup_ns += t3 - t2;

        // Phase C: retire in the SAME completion order.
        // Retire-in-order over the window: the trace retires exactly the
        // entries the completion order names, using bookkeeping positions.
        const std::uint64_t t4 = now_ns();
        for (std::uint32_t k = 0; k < D; ++k) {
            const std::uint32_t pos = order[k];
            backend.router_retire_cookie_for_test(live_index[pos]);
            // Mark retired (bench bookkeeping only).
            live_index[pos] = static_cast<std::size_t>(-1);
            --live_count;
            ++erases;
        }
        const std::uint64_t t5 = now_ns();
        retire_ns += t5 - t4;
    }
    const std::uint64_t t_all1 = now_ns();

    // Fail-closed accounting gates.
    if (inserts != lookups || lookups != erases)
        bench_fatal("insert/lookup/erase count mismatch");
    if (backend.live_cookies_for_test() != 0 || backend.outstanding() != 0)
        bench_fatal("router not quiescent after the trace");
    const auto& diag = backend.router_scan_diagnostics_for_test();
    if (diag.lookup_calls != lookups || diag.lookup_hits != lookups ||
        diag.lookup_misses != 0)
        bench_fatal("unexpected lookup accounting shape");
    if (diag.matched_router_index_max >= C)
        bench_fatal("matched index out of range");
    if (cfg.candidate == Candidate::r3) {
        if (diag.table_insert_calls != inserts || diag.table_erase_calls != erases)
            bench_fatal("R3 table insert/erase count mismatch");
        if (diag.table_insert_probes_total == 0 || diag.table_erase_probes_total == 0)
            bench_fatal("R3 probe accounting absent");
    } else if (diag.table_insert_calls != 0 || diag.table_erase_calls != 0)
        bench_fatal("non-R3 candidate must not touch the table");

    // Structural memory facts (fixed per configured capacity; seam-reported).
    const std::uint64_t router_bytes =
        static_cast<std::uint64_t>(C) *
        static_cast<std::uint64_t>(
            UringAsyncBackend::router_entry_bytes_for_test());
    const std::uint64_t table_bytes =
        static_cast<std::uint64_t>(backend.router_table_bytes_for_test());
    const std::uint64_t freelist_bytes =
        static_cast<std::uint64_t>(C * sizeof(std::uint32_t));

    const char* cand = cfg.candidate == Candidate::r0   ? "r0"
                       : cfg.candidate == Candidate::r1 ? "r1"
                       : cfg.candidate == Candidate::r2 ? "r2"
                                                        : "r3";
    const char* pat = cfg.pattern == Pattern::P0_lifo       ? "P0"
                      : cfg.pattern == Pattern::P1_permutation ? "P1"
                                                               : "P2";

    std::string out;
    out += "{\n";
    out += "  \"bench\": \"tax0router_micro_bench\",\n";
    out += "  \"bench_version\": 1,\n";
    out += "  \"experiment\": \"TAX-0-ROUTER-SHOOTOUT-A\",\n";
    out += std::string("  \"candidate\": \"") + cand + "\",\n";
    out += std::string("  \"pattern\": \"") + pat + "\",\n";
    out += "  \"depth\": " + std::to_string(D) + ",\n";
    out += "  \"capacity\": " + std::to_string(C) + ",\n";
    out += "  \"seed\": " + std::to_string(cfg.seed) + ",\n";
    out += "  \"windows\": " + std::to_string(cfg.windows) + ",\n";
    out += "  \"lifecycle_ops\": " + std::to_string(inserts) + ",\n";
    out += "  \"install_ops\": " + std::to_string(inserts) + ",\n";
    out += "  \"lookup_ops\": " + std::to_string(lookups) + ",\n";
    out += "  \"retire_ops\": " + std::to_string(erases) + ",\n";
    out += "  \"install_ns_total\": " + std::to_string(install_ns) + ",\n";
    out += "  \"lookup_ns_total\": " + std::to_string(lookup_ns) + ",\n";
    out += "  \"retire_ns_total\": " + std::to_string(retire_ns) + ",\n";
    out += "  \"wall_ns_total\": " + std::to_string(t_all1 - t_all0) + ",\n";
    out += "  \"lookup_iterations_total\": " +
           std::to_string(lookup_iterations) + ",\n";
    out += "  \"lookup_iterations_max\": " +
           std::to_string(diag.operation_lookup_iterations_max) + ",\n";
    out += "  \"table_insert_probes_total\": " +
           std::to_string(diag.table_insert_probes_total) + ",\n";
    out += "  \"table_lookup_probes_total\": " +
           std::to_string(diag.table_lookup_probes_total) + ",\n";
    out += "  \"table_erase_probes_total\": " +
           std::to_string(diag.table_erase_probes_total) + ",\n";
    out += "  \"matched_router_index_max\": " +
           std::to_string(diag.matched_router_index_max) + ",\n";
    out += "  \"router_bytes\": " + std::to_string(router_bytes) + ",\n";
    out += "  \"candidate_table_bytes\": " + std::to_string(table_bytes) + ",\n";
    out += "  \"freelist_bytes\": " + std::to_string(freelist_bytes) + ",\n";
    out += "  \"steady_allocations_per_op\": 0,\n";
    out += "  \"accounting_ok\": true\n";
    out += "}\n";
    std::fputs(out.c_str(), stdout);
    return 0;
}

#else

int main() {
    std::fprintf(stderr,
                 "tax0router_micro_bench: requires SLUICE_HAS_LIBURING + "
                 "SLUICE_ASYNC_INTERNAL_TESTING (links sluice_async_internal_"
                 "testing; xmake f --with-liburing=true)\n");
    return 3;
}

#endif
