// async_rwlock_death_test — E12-F AsyncRwLock fail-fast boundary tests.
//
// Verifies the two distinct fail-fast categories documented in
// docs/e12-rwlock.md (§"Illegal states / Category A vs Category B"):
//
// Category A — caller misuse / lifetime contract violation.
//   Reached through the PUBLIC API. Debug: assert() fires with a specific
//   diagnostic. Release: trusts the caller (no recovery semantics). These
//   cases are therefore DEBUG-ONLY: under NDEBUG the assertion is compiled
//   out and the behavior is undefined-by-contract.
//   EXCEPTION (ADR-async-primitive-lifetime-failfast): the DESTRUCTION cases
//   A4-A6 fail fast through named per-authority entries in BOTH Debug and
//   Release; only the non-destruction misuse cases A1-A3 stay Debug-only
//   (their death-test children are gated to !NDEBUG and are NOT asserted to
//   terminate in Release).
//
//   A1  unlock_read with zero active readers (underflow)
//   A2  unlock_write while unlocked (writer_active == false)
//   A3  unlock_write by non-owner Fiber
//   A4  destroy with active reader (active_readers_ > 0)
//   A5  destroy with active writer (writer_active_ == true)
//   A6  destroy with queued waiter (wait_queue_lifetime_fail_fast)
//
// Category B — internal linked-queue / authority corruption.
//   Debug: assert(false) + diagnostic. Release: deterministic fail-fast
//   (std::abort). Reached through a NARROWLY-SCOPED, guarded
//   AsyncTestAccess seam that constructs a corrupted linked node and then
//   invokes the SAME production grant path. The seam does NOT expose the
//   production WaitQueue authority: it is the only test entry that may
//   install a forged user_ on a linked node, and it routes through the
//   real rwlock_grant_from_head_locked so the fail-fast is the production
//   one, not a fake.
//
//   B1  grant_from_head on linked head with invalid mode
//   B2  grant_from_head on linked head with null user_
//   B3  reader_batch on linked node with invalid mode
//
// Control cases:
//   CTL valid usage completes, exit 0.
//
// Each case runs in a forked child that re-execs this binary with
// --death-child=<case>; the child installs a deterministic terminate handler
// and the parent asserts the exact exit code (death_test_runner_posix.hpp).
// POSIX-only: gated to linux/macosx in xmake.lua.
#include "death_test_runner_posix.hpp"

#if defined(__unix__)
#include "async_test_control.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/async_rwlock.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>

#include <csignal>
#include <cstddef>
#include <cstdlib>  // std::_Exit
#include <atomic>   // std::atomic
#include <memory>   // std::make_unique
#include <iostream>
#include <string>
#include <vector>

namespace {

using sluice::async::AsyncIoContext;
using sluice::async::AsyncRwLock;
using sluice::async::Fiber;
using sluice::async::FakeAsyncBackend;
using sluice::async::Scheduler;
using sluice::async::WaitNode;
using sluice::async::AsyncBackend;
using sluice::async::ReadOp;
using sluice::async::WriteOp;
using sluice::async::SyncDataOp;
using sluice::async::SyncAllOp;
using sluice::async::Completion;
using sluice::Result;
using AsyncTestAccess = Scheduler::AsyncTestAccess;

struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};

// A backend that never completes anything (the lock tests never need I/O).
class IdleBackend : public AsyncBackend {
public:
    Result<void> submit_read(ReadOp, Completion<std::size_t>&) override { return {}; }
    Result<void> submit_write(WriteOp, Completion<std::size_t>&) override { return {}; }
    Result<void> submit_sync_data(SyncDataOp, Completion<void>&) override { return {}; }
    Result<void> submit_sync_all(SyncAllOp, Completion<void>&) override { return {}; }
    std::size_t poll() override { return 0; }
    Result<std::size_t> wait_one() override { return 0; }
    void cancel(Completion<std::size_t>&) override {}
    void cancel(Completion<void>&) override {}
    std::size_t outstanding() const noexcept override { return 0; }
};

// ===========================================================================
// Category A — caller contract violations (PUBLIC API; DEBUG-only assertion)
// ===========================================================================
#if !defined(NDEBUG)
// A1 — unlock_read with zero active readers (underflow).
void child_a1_unlock_read_underflow() {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);
    // No read share held: unlock_read MUST assert active_readers_ > 0.
    rw.unlock_read();  // MUST terminate; never returns.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// A2 — unlock_write while unlocked.
void child_a2_unlock_write_while_unlocked() {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);
    // Requires a running Fiber for the owner check; spawn one that calls
    // unlock_write without first acquiring.
    Fiber f;
    f.set_entry([&](Fiber&) { rw.unlock_write(); });
    FiberStack s;
    if (!sched.init_fiber(f, s.base(), s.size())) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    sched.spawn(f);
    sched.run(1);  // MUST terminate inside unlock_write; never returns.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// A3 — unlock_write by non-owner Fiber.
void child_a3_unlock_write_non_owner() {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);
    // Owner acquires write lock on Fiber A, parks; non-owner Fiber B calls
    // unlock_write and MUST assert writer_owner_ != current.
    std::atomic<bool> owner_holds{false};
    std::atomic<bool> release{false};
    Fiber fa;
    fa.set_entry([&](Fiber&) {
        WaitNode wn;
        rw.write_lock(wn);
        owner_holds.store(true, std::memory_order_release);
        sched.await_ready_flag(release);
        // Parked forever (test terminates via B's unlock_write first).
    });
    Fiber fb;
    fb.set_entry([&](Fiber&) {
        sched.await_ready_flag(owner_holds);
        rw.unlock_write();  // MUST terminate (non-owner); never returns.
    });
    FiberStack sa, sb;
    if (!sched.init_fiber(fa, sa.base(), sa.size()) ||
        !sched.init_fiber(fb, sb.base(), sb.size())) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    sched.spawn(fa);
    sched.spawn(fb);
    sched.run(1);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// A4 — destroy with active reader.
//
// A single reader Fiber acquires a read share and then parks on a gate that
// is NEVER set before the destructor runs. At the moment of delete, the
// Fiber is still suspended holding its share, so active_readers_ > 0 and
// the destructor MUST assert and terminate. This matches the documented
// case: the Fiber is live and parked, not returned-and-cleaned-up.
#endif  // !defined(NDEBUG)

void child_a4_destroy_with_active_reader() {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock* rw = new AsyncRwLock(sched);

    std::atomic<bool> reader_holds{false};
    std::atomic<bool> release{false};  // never set before delete rw

    Fiber reader;
    reader.set_entry([&](Fiber&) {
        WaitNode rn;
        rw->read_lock(rn);
        reader_holds.store(true, std::memory_order_release);
        // Park while STILL holding the read share. The destructor aborts
        // before `release` is ever set, so unlock_read is unreachable.
        sched.await_ready_flag(release);
        rw->unlock_read();
    });
    FiberStack sr;
    if (!sched.init_fiber(reader, sr.base(), sr.size())) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    sched.spawn(reader);
    sched.run(1);
    // The reader Fiber is now parked holding its share (reader_holds == true).
    delete rw;  // MUST terminate (active_readers_ > 0); never returns.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// A5 — destroy with active writer.
void child_a5_destroy_with_active_writer() {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock* rw = new AsyncRwLock(sched);
    Fiber f;
    f.set_entry([&](Fiber&) {
        if (!rw->try_write_lock()) {
            std::_Exit(sluice_death_test::kChildTestFailExit);
        }
        // DO NOT unlock_write; let the destructor assert.
    });
    FiberStack s;
    if (!sched.init_fiber(f, s.base(), s.size())) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    sched.spawn(f);
    sched.run(1);
    delete rw;  // MUST terminate (writer_active_); never returns.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// A6 — destroy with queued waiter.
//
// The writer parks on a SEPARATE never-signalled gate (release_writer), NOT
// on writer_holds. The previous version parked on writer_holds which was
// already TRUE (just set on the line above), so await_ready_flag returned
// immediately and the writer fiber actually returned without parking — leaving
// writer_active_ false. A separate gate that is NEVER set keeps the writer
// genuinely parked while holding the write lock, so the subsequent queued
// reader exercises the ~WaitQueue head_ != nullptr assertion (A6), not the
// writer_active_ assertion (A5).
void child_a6_destroy_with_queued_waiter() {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock* rw = new AsyncRwLock(sched);
    std::atomic<bool> writer_holds{false};
    std::atomic<bool> release_writer{false};  // never set before delete rw
    Fiber wf;
    wf.set_entry([&](Fiber&) {
        WaitNode wn;
        rw->write_lock(wn);
        writer_holds.store(true, std::memory_order_release);
        // Park on a gate that is NEVER set. The writer remains suspended
        // holding the write lock for the entire test.
        sched.await_ready_flag(release_writer);
        rw->unlock_write();  // unreachable before destructor abort
    });
    FiberStack sw;
    if (!sched.init_fiber(wf, sw.base(), sw.size())) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    sched.spawn(wf);
    sched.run(1);
    // wf now holds the write lock and is parked. Queue a reader behind it.
    Fiber rf;
    rf.set_entry([&](Fiber&) {
        WaitNode rn;
        rw->read_lock(rn);  // queues behind the active writer
    });
    FiberStack sr;
    if (!sched.init_fiber(rf, sr.base(), sr.size())) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    sched.spawn(rf);
    sched.run(1);
    // Destroy with both an active writer AND a queued reader. Either the
    // writer_active_ assert (A5) or the ~WaitQueue head_ assert fires; both
    // are caller-contract violations and the child MUST terminate.
    delete rw;  // MUST terminate; never returns.
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);
}

// ===========================================================================
// Category B — internal invariant corruption (DEBUG assert + Release abort)
//
// These cases construct a CORRUPTED linked-node topology through the guarded
// AsyncTestAccess seam, then invoke the SAME production grant path. The seam
// does NOT expose WaitQueue structural authority to ordinary tests; it is
// the unique test entry permitted to forge user_ on a linked node, and it
// routes through rwlock_grant_from_head_locked so the fail-fast is real.
//
// In Release builds, Category B termination is guaranteed by std::abort;
// the assert is diagnostic only and may be compiled out under NDEBUG.
// ===========================================================================

// B1 — grant_from_head on linked head with INVALID mode (neither read nor write).
void child_b1_grant_invalid_head_mode() {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);
    // Build the corrupted topology through the guarded seam. The seam
    // registers one node at the head with a forged invalid-mode user_, then
    // calls the production grant path. The grant's switch default MUST abort.
    AsyncTestAccess::rwlock_death_forge_invalid_head_mode(sched, rw);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);  // unreachable
}

// B2 — grant_from_head on linked head with null user_.
void child_b2_grant_null_head_user() {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);
    AsyncTestAccess::rwlock_death_forge_null_head_user(sched, rw);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);  // unreachable
}

// B3 — reader_batch on a linked node with invalid mode (head is a valid
// reader, the SECOND node in the prefix has invalid mode).
void child_b3_reader_batch_invalid_mode() {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);
    AsyncTestAccess::rwlock_death_forge_invalid_batch_member(sched, rw);
    std::_Exit(sluice_death_test::kUnexpectedReturnExit);  // unreachable
}

// ===========================================================================
// Control case — valid usage completes, exit 0.
// ===========================================================================
void child_ctl_valid_usage() {
    AsyncIoContext ctx(std::make_unique<IdleBackend>());
    Scheduler sched(ctx);
    AsyncRwLock rw(sched);
    Fiber f;
    f.set_entry([&](Fiber&) {
        WaitNode rn, wn;
        rw.read_lock(rn);
        rw.unlock_read();
        rw.write_lock(wn);
        rw.unlock_write();
    });
    FiberStack s;
    if (!sched.init_fiber(f, s.base(), s.size())) {
        std::_Exit(sluice_death_test::kChildTestFailExit);
    }
    sched.spawn(f);
    sched.run(1);
    std::_Exit(0);
}

// dispatch_child never returns: every known case calls _Exit (either via the
// terminate handler on the fail-fast path, _Exit(0) for the control case,
// or _Exit(kUnexpectedReturnExit) if a must-terminate case wrongly returns).
void dispatch_child(const std::string& name) {
    sluice_death_test::install_deterministic_terminate_handler();
    // RwLock fail-fast boundaries use assert(false) + std::abort (Category B)
    // and assert() (Category A in Debug). std::abort raises SIGABRT; the
    // terminate handler above does NOT catch signals. Install a SIGABRT
    // handler that converts the signal to the deterministic terminate exit
    // code so the parent can assert it exactly (mirrors select_claim_death_
    // test / select_event_registry_death_test).
    std::signal(SIGABRT, [](int) noexcept {
        std::_Exit(sluice_death_test::kExpectedTerminateExit);
    });
#if !defined(NDEBUG)
    if      (name == "A1") child_a1_unlock_read_underflow();
    else if (name == "A2") child_a2_unlock_write_while_unlocked();
    else if (name == "A3") child_a3_unlock_write_non_owner();
#endif  // !defined(NDEBUG)
    // ADR-async-primitive-lifetime-failfast: the DESTRUCTION cases are
    // deterministic named fail-fast in BOTH Debug and Release.
    if      (name == "A4") child_a4_destroy_with_active_reader();
    else if (name == "A5") child_a5_destroy_with_active_writer();
    else if (name == "A6") child_a6_destroy_with_queued_waiter();
    if      (name == "B1") child_b1_grant_invalid_head_mode();
    else if (name == "B2") child_b2_grant_null_head_user();
    else if (name == "B3") child_b3_reader_batch_invalid_mode();
    else if (name == "CTL") child_ctl_valid_usage();
    std::cerr << "[death] unknown child case: " << name << "\n";
    std::_Exit(sluice_death_test::kChildTestFailExit);
}

int run_parent() {
    int failures = 0;
    const auto must_term = [&](const char* name) {
        auto r = sluice_death_test::run_death_case(name);
        if (!sluice_death_test::expect_terminated_via_fail_fast(r)) ++failures;
    };
    const auto must_zero = [&](const char* name) {
        auto r = sluice_death_test::run_death_case(name);
        if (!sluice_death_test::expect_normal_exit_zero(r)) ++failures;
    };

#if !defined(NDEBUG)
    // Category A (non-destruction misuse): DEBUG-only (Release compiles out
    // the assertions and trusts the caller per the design contract).
    must_term("A1");  // unlock_read underflow
    must_term("A2");  // unlock_write while unlocked
    must_term("A3");  // unlock_write by non-owner
#endif  // !defined(NDEBUG)
    // ADR-async-primitive-lifetime-failfast: destruction cases are named
    // fail-fast in BOTH Debug and Release (A6 also exercises
    // wait_queue_lifetime_fail_fast via ~WaitQueue).
    must_term("A4");  // destroy with active reader
    must_term("A5");  // destroy with active writer
    must_term("A6");  // destroy with queued waiter
    // Category B: deterministic fail-fast in BOTH Debug and Release.
    must_term("B1");  // invalid head mode
    must_term("B2");  // null head user_
    must_term("B3");  // invalid batch member mode
    must_zero("CTL");  // control: valid usage exits 0

    if (failures == 0) {
        std::cout << "ALL DEATH TESTS PASSED\n";
        return 0;
    }
    std::cout << failures << " death-test case(s) FAILED\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string child_case = sluice_death_test::parse_child_case(argc, argv);
    if (!child_case.empty()) {
        dispatch_child(child_case);  // never returns
        return sluice_death_test::kChildTestFailExit;  // unreachable
    }
    return run_parent();
}

#else  // !defined(__unix__)

#include <iostream>

int main() {
    std::cout << "async_rwlock_death_test: NOT RUN on this platform "
                 "(POSIX fork/exec harness only; see death_test_runner_posix.hpp)\n";
    return 0;
}

#endif  // defined(__unix__)
