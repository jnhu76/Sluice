// AC-1a (#234/#227): minimal resource-observation semantics for the
// ThreadPoolBackend production introspection surface. These tests verify
// SEMANTIC facts of the four-quantity vocabulary (capacity / occupancy /
// high-water / rejection count) against the real backend driving real file
// I/O — not implementation layout. Terminology constraint: nothing here may
// be labeled "pressure" (no time/progress-loss dimension exists at this tier)
// and demand is not claimed observable.
//
// Facts pinned:
//   A. RequestArena resource: occupancy 0 at rest; rises on acceptance; falls
//      only after reset (completion-ready still holds the slot); never exceeds
//      capacity; high-water monotonic and <= capacity; capacity_rejections
//      changes ONLY for actual capacity rejections (would_block) and is
//      monotonic across completions.
//   B. Dispatch occupancy: bounded by capacity while queued work exists;
//      drains to 0 once every accepted request completed; high-water valid,
//      <= capacity.
//   C. Workers: configured_worker_count stable; active_workers returns to 0
//      after drain (bookkeeping leak detector for the mark_running ->
//      post-syscall-decrement window).
//   D. Snapshot: on QUIESCENT state every field equals the corresponding
//      single-domain accessor — the only state where exact cross-field
//      agreement is defined for a component-wise coherent snapshot.
//
// The new accessors share the SAME lock domains as the pre-existing hot paths
// (arena leaf mutex_ / work_mtx_), so reader/writer concurrency of those
// domains keeps being exercised by the full ThreadPoolBackend suite under
// TSan; these cases assert semantics, not races.
#include "harness.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

using namespace sluice::async;

namespace {

class TempPath {
public:
    TempPath() {
        // Process-unique name (see threadpool_backend_test.cpp TempPath).
        path_ = (std::filesystem::temp_directory_path() /
                 ("sluice_ac1a_obs_" + std::to_string(::getpid()) + "_" +
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

int open_temp(const TempPath& tmp) {
    int fd = ::open(tmp.path().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        std::fprintf(stderr, "open failed\n");
        std::exit(1);
    }
    return fd;
}
// Reap until fully drained (poll is non-blocking; workers progress
// independently). Returns the number of Completions published.
std::size_t drain(ThreadPoolBackend& b) {
    std::size_t reaped = 0;
    for (;;) {
        reaped += b.poll();
        if (b.outstanding() == 0) return reaped;
    }
}

SLUICE_TEST_CASE(resource_observations_rest_state_zero) {
    ThreadPoolBackend be(ThreadPoolConfig{8, 2});
    auto snap = be.resource_snapshot();
    SLUICE_CHECK(be.arena_capacity() == 8);
    SLUICE_CHECK(be.configured_worker_count() == 2);
    SLUICE_CHECK(be.arena_slot_in_use() == 0);
    SLUICE_CHECK(be.outstanding() == 0);
    SLUICE_CHECK(be.arena_high_water_mark() == 0);
    SLUICE_CHECK(be.dispatch_occupancy() == 0);
    SLUICE_CHECK(be.dispatch_high_water_mark() == 0);
    SLUICE_CHECK(be.active_workers() == 0);
    SLUICE_CHECK(snap.arena_capacity == 8);
    SLUICE_CHECK(snap.configured_workers == 2);
    SLUICE_CHECK(snap.arena_slot_in_use == 0);
    SLUICE_CHECK(snap.accepted_outstanding == 0);
    SLUICE_CHECK(snap.arena_high_water_mark == 0);
    SLUICE_CHECK(snap.arena_capacity_rejections == 0);
    SLUICE_CHECK(snap.dispatch_occupancy == 0);
    SLUICE_CHECK(snap.dispatch_high_water_mark == 0);
    SLUICE_CHECK(snap.active_workers == 0);
}

SLUICE_TEST_CASE(arena_occupancy_lifecycle_boundaries_and_saturation_rejection) {
    TempPath tmp;
    int fd = open_temp(tmp);

    ThreadPoolBackend be(ThreadPoolConfig{/*capacity=*/2, /*workers=*/1});
    std::byte buf[8] = {};

    // Accept up to capacity: occupancy == accepted count.
    Completion<std::size_t> c1;
    Completion<std::size_t> c2;
    SLUICE_CHECK(be.submit_read(ReadOp{fd, buf, sizeof(buf), 0}, c1).has_value());
    SLUICE_CHECK(c1.outstanding());
    SLUICE_CHECK(be.outstanding() == 1);
    SLUICE_CHECK(be.arena_slot_in_use() == 1);
    SLUICE_CHECK(be.arena_high_water_mark() >= 1);

    SLUICE_CHECK(be.submit_read(ReadOp{fd, buf, sizeof(buf), sizeof(buf)}, c2)
                     .has_value());
    SLUICE_CHECK(c2.outstanding());
    SLUICE_CHECK(be.outstanding() == 2);
    SLUICE_CHECK(be.arena_slot_in_use() == 2);           // occupancy == capacity
    SLUICE_CHECK(be.arena_high_water_mark() == 2);       // monotonic peak

    // Saturation: exactly ONE would_block per oversubscribed attempt, zero
    // side effects on the rejected Completion, rejection counter tracks ONLY
    // refusals.
    Completion<std::size_t> cx;
    auto r3 = be.submit_read(ReadOp{fd, buf, sizeof(buf), 0}, cx);
    SLUICE_CHECK(!r3.has_value());
    SLUICE_CHECK(r3.error().code == sluice::IoError::Code::would_block);
    SLUICE_CHECK(cx.idle());
    SLUICE_CHECK(be.arena_capacity_rejections() == 1);
    SLUICE_CHECK(be.arena_slot_in_use() == 2);  // unchanged by refusal

    // Workers complete in the background (real file I/O). Completion-ready
    // does NOT release slots: occupancy stays 2 through reap...
    drain(be);
    SLUICE_CHECK(be.outstanding() == 0);
    SLUICE_CHECK(c1.ready() && c1.result().has_value());
    SLUICE_CHECK(c2.ready() && c2.result().has_value());
    SLUICE_CHECK(be.arena_slot_in_use() == 2);
    // ...the completion boundary that releases the slot is caller reset.
    c1.reset();
    SLUICE_CHECK(be.arena_slot_in_use() == 1);
    c2.reset();
    SLUICE_CHECK(be.arena_slot_in_use() == 0);
    // High-water stayed at the observed peak; dispatch drained; no worker
    // leak. Rejection counter is monotonic across completions.
    SLUICE_CHECK(be.arena_high_water_mark() == 2);
    SLUICE_CHECK(be.dispatch_occupancy() == 0);
    SLUICE_CHECK(be.active_workers() == 0);
    SLUICE_CHECK(be.arena_capacity_rejections() == 1);
    ::close(fd);
}

SLUICE_TEST_CASE(dispatch_occupancy_bounded_and_drains_to_zero_under_pipeline) {
    TempPath tmp;
    int fd = open_temp(tmp);

    ThreadPoolBackend be(ThreadPoolConfig{/*capacity=*/4, /*workers=*/1});
    std::byte payload[64] = {};

    std::size_t peak_dispatch_seen = 0;
    for (int round = 0; round < 24; ++round) {
        Completion<std::size_t> c;
        SLUICE_CHECK(
            be.submit_write(
                  WriteOp{fd, payload, sizeof(payload),
                          static_cast<std::uint64_t>(round) * sizeof(payload)},
                  c)
                .has_value());
        // Dispatch occupancy counts QUEUED-but-not-dequeued work only: it can
        // never exceed ring capacity, and with one promptly-draining worker it
        // lags arena outstanding (outstanding includes running + terminal).
        peak_dispatch_seen = peak_dispatch_seen > be.dispatch_occupancy()
                                 ? peak_dispatch_seen
                                 : be.dispatch_occupancy();
        SLUICE_CHECK(be.dispatch_occupancy() <= 4);
        SLUICE_CHECK(be.active_workers() <= 1);
        // Drive to completion each round: deterministic requeue boundaries.
        // poll() is non-blocking; workers record terminals independently, so
        // poll()==0 while outstanding simply means "not terminal yet".
        while (!c.ready()) {
            be.poll();
        }
        SLUICE_CHECK(c.result().has_value());
        c.reset();
        // Fully drained state: dispatch empty, nobody running or queued.
        SLUICE_CHECK(be.dispatch_occupancy() == 0);
        SLUICE_CHECK(be.active_workers() == 0);
        SLUICE_CHECK(be.outstanding() == 0);
        SLUICE_CHECK(be.arena_slot_in_use() == 0);
    }
    SLUICE_CHECK(be.dispatch_high_water_mark() >= 1);
    SLUICE_CHECK(be.dispatch_high_water_mark() <= 4);
    SLUICE_CHECK(peak_dispatch_seen <= be.dispatch_high_water_mark());
    ::close(fd);
}

SLUICE_TEST_CASE(capacity_rejection_counter_tracks_only_would_block_refusals) {
    TempPath tmp;
    int fd = open_temp(tmp);

    ThreadPoolBackend be(ThreadPoolConfig{/*capacity=*/2, /*workers=*/1});
    std::byte buf[4] = {};
    // Accept to saturation with LONG-LIVED Completions (they must outlive the
    // whole acceptance window: destroying an outstanding Completion is a
    // contract violation and fail-fasts by design).
    Completion<std::size_t> c1;
    Completion<std::size_t> c2;
    Completion<std::size_t> cx;
    std::size_t accepted = 0;
    std::size_t refused = 0;
    for (int i = 0; i < 10; ++i) {
        sluice::Result<void> r;
        switch (accepted) {
        case 0: r = be.submit_read(ReadOp{fd, buf, sizeof(buf), 0}, c1); break;
        case 1: r = be.submit_read(ReadOp{fd, buf, sizeof(buf), sizeof(buf)}, c2); break;
        default: r = be.submit_read(ReadOp{fd, buf, sizeof(buf), 0}, cx); break;
        }
        if (r.has_value()) {
            ++accepted;
            SLUICE_CHECK((accepted == 1 ? c1 : c2).outstanding());
        } else {
            SLUICE_CHECK(r.error().code == sluice::IoError::Code::would_block);
            SLUICE_CHECK(cx.idle());
            ++refused;
        }
        SLUICE_CHECK(be.arena_capacity_rejections() == refused);
        SLUICE_CHECK(be.arena_slot_in_use() == accepted);  // == min(i+1, cap)
        SLUICE_CHECK(be.arena_slot_in_use() <= 2);
    }
    SLUICE_CHECK(refused > 0 && accepted == 2);
    drain(be);
    SLUICE_CHECK(c1.ready() && c1.result().has_value());
    SLUICE_CHECK(c2.ready() && c2.result().has_value());
    // Reset releases the final slots; rejection counter is monotonic through
    // completions (it counts refusals, which completions do not "undo").
    c1.reset();
    c2.reset();
    SLUICE_CHECK(be.arena_slot_in_use() == 0);
    SLUICE_CHECK(be.arena_capacity_rejections() == refused);  // monotonic
    ::close(fd);
}

SLUICE_TEST_CASE(snapshot_fields_equal_single_domain_accessors_when_quiescent) {
    ThreadPoolBackend be(ThreadPoolConfig{6, 3});
    auto snap = be.resource_snapshot();
    // Quiescence removes inter-domain motion, so field-wise equality with the
    // individual accessors is well-defined and required.
    SLUICE_CHECK(snap.arena_capacity == be.arena_capacity());
    SLUICE_CHECK(snap.arena_slot_in_use == be.arena_slot_in_use());
    SLUICE_CHECK(snap.arena_high_water_mark == be.arena_high_water_mark());
    SLUICE_CHECK(snap.arena_capacity_rejections == be.arena_capacity_rejections());
    SLUICE_CHECK(snap.accepted_outstanding == be.outstanding());
    SLUICE_CHECK(snap.dispatch_capacity == 6);
    SLUICE_CHECK(snap.dispatch_occupancy == be.dispatch_occupancy());
    SLUICE_CHECK(snap.dispatch_high_water_mark == be.dispatch_high_water_mark());
    SLUICE_CHECK(snap.configured_workers == be.configured_worker_count());
    SLUICE_CHECK(snap.active_workers == be.active_workers());
}

SLUICE_TEST_CASE(concurrent_observer_sampling_while_io_runs) {
    // TSan-focused: the observation accessors are NEW readers on the EXISTING
    // authority locks (arena leaf mutex_ / work_mtx_). A dedicated observer
    // thread samples snapshots and single accessors while a producer runs real
    // I/O through the backend, forcing the new reader/writer pairs (snapshot
    // vs worker bookkeeping, accessor vs submit enqueue) to execute
    // concurrently. Deterministic end-state assertions only; the loop is a
    // sampling driver, not an ordering proof.
    TempPath tmp;
    int fd = open_temp(tmp);

    ThreadPoolBackend be(ThreadPoolConfig{/*capacity=*/8, /*workers=*/2});
    std::atomic<bool> producer_done{false};
    std::thread observer([&] {
        while (!producer_done.load(std::memory_order_acquire)) {
            auto snap = be.resource_snapshot();
            // Per-field semantic bounds that hold at ANY instant.
            SLUICE_CHECK(snap.arena_slot_in_use <= snap.arena_capacity);
            SLUICE_CHECK(snap.accepted_outstanding <= snap.arena_slot_in_use);
            SLUICE_CHECK(snap.dispatch_occupancy <= snap.dispatch_capacity);
            SLUICE_CHECK(snap.active_workers <= snap.configured_workers);
        }
    });

    const std::byte buf[32] = {};
    constexpr int kRounds = 64;
    for (int i = 0; i < kRounds; ++i) {
        Completion<std::size_t> c;
        SLUICE_CHECK(
            be.submit_write(WriteOp{fd, buf, sizeof(buf),
                                    static_cast<std::uint64_t>(i) * sizeof(buf)},
                            c)
                .has_value());
        while (!c.ready()) {
            be.poll();
        }
        c.reset();
    }
    producer_done.store(true, std::memory_order_release);
    observer.join();

    drain(be);
    SLUICE_CHECK(be.outstanding() == 0);
    SLUICE_CHECK(be.dispatch_occupancy() == 0);
    SLUICE_CHECK(be.active_workers() == 0);
    SLUICE_CHECK(be.arena_slot_in_use() == 0);
    SLUICE_CHECK(be.arena_high_water_mark() >= 1);
    SLUICE_CHECK(be.dispatch_high_water_mark() >= 1);
    ::close(fd);
}

} // namespace

SLUICE_MAIN()
