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
        p.paused = p.armed &&
                   (p.armed_worker == kAnyWorker || p.armed_worker == worker_id);
    }
    p.cv.notify_all();  // tell the coordinator we reached + paused
    {
        std::unique_lock<std::mutex> lk(p.mtx);
        if (p.paused) {
            p.cv.wait(lk, [&p] { return !p.armed; });
            p.paused = false;
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

}  // namespace sluice_async_test
