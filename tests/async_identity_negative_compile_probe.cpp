// async_identity_negative_compile_probe.cpp
//
// E16-POST-MERGE-CORRECTIVE-1 (C2-T4) — installed-header authority audit.
//
// This probe is driven by scripts/verify-async-identity-negative-compile.sh.
// Each NEG_<KIND> macro selects ONE forbidden usage of the Fiber-local Runtime
// execution-tag setter. With no macro defined, the file compiles cleanly
// (positive control). With exactly one macro defined, compilation MUST fail
// with a private/deleted-member diagnostic: the write authority must remain
// reachable only by ApplicationRuntime (the friend grant), never by ordinary
// application code.
//
// The probe deliberately does NOT link (it is -fsyntax-only) and does NOT
// instantiate a running Runtime: it only asserts the COMPILE-TIME visibility
// of the setter from ordinary (non-friend) code.
#include <sluice/async/fiber.hpp>
#include <sluice/async/scheduler.hpp>

using namespace sluice::async;

// Ordinary (non-friend) context. None of these functions are members of
// ApplicationRuntime or Scheduler, so a private setter must be unreachable.
void ordinary_clear_fiber_tag(Fiber& f) {
#ifdef NEG_FIBER_SET_EXECUTION_TAG
    // Forbidden: ordinary code clearing/setting the Fiber identity tag.
    f.set_execution_tag(nullptr);
#else
    (void)f;
#endif
}

void ordinary_set_fiber_tag_value(Fiber& f, void* v) {
#ifdef NEG_FIBER_SET_EXECUTION_TAG_VALUE
    // Forbidden: ordinary code forging an arbitrary tag value.
    f.set_execution_tag(v);
#else
    (void)f;
    (void)v;
#endif
}

void ordinary_scheduler_set_tag() {
#ifdef NEG_SCHEDULER_SET_CURRENT_FIBER_TAG
    // Forbidden: ordinary code reaching the Scheduler's private tag setter.
    Scheduler::set_current_fiber_execution_tag(nullptr);
#else
#endif
}

int main() { return 0; }
