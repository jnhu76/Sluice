// async_test_control.cpp — internal-testing controller implementation.
// (ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1)
//
// Compiled ONLY into the `sluice_async_internal_testing` variant (via the
// SLUICE_ASYNC_INTERNAL_TESTING define + the source manifest). The production
// `sluice_async` target does NOT compile this file, so it exports no controller
// symbols.
//
// This object owns the per-Scheduler controller registry. scheduler.cpp (in the
// variant) calls test_phase/release_all_phases; test TUs call register/
// unregister/arm/wait/release. The phase hot path performs a lookup into the
// pre-populated registry (no allocation, no insertion/erase/rehash on the
// reached path), then marks reached and optionally blocks.
#include "async_test_control_internal.hpp"

#include <sluice/async/scheduler.hpp>

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace sluice_async_test {

namespace {

// The registry. Mutated only by register_controller/unregister_controller
// (test setup/teardown). test_phase performs a find() (no insertion).
// Guarded by registry_mtx.
std::mutex registry_mtx;
std::unordered_map<sluice::async::Scheduler*, std::unique_ptr<SchedulerController>> registry;

// Unregistration removes lookup authority immediately, but an in-flight
// test_phase may still hold the raw pointer returned while registry_mtx was
// locked. Retain removed controllers until process teardown, after every test
// thread has joined, so their mutex/CV storage cannot be destroyed underneath
// that lease.
std::vector<std::unique_ptr<SchedulerController>> retired_controllers;

// Look up the controller for `s` WITHOUT allocating. Returns nullptr if not
// registered. Called on the phase hot path.
SchedulerController* find_controller(sluice::async::Scheduler& s) noexcept {
    // lock_guard: find() must not race a concurrent unregister. This is the
    // only lock taken on the hot path; it is held only for the lookup, then
    // released before any phase-state block.
    std::lock_guard<std::mutex> lk(registry_mtx);
    auto it = registry.find(&s);
    return it == registry.end() ? nullptr : it->second.get();
}

PhaseState& phase_of(SchedulerController& c, PhaseTag tag) noexcept {
    return c.phases[static_cast<std::size_t>(tag)];
}

// Reset every rollback-observation field on the controller. Shared by
// configure_rollback_fail_after and reset_rollback_injection (which differ only
// in the rollback_configured_fail_after value they store). Caller MUST already
// hold the select_rollback_aborted phase lock: these fields are observed under it.
void reset_rollback_observation_locked(SchedulerController& c) noexcept {
    c.rollback_successful_registrations = 0;
    c.rollback_begin_count = 0;
    c.rollback_finish_count = 0;
    c.rollback_arm_order_len = 0;
    c.rollback_arm_order_indices.fill(0);
    c.rollback_arm_order_kinds.fill(0);
    c.rollback_event_linked_before.fill(false);
}

}  // namespace

void test_phase_worker(sluice::async::Scheduler& s, PhaseTag tag,
                       unsigned worker_id) noexcept {
    // No allocation: find only. If the Scheduler has no registered controller
    // (e.g. a variant-lib path hit during a non-test run), this is a no-op.
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    PhaseState& p = phase_of(*c, tag);
    // Mark reached + optionally block. We acquire the phase's own mtx (NOT a
    // production lock) so the test coordinator can observe/ release. When the
    // phase is armed, we block here until release — the production lock held
    // by the caller (e.g. global_mtx_) remains held, which is the guarantee
    // under test. Issue #161 per-worker arming: a pinned armed_worker blocks
    // only the identified worker; the pre-existing global arming (kAnyWorker)
    // blocks every arriver, preserving the test_phase semantics unchanged.
    {
        std::lock_guard<std::mutex> lk(p.mtx);
        p.reached = true;
        // The pauser owns `paused`: set it only for the worker that actually
        // blocks, so a non-pinned peer's pass through the same call site
        // cannot clear an active hold while the pinned worker is still
        // blocked in the cv wait below (wait_paused/is_paused would then miss
        // the hold).
        const bool will_pause =
            p.armed &&
            (p.armed_worker == kAnyWorker || p.armed_worker == worker_id);
        if (will_pause) {
            p.paused = true;
            p.pauser = worker_id;
        }
    }
    p.cv.notify_all();  // tell the coordinator we reached + paused
    {
        std::unique_lock<std::mutex> lk(p.mtx);
        if (p.paused && p.pauser == worker_id) {
            p.cv.wait(lk, [&p] { return !p.armed; });
            p.paused = false;
            p.pauser = kAnyWorker;
        }
    }
}

void test_phase(sluice::async::Scheduler& s, PhaseTag tag) noexcept {
    test_phase_worker(s, tag, kAnyWorker);
}

void release_all_phases(sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    // Disarm every phase so any paused worker observes termination.
    for (std::size_t i = 0; i < std::size(c->phases); ++i) {
        // These phases are reached only after every worker has joined (or
        // hold a survivor that a sibling's termination must NOT silently
        // release). They cannot strand a worker during termination in the
        // seam's own right, and preserving their armed state lets the
        // coordinator control the boundary:
        //   - worker_topology_joined_before_unpublish: post-join, so pausing
        //     there is inherently safe;
        //   - worker_park_returned (Phase G review P2b): the G1 deterministic
        //     reproducer holds the SURVIVOR of a sibling worker's
        //     mw_s2_no_progress_terminate exit exactly at its post-park
        //     recheck; that exit path calls release_all_phases, and releasing
        //     the hold would destroy the reproduction (the survivor would
        //     proceed before the test published the backend completion). The
        //     arming test owns releasing it — its bounded watchdog is the
        //     escape hatch, mirroring the forensic-stall fail-closed pattern.
        if (i == static_cast<std::size_t>(
                     PhaseTag::worker_topology_joined_before_unpublish) ||
            i == static_cast<std::size_t>(PhaseTag::worker_park_returned)) {
            continue;
        }
        PhaseState& p = c->phases[i];
        {
            std::lock_guard<std::mutex> lk(p.mtx);
            p.armed = false;
        }
        p.cv.notify_all();
    }
}

void register_controller(sluice::async::Scheduler& s) noexcept {
    std::lock_guard<std::mutex> lk(registry_mtx);
    // emplace: if already present, leave the existing entry (idempotent).
    registry.try_emplace(&s, std::make_unique<SchedulerController>());
}

void unregister_controller(sluice::async::Scheduler& s) noexcept {
    std::lock_guard<std::mutex> lk(registry_mtx);
    auto node = registry.extract(&s);
    if (!node.empty()) {
        retired_controllers.push_back(std::move(node.mapped()));
    }
}

void arm(sluice::async::Scheduler& s, PhaseTag tag) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    PhaseState& p = phase_of(*c, tag);
    {
        std::lock_guard<std::mutex> lk(p.mtx);
        p.armed = true;
        p.armed_worker = kAnyWorker;  // plain arm is global (issue #161 note)
        p.reached = false;
        p.paused = false;
        p.pauser = kAnyWorker;
    }
}

void arm_worker(sluice::async::Scheduler& s, PhaseTag tag,
                unsigned worker_id) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    PhaseState& p = phase_of(*c, tag);
    {
        std::lock_guard<std::mutex> lk(p.mtx);
        p.armed = true;
        p.armed_worker = worker_id;
        p.reached = false;
        p.paused = false;
        p.pauser = kAnyWorker;
    }
}

void wait_reached(sluice::async::Scheduler& s, PhaseTag tag) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    PhaseState& p = phase_of(*c, tag);
    std::unique_lock<std::mutex> lk(p.mtx);
    p.cv.wait(lk, [&p] { return p.reached; });
}

bool is_reached(sluice::async::Scheduler& s, PhaseTag tag) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return false;
    PhaseState& p = phase_of(*c, tag);
    std::lock_guard<std::mutex> lk(p.mtx);
    return p.reached;
}

void wait_paused(sluice::async::Scheduler& s, PhaseTag tag) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    PhaseState& p = phase_of(*c, tag);
    std::unique_lock<std::mutex> lk(p.mtx);
    p.cv.wait(lk, [&p] { return p.paused; });
}

bool is_paused(sluice::async::Scheduler& s, PhaseTag tag) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return false;
    PhaseState& p = phase_of(*c, tag);
    std::lock_guard<std::mutex> lk(p.mtx);
    return p.paused;
}

void release(sluice::async::Scheduler& s, PhaseTag tag) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    PhaseState& p = phase_of(*c, tag);
    {
        std::lock_guard<std::mutex> lk(p.mtx);
        p.armed = false;
    }
    p.cv.notify_all();
}

void disarm(sluice::async::Scheduler& s, PhaseTag tag) noexcept {
    release(s, tag);
}

void clear_reached(sluice::async::Scheduler& s, PhaseTag tag) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    PhaseState& p = phase_of(*c, tag);
    {
        std::lock_guard<std::mutex> lk(p.mtx);
        p.reached = false;
    }
}

void capture_admission_snapshot(sluice::async::Scheduler& s, PhaseTag tag,
                                const AdmissionSnapshot& snap) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    PhaseState& p = phase_of(*c, tag);
    // Write the snapshot under the controller's own mutex so the coordinator
    // thread can read it without acquiring any production lock. The admission
    // worker holds global_mtx_ at this point.
    {
        std::lock_guard<std::mutex> lk(p.mtx);
        if (tag == PhaseTag::select_admission_armed) {
            c->admission_armed_snapshot = snap;
        } else if (tag == PhaseTag::select_admission_consumed) {
            c->admission_consumed_snapshot = snap;
        }
    }
}

AdmissionSnapshot read_admission_snapshot(sluice::async::Scheduler& s,
                                           PhaseTag tag) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return AdmissionSnapshot{};
    PhaseState& p = phase_of(*c, tag);
    std::lock_guard<std::mutex> lk(p.mtx);
    if (tag == PhaseTag::select_admission_armed) {
        return c->admission_armed_snapshot;
    } else if (tag == PhaseTag::select_admission_consumed) {
        return c->admission_consumed_snapshot;
    }
    return AdmissionSnapshot{};
}

// ---- E13 P6 publication snapshots + counters ----
void capture_publication_snapshot(sluice::async::Scheduler& s, PhaseTag tag,
                                  const PublicationSnapshot& snap) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    PhaseState& p = phase_of(*c, tag);
    // Write the snapshot under the controller's own mutex so the coordinator
    // thread can read it without acquiring any production lock. The publication
    // / resume path holds global_mtx_ at this point.
    {
        std::lock_guard<std::mutex> lk(p.mtx);
        if (tag == PhaseTag::select_publish_entry) {
            c->publish_entry_snapshot = snap;
        } else if (tag == PhaseTag::select_publish_done) {
            c->publish_done_snapshot = snap;
        } else if (tag == PhaseTag::select_suspended_before_consume) {
            c->suspended_before_consume_snapshot = snap;
        }
    }
}

PublicationSnapshot read_publication_snapshot(sluice::async::Scheduler& s,
                                              PhaseTag tag) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return PublicationSnapshot{};
    PhaseState& p = phase_of(*c, tag);
    std::lock_guard<std::mutex> lk(p.mtx);
    if (tag == PhaseTag::select_publish_entry) {
        return c->publish_entry_snapshot;
    } else if (tag == PhaseTag::select_publish_done) {
        return c->publish_done_snapshot;
    } else if (tag == PhaseTag::select_suspended_before_consume) {
        return c->suspended_before_consume_snapshot;
    }
    return PublicationSnapshot{};
}

void increment_result_publication(sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    // The counters are read/written by the controller's own coordination mutex
    // (the same mutex the snapshots use) so a coordinator thread observes them
    // without acquiring global_mtx_. Incremented under global_mtx_ by the
    // publication path.
    PhaseState& p = phase_of(*c, PhaseTag::select_publish_done);
    std::lock_guard<std::mutex> lk(p.mtx);
    ++c->result_publication_count;
}

void increment_runnable_publication(sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    PhaseState& p = phase_of(*c, PhaseTag::select_publish_done);
    std::lock_guard<std::mutex> lk(p.mtx);
    ++c->runnable_publication_count;
}

std::size_t result_publication_count(sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return 0;
    PhaseState& p = phase_of(*c, PhaseTag::select_publish_done);
    std::lock_guard<std::mutex> lk(p.mtx);
    return c->result_publication_count;
}

std::size_t runnable_publication_count(sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return 0;
    PhaseState& p = phase_of(*c, PhaseTag::select_publish_done);
    std::lock_guard<std::mutex> lk(p.mtx);
    return c->runnable_publication_count;
}

void reset_publication_counts(sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    PhaseState& p = phase_of(*c, PhaseTag::select_publish_done);
    std::lock_guard<std::mutex> lk(p.mtx);
    c->result_publication_count = 0;
    c->runnable_publication_count = 0;
}

// ---- E13 P7: synthetic registration-failure injection + rollback observability
// (task §18 / §19). All controller-only; the production target has none of it.
void configure_rollback_fail_after(sluice::async::Scheduler& s,
                                   std::size_t fail_after) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    PhaseState& p = phase_of(*c, PhaseTag::select_rollback_aborted);
    std::lock_guard<std::mutex> lk(p.mtx);
    c->rollback_configured_fail_after = fail_after;
    // Reset the per-call observation for the next failing admission.
    reset_rollback_observation_locked(*c);
}

void reset_rollback_injection(sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    PhaseState& p = phase_of(*c, PhaseTag::select_rollback_aborted);
    std::lock_guard<std::mutex> lk(p.mtx);
    c->rollback_configured_fail_after = kRollbackFailAfterDisabled;
    reset_rollback_observation_locked(*c);
}

std::size_t rollback_fail_after(sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return kRollbackFailAfterDisabled;
    PhaseState& p = phase_of(*c, PhaseTag::select_rollback_aborted);
    std::lock_guard<std::mutex> lk(p.mtx);
    return c->rollback_configured_fail_after;
}

bool rollback_should_inject_after(sluice::async::Scheduler& s,
                                  std::size_t successful) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return false;
    PhaseState& p = phase_of(*c, PhaseTag::select_rollback_aborted);
    std::lock_guard<std::mutex> lk(p.mtx);
    const std::size_t boundary = c->rollback_configured_fail_after;
    if (boundary == kRollbackFailAfterDisabled) return false;
    return successful >= boundary;
}

// ---- R2-ALLOC: ordinary timed-admission allocation-failure injection.
// Controller-only; the production target has none of it. The guarded hook
// runs at prepare_ordinary_deadline_locked entry under global_mtx_; this
// leaf controller mutex is held only for the flag check/clear (the phase-
// state discipline — never held while acquiring any production lock).
void arm_ordinary_deadline_alloc_failure(
    sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    std::lock_guard<std::mutex> lk(c->ordinary_deadline_alloc_fail_mtx);
    c->ordinary_deadline_alloc_fail_armed = true;
}

void disarm_ordinary_deadline_alloc_failure(
    sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    std::lock_guard<std::mutex> lk(c->ordinary_deadline_alloc_fail_mtx);
    c->ordinary_deadline_alloc_fail_armed = false;
}

bool ordinary_deadline_alloc_should_fail(
    sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return false;
    std::lock_guard<std::mutex> lk(c->ordinary_deadline_alloc_fail_mtx);
    if (!c->ordinary_deadline_alloc_fail_armed) return false;
    c->ordinary_deadline_alloc_fail_armed = false;  // one-shot
    return true;
}

// ---- FE-CORRECTIVE-1 P1-1: deferred-publication storage-failure injection.
// Same controller discipline as the R2 pair; the guarded hook runs at the
// defer_publication_locked insertion edge under global_mtx_.
void arm_deferred_publication_alloc_failure(
    sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    std::lock_guard<std::mutex> lk(c->deferred_publication_alloc_fail_mtx);
    c->deferred_publication_alloc_fail_armed = true;
}

void disarm_deferred_publication_alloc_failure(
    sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    std::lock_guard<std::mutex> lk(c->deferred_publication_alloc_fail_mtx);
    c->deferred_publication_alloc_fail_armed = false;
}

bool deferred_publication_alloc_should_fail(
    sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return false;
    std::lock_guard<std::mutex> lk(c->deferred_publication_alloc_fail_mtx);
    if (!c->deferred_publication_alloc_fail_armed) return false;
    c->deferred_publication_alloc_fail_armed = false;  // one-shot
    return true;
}

void rollback_record_begin(sluice::async::Scheduler& s,
                           std::size_t successful_registrations) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    PhaseState& p = phase_of(*c, PhaseTag::select_rollback_aborted);
    std::lock_guard<std::mutex> lk(p.mtx);
    c->rollback_successful_registrations = successful_registrations;
    ++c->rollback_begin_count;
    // Reset the per-arm order buffer for this rollback pass.
    c->rollback_arm_order_len = 0;
}

void rollback_record_arm(sluice::async::Scheduler& s,
                         std::uint32_t arm_index,
                         std::uint8_t arm_kind_raw,
                         bool event_linked_before) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    PhaseState& p = phase_of(*c, PhaseTag::select_rollback_aborted);
    std::lock_guard<std::mutex> lk(p.mtx);
    const std::size_t idx = c->rollback_arm_order_len;
    if (idx < c->rollback_arm_order_indices.size()) {
        c->rollback_arm_order_indices[idx] = arm_index;
        c->rollback_arm_order_kinds[idx] = arm_kind_raw;
        c->rollback_event_linked_before[idx] = event_linked_before;
        ++c->rollback_arm_order_len;
    }
}

void rollback_record_finish(sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    PhaseState& p = phase_of(*c, PhaseTag::select_rollback_aborted);
    std::lock_guard<std::mutex> lk(p.mtx);
    ++c->rollback_finish_count;
}

RollbackObservation read_rollback_observation(
    sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return RollbackObservation{};
    PhaseState& p = phase_of(*c, PhaseTag::select_rollback_aborted);
    std::lock_guard<std::mutex> lk(p.mtx);
    RollbackObservation o;
    o.configured_fail_after = c->rollback_configured_fail_after;
    o.successful_registrations = c->rollback_successful_registrations;
    o.begin_count = c->rollback_begin_count;
    o.finish_count = c->rollback_finish_count;
    o.arm_order_len = c->rollback_arm_order_len;
    o.arm_order_indices = c->rollback_arm_order_indices;
    o.arm_order_kinds = c->rollback_arm_order_kinds;
    o.event_linked_before = c->rollback_event_linked_before;
    return o;
}

// ---- #196 E9 trace-conformance recorder ----
// Controller-owned fixed ring; every entry point is a no-op without a
// registered controller, and recording is a no-op outside the test's capture
// window. trace_mtx is a controller leaf: the guarded production call sites
// invoke these while holding wake_mtx_ / global_mtx_ (the same discipline as
// test_phase, which also takes a controller mutex under production locks).

void trace_enable(sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    std::lock_guard<std::mutex> lk(c->trace_mtx);
    c->trace_enabled = true;
}

void trace_disable(sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    std::lock_guard<std::mutex> lk(c->trace_mtx);
    c->trace_enabled = false;
}

void trace_clear(sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    std::lock_guard<std::mutex> lk(c->trace_mtx);
    c->trace_len = 0;
    c->trace_overflow = false;
    c->pending_wake_cause = 0;
    c->pending_wake_worker = TraceEvent{}.worker;
}

bool trace_overflow(sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return false;
    std::lock_guard<std::mutex> lk(c->trace_mtx);
    return c->trace_overflow;
}

std::vector<TraceEvent> trace_events(sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return {};
    std::lock_guard<std::mutex> lk(c->trace_mtx);
    return std::vector<TraceEvent>(c->trace_events.begin(),
                                   c->trace_events.begin() +
                                       static_cast<std::ptrdiff_t>(c->trace_len));
}

void record_trace_event(sluice::async::Scheduler& s,
                        const TraceEvent& ev) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    std::lock_guard<std::mutex> lk(c->trace_mtx);
    if (!c->trace_enabled) return;
    if (c->trace_len >= c->trace_events.size()) {
        c->trace_overflow = true;  // drop; the test's shape check fail-closes
        return;
    }
    c->trace_events[c->trace_len++] = ev;
}

void set_trace_wake_cause(sluice::async::Scheduler& s, WakeCause cause,
                          unsigned worker) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    std::lock_guard<std::mutex> lk(c->trace_mtx);
    c->pending_wake_cause = static_cast<unsigned char>(cause);
    c->pending_wake_worker = worker == static_cast<unsigned>(-1)
                                 ? TraceEvent{}.worker
                                 : static_cast<unsigned char>(worker);
}

void record_trace_wake(sluice::async::Scheduler& s,
                       std::uint64_t new_epoch) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    std::lock_guard<std::mutex> lk(c->trace_mtx);
    if (!c->trace_enabled) {
        // Consume the marker even outside the window so a stale attribution
        // can never attach to a later in-window wake.
        c->pending_wake_cause = 0;
        c->pending_wake_worker = TraceEvent{}.worker;
        return;
    }
    TraceEvent ev;
    ev.kind = static_cast<unsigned char>(TraceEventKind::wake_published);
    ev.cause = c->pending_wake_cause;
    ev.worker = c->pending_wake_worker;
    ev.epoch = new_epoch;
    c->pending_wake_cause = 0;
    c->pending_wake_worker = TraceEvent{}.worker;
    if (c->trace_len >= c->trace_events.size()) {
        c->trace_overflow = true;
        return;
    }
    c->trace_events[c->trace_len++] = ev;
}

// ---- DST-PV-1 schedule script (test-only next-runnable choice) ----

namespace {

// The installing thread's script activation (review P1-1: the INACTIVE
// worker-pop path must be a no-lock fast path — one TLS pair + a Scheduler
// identity compare; NO registry mutex, NO script mutex, NO atomic). The
// script is visible ONLY on the installing thread: the PV model installs
// before an inline run(1), which runs worker_loop on the SAME thread
// (scheduler.cpp single-worker fast path). A worker on any other thread —
// every multi-worker run — never activates and stays on the FIFO pop:
// multi-worker scheduling space is structurally untouched.
struct ActiveScript {
    const sluice::async::Scheduler* sched = nullptr;
    SchedulerController::ScheduleScriptState* state = nullptr;
};
thread_local ActiveScript t_active_script;

// Review R3 P1: while an Invoke action runs, installing / re-arming a
// schedule script is FORBIDDEN (fail-loud; see install_schedule_script). If
// it were allowed, the OLD script's post-action epilogue would advance the
// NEW script's step counter — silent deterministic-replay corruption.
// Same-thread flag only: the pick and the action run on the installing
// thread, so no atomic is needed. Cleared after the action returns; if the
// action aborts, no cleanup is required beyond process teardown.
thread_local bool t_schedule_invoke_active = false;

// Fail loudly on a script-configuration violation (capacity overflow or a
// step referencing an unregistered action). A test-only harness must never
// silently truncate a decision vector or fall back (review P1-4).
[[noreturn]] void schedule_script_config_fail(const char* what,
                                              std::size_t value) noexcept {
    std::fprintf(stderr,
                 "\n=== DST-PV-1 SCHEDULE SCRIPT FAILURE (configuration) ===\n"
                 "%s: %zu\n"
                 "===================================================\n",
                 what, value);
    std::fflush(stderr);
    std::abort();
}

// Render the failure diagnostic package and abort. NEVER falls back to
// another runnable: a replay that requests an illegal decision is a replay
// bug, and silently choosing a different schedule would destroy replay
// fidelity (the whole point of the decision vector).
[[noreturn]] void schedule_script_fail(
    SchedulerController::ScheduleScriptState& st,
    sluice::async::Scheduler& s, sluice::async::WorkerState* ws,
    std::size_t step_index, unsigned char requested_id,
    const std::vector<sluice::async::Fiber*>& legal) noexcept {
    st.failed = true;
    st.failed_step = step_index;
    st.requested_id = requested_id;
    std::fprintf(stderr,
                 "\n=== DST-PV-1 SCHEDULE SCRIPT FAILURE (deterministic "
                 "replay diagnostic) ===\n"
                 "test:            %s\n"
                 "step index:      %zu\n"
                 "requested:       Run(%u)\n"
                 "logical clock:   %llu\n"
                 "replay vector:   %s\n",
                 st.test_name != nullptr ? st.test_name : "<unnamed>",
                 step_index, static_cast<unsigned>(requested_id),
                 static_cast<unsigned long long>(
                     sluice::async::Scheduler::AsyncTestAccess::clock_now(s)),
                 st.replay_vector != nullptr ? st.replay_vector->c_str()
                                             : "<none>");
    std::fprintf(stderr, "legal runnable:  ");
    if (legal.empty()) {
        std::fprintf(stderr, "<empty>");
    }
    for (sluice::async::Fiber* f : legal) {
        // Render bound participants as their test-assigned id; unbound
        // fibers as <unbound> (addresses are NOT replay syntax).
        bool bound = false;
        for (std::size_t i = 0; i < st.fiber_count; ++i) {
            if (st.fiber_ptrs[i] == f) {
                std::fprintf(stderr, "%sRun(%u)", f == legal.front() ? "" : ", ",
                             static_cast<unsigned>(i));
                bound = true;
                break;
            }
        }
        if (!bound) {
            std::fprintf(stderr, "%s<unbound>", f == legal.front() ? "" : ", ");
        }
    }
    std::fprintf(stderr, "\nqueue depth:     %zu\n",
                 [&] {
                     std::lock_guard<std::mutex> lk(ws->inbox_mtx);
                     return ws->local_runnable.size();
                 }());
    std::fprintf(stderr, "executed prefix: ");
    if (st.executed_len == 0) std::fprintf(stderr, "<none>");
    for (std::size_t i = 0; i < st.executed_len; ++i) {
        const auto& e = st.executed[i];
        if (e.kind == SchedulerController::ScheduleScriptStep::Kind::run) {
            std::fprintf(stderr, "%sRun(%u)", i == 0 ? "" : " -> ",
                         static_cast<unsigned>(e.payload));
        } else {
            const char* label = "<action>";
            if (e.payload < st.action_count &&
                st.action_labels[e.payload] != nullptr) {
                label = st.action_labels[e.payload];
            }
            std::fprintf(stderr, "%s%s", i == 0 ? "" : " -> ", label);
        }
    }
    std::fprintf(stderr, "\n=========================================="
                         "=======================\n");
    std::fflush(stderr);
    std::abort();
}

}  // namespace

bool schedule_script_active(sluice::async::Scheduler& s) noexcept {
    // No-lock gate: a TLS activation pair is set ONLY by
    // install_schedule_script on the installing thread and the inline run(1)
    // worker loop runs on that same thread, so `enabled` / `next_step` are
    // same-thread reads — no mutex, no atomic, no registry lookup on the
    // per-pop path. Every other thread (multi-worker runs) has an empty pair
    // and returns false here.
    const ActiveScript& a = t_active_script;
    if (a.sched != &s || a.state == nullptr) return false;
    const SchedulerController::ScheduleScriptState& st = *a.state;
    return st.enabled && st.next_step < st.step_count;
}

sluice::async::Fiber* schedule_script_pick(sluice::async::Scheduler& s,
                                           sluice::async::WorkerState* ws) noexcept {
    const ActiveScript& a = t_active_script;
    if (a.sched != &s || a.state == nullptr) return nullptr;
    SchedulerController::ScheduleScriptState& st = *a.state;
    while (true) {
        SchedulerController::ScheduleScriptStep step;
        {
            std::lock_guard<std::mutex> lk(st.mtx);
            if (!st.enabled || st.next_step >= st.step_count) return nullptr;
            step = st.steps[st.next_step];
        }
        if (step.kind == SchedulerController::ScheduleScriptStep::Kind::invoke) {
            // Review R3 P1: the action is invoked THROUGH A STABLE POINTER
            // into the controller's fixed action array — NO std::function
            // copy on the pick path, so the pick allocates nothing after
            // install. The pointer is stable because: the array is fixed
            // storage inside the controller (retained until process
            // teardown); re-install / re-arm is fail-closed during Invoke
            // (install_schedule_script below); and uninstall never rewrites
            // the array. The script mutex guards only the lookup, then the
            // action runs AFTER release and the step is recorded on
            // re-acquire — the mutex is NEVER held while an action runs.
            // Actions may re-enter SUPPORTED NON-REPLACING test-control
            // surfaces (uninstall_schedule_script, advance_clock,
            // cancel_wait, fake/scripted I/O completion); installing or
            // re-arming a schedule script from inside an action is
            // FAIL-CLOSED. If the action uninstalled the script mid-run, the
            // epilogue below still records the executed step into the OLD
            // controller state (documented) — it never reactivates:
            // activation is governed solely by the TLS pair, which an
            // uninstall already cleared.
            std::function<void(sluice::async::Scheduler&)>* action = nullptr;
            {
                std::lock_guard<std::mutex> lk(st.mtx);
                if (step.payload >= st.action_count ||
                    !st.actions[step.payload]) {
                    schedule_script_config_fail(
                        "Invoke step references an unregistered action id",
                        step.payload);
                }
                action = &st.actions[step.payload];
            }
            t_schedule_invoke_active = true;
            (*action)(s);  // no script mutex, no production lock held
            t_schedule_invoke_active = false;
            {
                std::lock_guard<std::mutex> lk(st.mtx);
                if (st.executed_len < st.executed.size()) {
                    st.executed[st.executed_len++] = step;
                }
                ++st.next_step;
            }
            // An Invoke TERMINATES this hook visit (return nullptr): some
            // actions stage effects that become runnable only after the
            // worker's next drain (e.g. a staged fake-backend completion is
            // applied by wake_ready_completions, not by the staging call).
            // Ending the visit lets the normal pop -> drain -> FIFO path
            // publish those runnables; the next scripted Run is consumed at
            // the next pop-site visit. Run(X) is a CHOICE among >= 2 legal
            // runnables — a singleton publication is deterministic FIFO and
            // needs no script step.
            return nullptr;
        }
        // Run(id): remove the bound Fiber from THIS worker's deque, or fail
        // loudly. The pick holds inbox_mtx alone (leaf; the park predicate's
        // wake_mtx_ -> inbox_mtx edge is never inverted from here). The
        // success path allocates nothing; the failure diagnostic may.
        sluice::async::Fiber* chosen = nullptr;
        {
            std::lock_guard<std::mutex> lk(ws->inbox_mtx);
            if (step.payload < st.fiber_count) {
                sluice::async::Fiber* want = st.fiber_ptrs[step.payload];
                for (auto it = ws->local_runnable.begin();
                     it != ws->local_runnable.end(); ++it) {
                    if (*it == want) {
                        chosen = want;
                        ws->local_runnable.erase(it);
                        break;
                    }
                }
            }
        }
        if (chosen == nullptr) {
            // Failure diagnostic only: collect the legal set + step index.
            std::vector<sluice::async::Fiber*> legal;
            std::size_t failed_step = 0;
            {
                std::lock_guard<std::mutex> lk(ws->inbox_mtx);
                for (sluice::async::Fiber* f : ws->local_runnable) {
                    legal.push_back(f);
                }
            }
            {
                std::lock_guard<std::mutex> lk(st.mtx);
                failed_step = st.next_step;
            }
            schedule_script_fail(st, s, ws, failed_step, step.payload, legal);
        }
        {
            std::lock_guard<std::mutex> lk(st.mtx);
            if (st.executed_len < st.executed.size()) {
                st.executed[st.executed_len++] = step;
            }
            ++st.next_step;
        }
        return chosen;
    }
}

void install_schedule_script(sluice::async::Scheduler& s,
                             const ScheduleScriptInstall& plan) noexcept {
    // Review R3 P1: installing / re-arming a script from inside an Invoke
    // action would let the OLD script's post-action epilogue advance the NEW
    // script's step counter — silent deterministic-replay corruption. Fail
    // loudly instead (no generations, no nested schedules, no transactional
    // replacement; this PV driver has no such capability).
    if (t_schedule_invoke_active) {
        std::size_t step_index = 0;
        const char* current_test = nullptr;
        if (SchedulerController* c = find_controller(s); c != nullptr) {
            std::lock_guard<std::mutex> lk(c->schedule_script.mtx);
            step_index = c->schedule_script.next_step;
            current_test = c->schedule_script.test_name;
        }
        std::fprintf(stderr,
                     "\n=== DST-PV-1 SCHEDULE SCRIPT FAILURE ===\n"
                     "install/re-arm forbidden inside Invoke: "
                     "install_schedule_script is not allowed from inside an "
                     "Invoke action\n"
                     "current script: %s\n"
                     "step index:     %zu\n"
                     "======================================\n",
                     current_test != nullptr ? current_test : "<unnamed>",
                     step_index);
        std::fflush(stderr);
        std::abort();
    }
    // Fail loudly on capacity violations instead of silently truncating the
    // decision vector (review P1-4; DstScheduleDriver already validates at
    // append time — this is defense in depth for hand-built plans).
    if (plan.step_count > SchedulerController::kScheduleMaxSteps) {
        schedule_script_config_fail(
            "step_count exceeds kScheduleMaxSteps", plan.step_count);
    }
    if (plan.fiber_count > SchedulerController::kScheduleMaxFibers) {
        schedule_script_config_fail(
            "fiber_count exceeds kScheduleMaxFibers", plan.fiber_count);
    }
    if (plan.action_count > SchedulerController::kScheduleMaxActions) {
        schedule_script_config_fail(
            "action_count exceeds kScheduleMaxActions", plan.action_count);
    }
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    // find_controller released registry_mtx before we take the script mutex:
    // the controller object itself is retained until process teardown
    // (retired_controllers), so the pointer stays valid.
    {
        std::lock_guard<std::mutex> lk(c->schedule_script.mtx);
        SchedulerController::ScheduleScriptState& st = c->schedule_script;
        st.enabled = true;
        st.steps = plan.steps;
        st.step_count = plan.step_count;
        st.next_step = 0;
        st.fiber_ptrs = plan.fiber_ptrs;
        st.fiber_count = plan.fiber_count;
        st.actions = plan.actions;
        st.action_labels = plan.action_labels;
        st.action_count = plan.action_count;
        st.executed.fill(SchedulerController::ScheduleScriptStep{});
        st.executed_len = 0;
        st.test_name = plan.test_name;
        st.replay_vector = plan.replay_vector;
        st.failed = false;
        st.failed_step = 0;
        st.requested_id = 0;
    }
    // Activate on THIS thread only: the inline run(1) worker is the same
    // thread (the PV model). A multi-worker or cross-thread run never
    // activates the script (see schedule_script_active).
    t_active_script = ActiveScript{&s, &c->schedule_script};
}

void uninstall_schedule_script(sluice::async::Scheduler& s) noexcept {
    // Deactivate on this thread first so a same-thread gate cannot observe a
    // half-disabled script; the enabled flag below is defense in depth.
    if (t_active_script.sched == &s) {
        t_active_script = ActiveScript{};
    }
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    std::lock_guard<std::mutex> lk(c->schedule_script.mtx);
    c->schedule_script.enabled = false;
}

}  // namespace sluice_async_test
