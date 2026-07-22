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

#include <mutex>
#include <iterator>
#include <unordered_map>

namespace sluice_async_test {

namespace {

// The registry. Mutated only by register_controller/unregister_controller
// (test setup/teardown). test_phase performs a find() (no insertion).
// Guarded by registry_mtx.
std::mutex registry_mtx;
std::unordered_map<sluice::async::Scheduler*, SchedulerController> registry;

// Look up the controller for `s` WITHOUT allocating. Returns nullptr if not
// registered. Called on the phase hot path.
SchedulerController* find_controller(sluice::async::Scheduler& s) noexcept {
    // lock_guard: find() must not race a concurrent unregister. This is the
    // only lock taken on the hot path; it is held only for the lookup, then
    // released before any phase-state block.
    std::lock_guard<std::mutex> lk(registry_mtx);
    auto it = registry.find(&s);
    return it == registry.end() ? nullptr : &it->second;
}

PhaseState& phase_of(SchedulerController& c, PhaseTag tag) noexcept {
    return c.phases[static_cast<std::size_t>(tag)];
}

}  // namespace

void test_phase(sluice::async::Scheduler& s, PhaseTag tag) noexcept {
    // No allocation: find only. If the Scheduler has no registered controller
    // (e.g. a variant-lib path hit during a non-test run), this is a no-op.
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    PhaseState& p = phase_of(*c, tag);
    // Mark reached + optionally block. We acquire the phase's own mtx (NOT a
    // production lock) so the test coordinator can observe/ release. When the
    // phase is armed, we block here until release — the production lock held
    // by the caller (e.g. global_mtx_) remains held, which is the guarantee
    // under test.
    {
        std::lock_guard<std::mutex> lk(p.mtx);
        p.reached = true;
        p.paused = p.armed;
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

void release_all_phases(sluice::async::Scheduler& s) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    // Disarm every phase so any paused worker observes termination.
    for (std::size_t i = 0; i < std::size(c->phases); ++i) {
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
    registry.try_emplace(&s);
}

void unregister_controller(sluice::async::Scheduler& s) noexcept {
    std::lock_guard<std::mutex> lk(registry_mtx);
    registry.erase(&s);
}

void arm(sluice::async::Scheduler& s, PhaseTag tag) noexcept {
    SchedulerController* c = find_controller(s);
    if (c == nullptr) return;
    PhaseState& p = phase_of(*c, tag);
    {
        std::lock_guard<std::mutex> lk(p.mtx);
        p.armed = true;
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
        if (tag == PhaseTag::e13_admission_armed) {
            c->admission_armed_snapshot = snap;
        } else if (tag == PhaseTag::e13_admission_consumed) {
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
    if (tag == PhaseTag::e13_admission_armed) {
        return c->admission_armed_snapshot;
    } else if (tag == PhaseTag::e13_admission_consumed) {
        return c->admission_consumed_snapshot;
    }
    return AdmissionSnapshot{};
}

}  // namespace sluice_async_test
