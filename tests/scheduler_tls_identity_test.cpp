// scheduler_tls_identity_test — direct verification of the R1 split's ONE
// cross-TU linkage adaptation (post-freeze review, PR #114).
//
// scheduler.cpp was split into concept TUs; the moved function bodies are
// byte-identical, but g_worker changed linkage FORM: pre-split it was a
// single-TU anonymous-namespace thread_local; post-split it is
// `inline thread_local` in src/async/scheduler_internal.hpp shared by every
// scheduler implementation TU. The motion certificate cannot cover this
// line — it is new glue (#113 died in new glue), so this test verifies the
// identity contract directly:
//
//   * CROSS-TU STORAGE IDENTITY — a write through the header entity in THIS
//     translation unit is observed through probe(),
//     whose definition is compiled in src/async/scheduler.cpp. If the
//     inline variable were not merged into one per-thread entity, the
//     scheduler TU would observe nullptr/stale storage and this check
//     fails.
//   * PER-THREAD ISOLATION — a sibling OS thread starts at nullptr and its
//     writes never leak into this thread (thread_local, not a global slot).
//
// TEST-ONLY placement: this target links sluice_async_internal_testing and
// adds src/async to its include path purely to reach the non-installed
// scheduler_internal.hpp. Production headers and behavior are untouched.
#include "harness.hpp"

#include "scheduler_internal.hpp"

#include <atomic>
#include <new>
#include <thread>

using namespace sluice::async;

SLUICE_TEST_CASE(scheduler_g_worker_cross_tu_identity) {
    // Pointer identity only — never dereferenced, so raw aligned storage
    // avoids constructing a full WorkerState.
    alignas(alignof(WorkerState)) unsigned char buf_a[sizeof(WorkerState)];
    alignas(alignof(WorkerState)) unsigned char buf_b[sizeof(WorkerState)];
    WorkerState* pa = reinterpret_cast<WorkerState*>(buf_a);
    WorkerState* pb = reinterpret_cast<WorkerState*>(buf_b);

    // Cross-TU observation point: tls_worker_probe() reads g_worker inside
    // Scheduler::current_worker(), compiled in src/async/scheduler.cpp.
    auto probe = [] { return Scheduler::AsyncTestAccess::tls_worker_probe(); };

    // Baseline: a non-worker thread observes the null initial value.
    SLUICE_CHECK(sluice::async::g_worker == nullptr);
    SLUICE_CHECK(probe() == nullptr);

    // Cross-TU identity: write via THIS TU's view of the inline variable,
    // read back through the scheduler.cpp-compiled accessor.
    g_worker = pa;
    SLUICE_CHECK(probe() == pa);
    g_worker = pb;
    SLUICE_CHECK(probe() == pb);

    // Per-thread isolation: sibling thread starts null, its write is
    // visible to itself (through the same cross-TU accessor), and does not
    // leak into THIS thread.
    std::atomic<bool> sibling_saw_null{false};
    std::atomic<bool> sibling_roundtrip{false};
    std::thread sibling([&] {
        sibling_saw_null = (probe() == nullptr);
        g_worker = pb;
        sibling_roundtrip = (probe() == pb);
        g_worker = nullptr;
    });
    sibling.join();

    SLUICE_CHECK(sibling_saw_null.load(std::memory_order_acquire));
    SLUICE_CHECK(sibling_roundtrip.load(std::memory_order_acquire));
    // This thread's slot still holds pb — the sibling's writes (pb then
    // nullptr) landed in ITS OWN per-thread slot, never in this one.
    SLUICE_CHECK(probe() == pb);

    g_worker = nullptr;
    SLUICE_CHECK(probe() == nullptr);
}

SLUICE_MAIN()
