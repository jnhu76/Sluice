// dst_schedule_driver.hpp — DST-PV-1 deterministic schedule driver (TEST-ONLY).
//
// A thin test-side layer over the internal-testing schedule-script seam
// (tests/async_test_control_internal.hpp + the guarded worker_loop pop-site
// hook). It lets a test express a deterministic execution as an explicit
// decision vector over already-runnable Fibers and the EXISTING controllable
// seams (fake/scripted I/O completion, advance_clock, cancel_wait), rerun the
// exact same vector, and get a deterministic diagnostic when a decision is
// illegal.
//
// This is NOT a DST framework: no search, no seeds, no random scheduler, no
// permutation exploration, no trace database. The only genuinely new
// primitive is Run(X) — "which already-runnable Fiber the single worker
// dequeues next"; every other action reuses a seam the repository already
// exposes. Scope: inline run(1) (driver and worker are the same thread);
// install strictly before sched.run().
//
// Identity: participants are small test-assigned integers bound to Fiber
// objects BEFORE the run. Replay vectors reference only these ids — never
// Fiber addresses. The ids are rendered as letters (Run(A) == id 0) in the
// vector text. Scope: inline run(1) (driver and worker are the same thread);
// install strictly before sched.run() ON THE SAME THREAD — a multi-worker or
// cross-thread run never activates the script. Capacities are fixed (64
// steps, 8 fibers, 8 actions) and every append is bounds-checked: an
// out-of-range id or a full script fails loudly instead of corrupting
// memory or silently truncating the vector.
#pragma once

#include "async_test_control_internal.hpp"

#include <sluice/async/scheduler.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace sluice_dst {

class DstScheduleDriver {
public:
    DstScheduleDriver(sluice::async::Scheduler& s, const char* test_name)
        : s_(s) {
        plan_.test_name = test_name;
    }

    // Uninstall the script (idempotent; the Scheduler must outlive the
    // driver, as with every other test-control object).
    ~DstScheduleDriver() { sluice_async_test::uninstall_schedule_script(s_); }

    DstScheduleDriver(const DstScheduleDriver&) = delete;
    DstScheduleDriver& operator=(const DstScheduleDriver&) = delete;

    // Bind a participant identity (0..7) to a Fiber BEFORE arming. Replay
    // vectors use only this id. An out-of-range id fails loudly (never a
    // silent out-of-bounds write — review P1-4).
    DstScheduleDriver& bind(unsigned char id, sluice::async::Fiber& f) {
        if (id >= kMaxFibers) {
            fail("bind: participant id out of range", id);
        }
        plan_.fiber_ptrs[id] = &f;
        if (id >= plan_.fiber_count) plan_.fiber_count = id + 1;
        return *this;
    }

    // Register an action closure for Invoke steps. The closure reuses the
    // repository's existing controllable seams (backend completion,
    // advance_clock, cancel_wait); the label appears in the replay vector and
    // the failure diagnostic. An out-of-range id fails loudly.
    DstScheduleDriver& on_action(unsigned char action_id, const char* label,
                                 std::function<void(sluice::async::Scheduler&)> fn) {
        if (action_id >= kMaxActions) {
            fail("on_action: action id out of range", action_id);
        }
        plan_.actions[action_id] = std::move(fn);
        plan_.action_labels[action_id] = label;
        if (action_id >= plan_.action_count) {
            plan_.action_count = action_id + 1;
        }
        return *this;
    }

    // Append a Run decision: the bound Fiber must be already-runnable when
    // the worker reaches this step, else the run aborts with the
    // deterministic diagnostic (no silent fallback). The script capacity and
    // the id are checked here, at append time.
    DstScheduleDriver& run(unsigned char fiber_id) {
        if (fiber_id >= kMaxFibers) {
            fail("run: fiber id out of range", fiber_id);
        }
        append_step(StepKind::run, fiber_id);
        replay_ += replay_.empty() ? "" : " -> ";
        replay_ += "Run(";
        replay_ += render_id(fiber_id);
        replay_ += ")";
        return *this;
    }

    // Append an Invoke decision: execute the registered action INLINE at the
    // decision point (no scheduler lock, no script mutex held). The action id
    // is validated BEFORE any label/array access, and the script capacity is
    // checked at append time.
    DstScheduleDriver& invoke(unsigned char action_id) {
        if (action_id >= kMaxActions) {
            fail("invoke: action id out of range", action_id);
        }
        append_step(StepKind::invoke, action_id);
        replay_ += replay_.empty() ? "" : " -> ";
        replay_ += plan_.action_labels[action_id] != nullptr
                       ? plan_.action_labels[action_id]
                       : "<action>";
        return *this;
    }

    // Semantic note for the test's own trace (assertions + replay evidence).
    DstScheduleDriver& note(std::string event) {
        trace_.push_back(std::move(event));
        return *this;
    }
    const std::vector<std::string>& trace() const noexcept { return trace_; }

    // The decision vector text (e.g. "Run(A) -> Io(4) -> Clock(+20) -> Run(A)").
    const std::string& replay_vector() const noexcept { return replay_; }

    // Install the plan into the Scheduler's controller. Call after the plan
    // is complete and BEFORE sched.run(). register_controller must already
    // have run for this Scheduler.
    void arm() { install_schedule_script(s_, plan_); }

private:
    using StepKind =
        sluice_async_test::SchedulerController::ScheduleScriptStep::Kind;
    using Install = sluice_async_test::ScheduleScriptInstall;

    static constexpr std::size_t kMaxSteps =
        sluice_async_test::SchedulerController::kScheduleMaxSteps;
    static constexpr std::size_t kMaxFibers =
        sluice_async_test::SchedulerController::kScheduleMaxFibers;
    static constexpr std::size_t kMaxActions =
        sluice_async_test::SchedulerController::kScheduleMaxActions;

    // A test-only driver must fail loudly on capacity violations, never
    // silently truncate the decision vector or index out of bounds (the
    // review P1-4 contract). The abort is deliberate: it is the same
    // fail-fast contract the illegal-decision pick uses.
    [[noreturn]] static void fail(const char* what, unsigned long value) {
        std::fprintf(stderr,
                     "\n=== DST SCHEDULE DRIVER FAILURE (capacity) ===\n"
                     "%s: %lu\n"
                     "capacities: %zu steps, %zu fibers, %zu actions\n"
                     "===================================================\n",
                     what, value, kMaxSteps, kMaxFibers, kMaxActions);
        std::fflush(stderr);
        std::abort();
    }

    void append_step(StepKind kind, unsigned char payload) {
        if (plan_.step_count >= kMaxSteps) {
            fail("script exceeds kScheduleMaxSteps", plan_.step_count);
        }
        plan_.steps[plan_.step_count++] = {kind, payload};
    }

    static std::string render_id(unsigned char id) {
        if (id < 26) {
            return std::string(1, static_cast<char>('A' + id));
        }
        return std::to_string(static_cast<unsigned>(id));
    }

    sluice::async::Scheduler& s_;
    Install plan_{};
    std::string replay_;
    std::vector<std::string> trace_;
};

}  // namespace sluice_dst
