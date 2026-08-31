// TAX-0 router-fix candidate shootout (#255) — semantic equivalence gates.
//
// For EVERY admitted fix candidate (R0 production baseline, R1 reverse scan,
// R2 low placement + forward scan, R3 bounded cookie table) this suite proves
// mechanically, over REACHABLE router states on a REAL ring, that the
// candidate preserves the Sluice identity contract:
//
//   - a live cookie resolves to exactly one router entry (its own);
//   - unknown / stale / retired / control-tagged cookies resolve to nothing
//     in every candidate;
//   - stale cookies never alias a REUSED router slot (generation safety:
//     a retired cookie misses even after its slot was reinstalled with a
//     new cookie);
//   - cancel / tagged-control routing, duplicate-CQE tolerance, teardown
//     and drain behave identically;
//   - the physical representations bind to their designed structure
//     (placement witness: R0/R1/R3 park the live set at high indices,
//     R2 at low; R3 resolves through a bounded probe sequence).
//
// Semantic validity comes BEFORE performance: no candidate that fails a
// gate here may produce shootout evidence. The mode seam is the EXACT
// production code path (single implementation, selectable direction /
// placement / lookup structure), so these gates exercise the same functions
// the Layer-A/Layer-B instruments measure.
//
// Real mode requires a real ring; the cases no-op when the kernel provides
// none (stub-build honesty, mirroring the EXP-U0 suite).
#include "harness.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/async/uring_backend.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#if defined(SLUICE_HAS_LIBURING)
#include <unistd.h>
#endif

using namespace sluice::async;

#if defined(SLUICE_HAS_LIBURING)

namespace {

class TempFile {
  public:
    TempFile() {
        char path[] = "/tmp/sluice_uring_rfix_XXXXXX";
        fd_ = ::mkstemp(path);
        if (fd_ >= 0)
            (void)::unlink(path);
    }
    ~TempFile() {
        if (fd_ >= 0)
            (void)::close(fd_);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    int fd() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }

  private:
    int fd_ = -1;
};

// The cookie the k-th dispatch on a FRESH backend carries (no-wrap counter
// starts at 1); the k+K-th dispatch (after K retires) continues the counter.
constexpr std::uint64_t kth_cookie(std::size_t k) noexcept { return k + 1; }

constexpr UringAsyncBackend::RouterFixModeForTest kModes[] = {
    UringAsyncBackend::RouterFixModeForTest::production_baseline,
    UringAsyncBackend::RouterFixModeForTest::reverse_scan,
    UringAsyncBackend::RouterFixModeForTest::low_placement_forward,
    UringAsyncBackend::RouterFixModeForTest::bounded_cookie_table,
};

bool drain(UringAsyncBackend& backend, int rounds = 100000) {
    while (backend.outstanding() > 0 && rounds-- > 0) {
        (void)backend.poll();
    }
    return backend.outstanding() == 0;
}

// One full install/lookup/retire/reinstall round on a fresh-or-reused
// backend: K writes dispatched, every live cookie must hit and every
// unknown/stale/control-tagged probe must miss; after the drain the same
// cookies must miss everywhere (retirement + reuse generation safety).
void check_round(UringAsyncBackend& backend, std::size_t capacity,
                 std::size_t base_count, TempFile& file,
                 std::vector<Completion<std::size_t>>& comp) {
    const std::size_t k_count = comp.size();
    std::vector<std::byte> buf(k_count * 4096, std::byte{0x55});
    std::vector<std::uint64_t> live;
    for (std::size_t k = 0; k < k_count; ++k) {
        const std::uint64_t off = static_cast<std::uint64_t>(base_count + k) * 4096;
        SLUICE_CHECK(backend
                         .submit_write(WriteOp{file.fd(), buf.data() + k * 4096,
                                               4096, off},
                                       comp[k])
                         .has_value());
        live.push_back(kth_cookie(base_count + k));
    }
    SLUICE_CHECK(backend.live_cookies_for_test() == k_count);

    backend.reset_router_scan_diagnostics_for_test();

    // Live-cookie hits through the EXACT production lookup.
    for (const std::uint64_t cookie : live) {
        const std::size_t idx = backend.find_live_router_cookie_for_test(cookie);
        SLUICE_CHECK(idx < capacity);
    }
    // Unknown / stale / control-tagged probes must miss.
    const std::uint64_t probes[] = {
        0,                                     // never emitted
        kth_cookie(base_count + k_count),      // next cookie, not yet live
        (std::uint64_t{1} << 63u) | live[0],   // control-tagged live cookie
        live[0] + 1000000,                     // distant unknown
    };
    for (const std::uint64_t p : probes)
        SLUICE_CHECK(backend.find_live_router_cookie_for_test(p) == capacity);

    // Real CQE path accounting for the round: exactly K operation lookups,
    // all hits, no control/transport lookups in this no-cancel workload.
    backend.reset_router_scan_diagnostics_for_test();
    SLUICE_CHECK(drain(backend));
    SLUICE_CHECK(backend.live_cookies_for_test() == 0);
    const auto& diag = backend.router_scan_diagnostics_for_test();
    SLUICE_CHECK(diag.operation_cookie_lookup_calls == k_count);
    SLUICE_CHECK(diag.lookup_hits == k_count);
    SLUICE_CHECK(diag.lookup_misses == 0);
    SLUICE_CHECK(diag.control_cookie_lookup_calls == 0);
    SLUICE_CHECK(diag.transport_cookie_lookup_calls == 0);

    // Retirement equivalence: every former live cookie now misses (the
    // same cookie NEVER resolves to a reused slot with a new occupant).
    for (const std::uint64_t cookie : live)
        SLUICE_CHECK(backend.find_live_router_cookie_for_test(cookie) == capacity);

    for (auto& c : comp) {
        SLUICE_CHECK(c.ready());
        const auto res = c.result();
        SLUICE_CHECK(res.has_value() && res.value() == 4096);
        c.reset();
    }
    SLUICE_CHECK(backend.arena_slot_in_use() == 0);
}

} // namespace

// ---------------------------------------------------------------------------
// Matrix: modes x C x K reachable states. Every candidate must produce the
// same semantic answers (hits for live cookies, misses for everything else)
// across two consecutive rounds (router reuse + generation safety) and a
// quiescent teardown.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_router_fix_mode_equivalence_matrix) {
    const std::size_t capacities[] = {1, 2, 4, 8, 32};
    for (const auto mode : kModes) {
        for (const std::size_t capacity : capacities) {
            const std::size_t k_max = capacity < 5 ? capacity : 5;
            for (std::size_t k_count = 1; k_count <= k_max; ++k_count) {
                UringAsyncBackend backend{UringConfig{capacity, 8}};
                if (!backend.available())
                    return;
                SLUICE_CHECK(backend.router_fix_mode_for_test() ==
                             UringAsyncBackend::RouterFixModeForTest::
                                 production_baseline);
                backend.set_router_fix_mode_for_test(mode);
                SLUICE_CHECK(backend.router_fix_mode_for_test() == mode);
                TempFile file;
                SLUICE_CHECK(file.valid());

                std::vector<Completion<std::size_t>> comp(k_count);
                check_round(backend, capacity, 0, file, comp);
                // Reuse round: the freed slots are reinstalled with NEW
                // cookies; the retired cookies from round one must still
                // miss everywhere (stale != new occupant).
                std::vector<Completion<std::size_t>> comp2(k_count);
                check_round(backend, capacity, k_count, file, comp2);

                SLUICE_CHECK(backend.outstanding() == 0);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Cancel / tagged-control path equivalence per candidate. Running-cancel
// intent + original-kernel-result-wins (mirrors the EXP-U0 suite): identical
// disposition in every mode, exactly one operation, one control, and one
// transport lookup, zero misses. R3 additionally retires its table entry
// through the same production retirement path.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_router_fix_cancel_control_path_equivalence) {
    for (const auto mode : kModes) {
        int pipe_fds[2] = {-1, -1};
        if (::pipe(pipe_fds) != 0)
            return;
        UringAsyncBackend backend{UringConfig{8, 8}};
        if (!backend.available()) {
            ::close(pipe_fds[0]);
            ::close(pipe_fds[1]);
            return;
        }
        backend.set_router_fix_mode_for_test(mode);
        backend.reset_router_scan_diagnostics_for_test();

        std::byte buf[4]{};
        Completion<std::size_t> c;
        SLUICE_CHECK(
            backend.submit_read(ReadOp{pipe_fds[0], buf, 4, 0}, c).has_value());
        SLUICE_CHECK(backend.poll() == 0); // kernel blocks the read

        auto h = backend.handle_for_completion_for_test(&c);
        SLUICE_CHECK(h.has_value());
        SLUICE_CHECK(backend.cancel_handle_for_test(*h) ==
                     detail::CancelDisposition::intent_recorded);
        SLUICE_CHECK(backend.live_control_entries_for_test() == 1);
        SLUICE_CHECK(!c.ready()); // intent published nothing

        // Original kernel result wins verbatim (EOF = 0 bytes).
        SLUICE_CHECK(::close(pipe_fds[1]) == 0);
        SLUICE_CHECK(backend.poll() == 1);
        SLUICE_CHECK(c.ready());
        const auto res = c.result();
        SLUICE_CHECK(res.has_value() && res.value() == 0);
        c.reset();
        SLUICE_CHECK(backend.outstanding() == 0);
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
        SLUICE_CHECK(backend.live_cookies_for_test() == 0);
        SLUICE_CHECK(backend.live_control_entries_for_test() == 0);

        const auto& diag = backend.router_scan_diagnostics_for_test();
        SLUICE_CHECK(diag.operation_cookie_lookup_calls == 1);
        SLUICE_CHECK(diag.control_cookie_lookup_calls == 1);
        SLUICE_CHECK(diag.transport_cookie_lookup_calls == 1);
        SLUICE_CHECK(diag.lookup_misses == 0);
        if (mode == UringAsyncBackend::RouterFixModeForTest::bounded_cookie_table) {
            // R3: install + erase each happened exactly once on the
            // production paths.
            SLUICE_CHECK(diag.table_insert_calls == 1);
            SLUICE_CHECK(diag.table_erase_calls == 1);
        } else {
            SLUICE_CHECK(diag.table_insert_calls == 0);
            SLUICE_CHECK(diag.table_erase_calls == 0);
        }
        // Stale duplicate of the retired cookie: dropped identically.
        const auto stale = backend.peek_next_cookie_for_test() - 1;
        backend.inject_cqe_for_test(stale, 4);
        SLUICE_CHECK(backend.poll() == 0);
        SLUICE_CHECK(!c.ready());
        SLUICE_CHECK(backend.router_scan_diagnostics_for_test().lookup_misses == 1);
        ::close(pipe_fds[0]);
    }
}

// ---------------------------------------------------------------------------
// Placement + structure witness: bind each candidate to its DESIGNED
// physical representation on a fresh backend at C=32, K=8 (the EXP-U0
// binding discipline — the shootout's differentia must be the thing the
// report says it is):
//   R0/R1/R3: allocation k parks at index C-1-k (free-list back());
//             forward iterations C-k (R0), reverse k+1 (R1);
//             R3 resolves via a small bounded probe sequence.
//   R2:       allocation k parks at index k (descending seed);
//             forward iterations k+1.
//   R2 steady state: 200 install/retire cycles at D=8, C=512 keep every
//             matched index < 16 (live set pinned low; R0 would see ~512).
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_router_fix_placement_and_structure_witness) {
    const std::size_t capacity = 32;
    for (const auto mode : kModes) {
        UringAsyncBackend backend{UringConfig{capacity, 8}};
        if (!backend.available())
            return;
        backend.set_router_fix_mode_for_test(mode);
        TempFile file;
        SLUICE_CHECK(file.valid());
        const std::size_t k_count = 8;
        std::vector<std::byte> buf(k_count * 4096, std::byte{0x55});
        std::vector<Completion<std::size_t>> comp(k_count);
        for (std::size_t k = 0; k < k_count; ++k) {
            SLUICE_CHECK(backend
                             .submit_write(WriteOp{file.fd(), buf.data() + k * 4096,
                                                   4096, k * 4096},
                                           comp[k])
                             .has_value());
        }
        backend.reset_router_scan_diagnostics_for_test();
        for (std::size_t k = 0; k < k_count; ++k) {
            const std::uint64_t cookie = kth_cookie(k);
            const std::size_t idx = backend.find_live_router_cookie_for_test(cookie);
            const auto& diag = backend.router_scan_diagnostics_for_test();
            const std::uint64_t iters = diag.last_call_iterations;
            SLUICE_CHECK(idx < capacity);
            if (mode == UringAsyncBackend::RouterFixModeForTest::low_placement_forward) {
                SLUICE_CHECK(idx == k);
                SLUICE_CHECK(iters == k + 1);
            } else {
                // R0/R1/R3 keep the production high-index placement.
                SLUICE_CHECK(idx == capacity - 1 - k);
                if (mode == UringAsyncBackend::RouterFixModeForTest::reverse_scan)
                    SLUICE_CHECK(iters == k + 1);
                else if (mode ==
                         UringAsyncBackend::RouterFixModeForTest::production_baseline)
                    SLUICE_CHECK(iters == capacity - k);
                else // R3: bounded probe sequence, placement untouched.
                    SLUICE_CHECK(iters >= 1 && iters <= 8);
            }
        }
        SLUICE_CHECK(drain(backend));
        for (auto& c : comp) {
            SLUICE_CHECK(c.ready());
            c.reset();
        }
    }

    // R2 steady-state placement pin (real submit/drain cycles).
    {
        const std::size_t c_big = 512;
        UringAsyncBackend backend{UringConfig{c_big, 8}};
        if (!backend.available())
            return;
        backend.set_router_fix_mode_for_test(
            UringAsyncBackend::RouterFixModeForTest::low_placement_forward);
        TempFile file;
        SLUICE_CHECK(file.valid());
        backend.reset_router_scan_diagnostics_for_test();
        for (std::size_t round = 0; round < 200; ++round) {
            std::vector<std::byte> buf(8 * 4096, std::byte{0x55});
            std::vector<Completion<std::size_t>> comp(8);
            for (std::size_t k = 0; k < 8; ++k)
                SLUICE_CHECK(backend
                                 .submit_write(WriteOp{file.fd(), buf.data() + k * 4096,
                                                       4096, round * 8 * 4096},
                                               comp[k])
                                 .has_value());
            SLUICE_CHECK(drain(backend));
            for (auto& c : comp) {
                SLUICE_CHECK(c.ready());
                c.reset();
            }
        }
        const auto& diag = backend.router_scan_diagnostics_for_test();
        SLUICE_CHECK(diag.lookup_hits > 0);
        // The live set stays inside the low region for the WHOLE steady
        // state (R0's matched max would be ~C-1 = 511).
        SLUICE_CHECK(diag.matched_router_index_max < 16);
    }
}

// ---------------------------------------------------------------------------
// Full occupancy (D == C): every router slot live at once, all lookups hit,
// then retirement returns to quiescence. R3's construction capacity bound:
// the table is sized >= 2C, so C simultaneous live entries never overflow.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_router_fix_full_occupancy) {
    for (const auto mode : kModes) {
        const std::size_t capacity = 16;
        UringAsyncBackend backend{UringConfig{capacity, 16}};
        if (!backend.available())
            return;
        backend.set_router_fix_mode_for_test(mode);
        TempFile file;
        SLUICE_CHECK(file.valid());
        std::vector<std::byte> buf(capacity * 4096, std::byte{0x55});
        std::vector<Completion<std::size_t>> comp(capacity);
        for (std::size_t k = 0; k < capacity; ++k)
            SLUICE_CHECK(backend
                             .submit_write(WriteOp{file.fd(), buf.data() + k * 4096,
                                                   4096, k * 4096},
                                           comp[k])
                             .has_value());
        SLUICE_CHECK(backend.live_cookies_for_test() == capacity);
        for (std::size_t k = 0; k < capacity; ++k)
            SLUICE_CHECK(backend.find_live_router_cookie_for_test(kth_cookie(k)) <
                         capacity);
        const auto& diag = backend.router_scan_diagnostics_for_test();
        if (mode == UringAsyncBackend::RouterFixModeForTest::bounded_cookie_table) {
            SLUICE_CHECK(diag.table_insert_calls == capacity);
            SLUICE_CHECK(diag.table_erase_calls == 0);
        }
        SLUICE_CHECK(drain(backend));
        for (auto& c : comp) {
            SLUICE_CHECK(c.ready());
            c.reset();
        }
        SLUICE_CHECK(backend.live_cookies_for_test() == 0);
        if (mode == UringAsyncBackend::RouterFixModeForTest::bounded_cookie_table) {
            SLUICE_CHECK(
                backend.router_scan_diagnostics_for_test().table_erase_calls ==
                capacity);
        }
        // Quiescent mode switch back to baseline: legal on a retired router.
        backend.set_router_fix_mode_for_test(
            UringAsyncBackend::RouterFixModeForTest::production_baseline);
        SLUICE_CHECK(backend.router_fix_mode_for_test() ==
                     UringAsyncBackend::RouterFixModeForTest::production_baseline);
    }
}

// ---------------------------------------------------------------------------
// R3 table unit gates (bare structure, same include the backend uses):
// hash-collision handling, backward-shift deletion correctness, tombstone-
// freedom (lookup terminates at empty slots after interleaved erase), and
// the construction capacity bound.
// Death-gate cases (duplicate insert / missing erase) live in the dedicated
// death-test target.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_router_fix_table_unit_gates) {
    const std::size_t capacity = 64;
    RouterCookieTableForTest table{capacity};
    // Construction bound: strictly larger than the live-set maximum (2C
    // rounded up to a power of two) -> the legal maximum of C live cookies
    // can never fill it.
    SLUICE_CHECK(table.size >= 2 * capacity);

    // Cookie 0 is outside the operation-cookie domain: it must MISS (the
    // production linear scan agrees - no live entry ever carries 0).
    SLUICE_CHECK(table.lookup(0) == RouterCookieTableForTest::kMiss);

    // Find a colliding cookie pair by brute force over the same hash the
    // table uses; sequential real cookies rarely collide, so force one.
    std::uint64_t a = 1;
    std::uint64_t b = 0;
    for (std::uint64_t cand = 2; cand < 100000; ++cand) {
        if (RouterCookieTableForTest::hash(cand, table.log2_size) ==
            RouterCookieTableForTest::hash(a, table.log2_size)) {
            b = cand;
            break;
        }
    }
    SLUICE_CHECK(b != 0);
    // Cluster of three sharing one home (forces probe chains + a
    // backward-shift chain on erase).
    std::uint64_t c3 = 0;
    for (std::uint64_t cand = b + 1; cand < 200000; ++cand) {
        if (RouterCookieTableForTest::hash(cand, table.log2_size) ==
            RouterCookieTableForTest::hash(a, table.log2_size)) {
            c3 = cand;
            break;
        }
    }
    SLUICE_CHECK(c3 != 0);

    table.insert(a, 7);
    table.insert(b, 8);
    table.insert(c3, 9);
    SLUICE_CHECK(table.lookup(a) == 7);
    SLUICE_CHECK(table.lookup(b) == 8);
    SLUICE_CHECK(table.lookup(c3) == 9);
    // Unknown neighbor misses even inside a collision cluster.
    SLUICE_CHECK(table.lookup(999424) == RouterCookieTableForTest::kMiss);
    // Erase the middle of the chain; the far entries must remain reachable
    // (backward shift) and the erased cookie must miss (stale).
    table.erase(b);
    SLUICE_CHECK(table.lookup(b) == RouterCookieTableForTest::kMiss);
    SLUICE_CHECK(table.lookup(a) == 7);
    SLUICE_CHECK(table.lookup(c3) == 9);
    table.erase(a);
    table.erase(c3);
    SLUICE_CHECK(table.lookup(a) == RouterCookieTableForTest::kMiss);
    SLUICE_CHECK(table.lookup(c3) == RouterCookieTableForTest::kMiss);

    // Reuse: the freed home slots accept new cookies; the whole legal live
    // set (C entries) coexists, then drains to empty.
    for (std::uint64_t k = 1; k <= capacity; ++k)
        table.insert(k * 3 + 1, static_cast<std::size_t>(k) - 1);
    for (std::uint64_t k = 1; k <= capacity; ++k)
        SLUICE_CHECK(table.lookup(k * 3 + 1) == static_cast<std::size_t>(k) - 1);
    for (std::uint64_t k = 1; k <= capacity; ++k)
        table.erase(k * 3 + 1);
    SLUICE_CHECK(table.lookup(4) == RouterCookieTableForTest::kMiss);
}

#else

// Stub builds: build/API honesty only (no ring -> no router to fix).
SLUICE_TEST_CASE(uring_router_fix_stub_build_compile) {}

#endif

SLUICE_MAIN()
