// Phase C2c — ThreadPoolBackend borrow / waiter / delivery-lease integration.
//
// Issue #68 rows 11-14 at the PRODUCTION blocking-I/O backend layer (Layer B):
//   row 11  — the running window (worker paused between mark_running and the
//             syscall) shows the borrow active with the EXACT submitted
//             fd/address/length; running cancel intent does NOT end the
//             borrow; the backend_ready window (syscall finished, terminal
//             recorded, not yet reaped) STILL shows the borrow active — a
//             worker finishing its syscall is NOT the borrow lifetime end;
//             only reap ends the borrow.
//   row 12a — a waiter registered through the real arena authority while the
//             op is enqueued survives the enqueue -> running -> backend_ready
//             transitions and is delivered exactly once at reap; registration
//             is ALSO legal while running (Gate C) and at backend_ready
//             (terminal recorded, not yet reaped) — registration is
//             orthogonal to execution state, only reap closes it (ADR
//             Decision 10).
//   row 13  — wait-cancel removes ONLY the waiter (the real syscall still
//             executes and its real result wins); enqueued I/O cancel keeps
//             the waiter (canceled result + waiter delivered together).
//   row 14a — the production sink receives the registered token + lease
//             exactly once; a stale-generation waiter authority resolves to
//             not_found against a live N+1 occupant with zero side effect.
//
// All windows are deterministic pause gates (no sleep_for): the same
// SLUICE_ASYNC_INTERNAL_TESTING gates the Phase E race tests use. All waits
// are bounded; on failure every armed gate is resumed and drained before the
// failure is reported (issue #68 §13 fail-path discipline).
#include "harness.hpp"

#include "support/waiter_error_vocabulary_cases.hpp"

#include <sluice/async/completion.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/error.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;
using sluice::async::detail::WaiterRegistration;
using sluice::async::detail::WaiterToken;

SLUICE_MAIN()

namespace {

constexpr auto kWaitTimeout = std::chrono::seconds(5);

class TempPath {
public:
    TempPath(const char* tag) {
        path_ = (std::filesystem::temp_directory_path() /
                 ("sluice_tp_c2c_" + std::string(tag) + "_" +
                  std::to_string(::getpid()) + "_" +
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

int open_temp(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "open_temp failed\n");
        std::exit(1);
    }
    return fd;
}

// Wait for a gate's paused flag with a bounded deadline. Returns true on success.
template <class Gate>
bool wait_paused(Gate& gate, std::chrono::steady_clock::time_point deadline) {
    while (!gate.paused.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

// Drain outstanding ops through the real reaper with a bounded total time.
bool drain_bounded(ThreadPoolBackend& backend,
                   std::chrono::steady_clock::time_point deadline) {
    while (backend.outstanding() > 0) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        if (backend.poll() == 0) {
            std::this_thread::yield();
        }
    }
    return true;
}

// Wait for the production path to leave a gate (bounded; abort on timeout).
template <class Gate>
void wait_gate_exit(Gate& gate, const char* case_name) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (!gate.exited.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr,
                         "%s: gate failed to exit before timeout; aborting\n",
                         case_name);
            std::abort();
        }
        std::this_thread::yield();
    }
}

// RAII: resume a paused gate on scope exit and wait for the production path to
// leave it (the gate object must outlive the backend — lexical scope).
template <class Gate>
class ScopedGateResume {
public:
    ScopedGateResume(Gate& gate)
        : gate_(&gate) {}
    void resume() {
        if (resumed_) return;
        resume_threadpool_gate(*gate_);
        resumed_ = true;
    }
    ~ScopedGateResume() { cleanup(); }
private:
    void cleanup() {
        resume();
        wait_gate_exit(*gate_, "ThreadPool C2c");
    }
    Gate* gate_;
    bool resumed_ = false;
};

}  // namespace

// ---- Row 11/12a/13 (ThreadPool): running borrow + cancel intent + waiter -----
// Gate B holds the op `enqueued` so the waiter registers through the real
// arena authority deterministically; Gate C then holds the worker in the
// RUNNING window. In that window: the borrow is active with the exact
// submitted fd/address/length, the registered waiter is still registered, and
// running cancel intent neither ends the borrow nor cancels the waiter. The
// syscall's REAL result then wins verbatim; the borrow stays active through
// backend_ready and only reap ends it; the waiter is delivered exactly once.
SLUICE_TEST_CASE(tp_running_borrow_cancel_intent_waiter_survives) {
    ThreadPoolBackend::BeforeWorkerDequeuePauseGate gate_b;
    ThreadPoolBackend::WorkerRunningPauseGate gate_c;
    // Gates must outlive the backend (reverse declaration order).
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_before_dequeue_pause_gate(&gate_b);
    backend.set_running_pause_gate(&gate_c);
    ScopedGateResume guard_b(gate_b);
    ScopedGateResume guard_c(gate_c);
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    TempPath tp("R");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0xAB}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;

    // Window 1: op enqueued (worker paused before dequeue). Register the
    // waiter through the REAL arena authority; the borrow is already active.
    if (!wait_paused(gate_b, deadline)) {
        fail_msg = "Gate B did not pause in time";
    } else {
        auto h = backend.handle_for_completion_for_test(&c);
        if (!h.has_value()) {
            fail_msg = "handle_for_completion_for_test must find the bound Completion";
        } else {
            auto b = backend.borrow_for_test(*h);
            if (!b.has_value()) {
                fail_msg = "borrow_for_test must validate the live handle";
            } else if (!b->active || b->fd != fd || b->address != buf ||
                       b->length != 1) {
                fail_msg = "enqueued borrow must be active with exact fd/addr/len";
            } else if (!backend.register_waiter_for_test(c, WaiterToken{1, 7, 3},
                                                         detail::RoutingLease{99})
                            .has_value()) {
                fail_msg = "waiter registration on the enqueued op must succeed";
            }
        }
    }

    // Window 2: worker RUNNING (paused between mark_running and the syscall).
    if (fail_msg == nullptr) {
        guard_b.resume();
        if (!wait_paused(gate_c, deadline)) {
            fail_msg = "Gate C did not pause in time";
        } else {
            auto h = backend.handle_for_completion_for_test(&c);
            auto b = h.has_value() ? backend.borrow_for_test(*h)
                                   : std::optional<detail::RequestArena::BorrowSnapshot>{};
            auto w = h.has_value() ? backend.waiter_for_test(*h)
                                   : std::optional<detail::RequestArena::WaiterObservation>{};
            if (!h.has_value() || !b.has_value() || !w.has_value()) {
                fail_msg = "running handle must still observe borrow + waiter";
            } else if (!b->active || b->fd != fd || b->address != buf ||
                       b->length != 1) {
                fail_msg = "running borrow must still be active with exact metadata";
            } else if (w->registration != WaiterRegistration::open_registered ||
                       w->token != WaiterToken{1, 7, 3} || w->lease_id != 99) {
                fail_msg = "waiter must survive the enqueued -> running transition";
            } else if (backend.active_workers_for_test() != 1) {
                fail_msg = "exactly one worker must be running";
            } else {
                // Running cancel records INTENT only (the arena disposition is
                // intent_recorded; the public cancel(Completion&) returns void).
                backend.cancel(c);
                if (stats.canceled_ops != 0) {
                    fail_msg = "running cancel intent must NOT tally canceled_ops";
                } else if (c.ready()) {
                    fail_msg = "Completion must not be ready in the running window";
                } else {
                    auto b2 = backend.borrow_for_test(*h);
                    if (!b2.has_value() || !b2->active) {
                        fail_msg = "running cancel intent must not end the borrow";
                    } else {
                        auto w2 = backend.waiter_for_test(*h);
                        if (!w2.has_value() ||
                            w2->registration != WaiterRegistration::open_registered ||
                            w2->token != WaiterToken{1, 7, 3} ||
                            w2->lease_id != 99) {
                            fail_msg =
                                "running cancel intent must not delete the waiter";
                        }
                    }
                }
            }
        }
    }

    // Window 3: backend_ready (syscall finished, terminal recorded, not yet
    // reaped). The borrow is STILL active — a worker finishing its syscall is
    // NOT the borrow lifetime end; only reap releases the borrow.
    if (fail_msg == nullptr) {
        guard_c.resume();
        const auto br_deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (backend.backend_ready_count_for_test() == 0) {
            if (std::chrono::steady_clock::now() >= br_deadline) {
                fail_msg = "terminal was not recorded before timeout";
                break;
            }
            std::this_thread::yield();
        }
    }
    if (fail_msg == nullptr) {
        auto h = backend.handle_for_completion_for_test(&c);
        auto b = h.has_value() ? backend.borrow_for_test(*h)
                               : std::optional<detail::RequestArena::BorrowSnapshot>{};
        if (!h.has_value() || !b.has_value()) {
            fail_msg = "backend_ready handle must still observe the borrow";
        } else if (!b->active) {
            fail_msg = "borrow must STILL be active at backend_ready (worker "
                       "finishing the syscall != borrow lifetime end)";
        } else if (c.ready()) {
            fail_msg = "Completion must not be ready before reap publishes";
        }
    }

    // Reap: real result wins verbatim; borrow ends; waiter delivered exactly
    // once; canceled_ops stays 0.
    if (fail_msg == nullptr) {
        if (!drain_bounded(backend,
                           std::chrono::steady_clock::now() + kWaitTimeout)) {
            fail_msg = "drain did not complete in time";
        } else if (!c.ready()) {
            fail_msg = "op must complete after resume";
        } else if (!c.result().has_value() || c.result().value() != 1) {
            fail_msg = "real syscall result must win verbatim";
        } else if (backend.syscall_count_for_test() != 1) {
            fail_msg = "exactly one syscall must have executed";
        } else if (backend.sink_deliveries() != 1 ||
                   !backend.sink_last_has_waiter() ||
                   backend.sink_last_token() != WaiterToken{1, 7, 3} ||
                   backend.sink_last_lease_id() != 99) {
            fail_msg = "waiter must be delivered exactly once with token A + lease 99";
        } else if (stats.canceled_ops != 0) {
            fail_msg = "intent must never tally canceled_ops";
        }
    }

    if (fail_msg == nullptr) {
        auto h = backend.handle_for_completion_for_test(&c);
        if (h.has_value()) {
            auto b = backend.borrow_for_test(*h);
            if (!b.has_value() || b->active) {
                fail_msg = "borrow must be ended after reap";
            }
        } else {
            fail_msg = "ready Completion must still resolve until reset";
        }
    }

    if (fail_msg == nullptr) {
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        guard_b.resume();
        guard_c.resume();
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kWaitTimeout);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- Row 11 (ThreadPool): backend_ready-before-reap borrow still active -----
// No gate needed: the test catches the exact window where the worker finished
// the syscall and record_terminal stored the backend_ready terminal but no
// poll/reap has run. The borrow is STILL active and the Completion is NOT
// ready; reap then ends the borrow and publishes ready. This is the
// "worker finishing syscall != borrow lifetime end" proof on the real backend.
SLUICE_TEST_CASE(tp_backend_ready_borrow_still_active_before_reap) {
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});

    TempPath tp("B");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0xCD}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (backend.backend_ready_count_for_test() == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            fail_msg = "terminal was not recorded before timeout";
            break;
        }
        std::this_thread::yield();
    }

    if (fail_msg == nullptr) {
        auto h = backend.handle_for_completion_for_test(&c);
        auto b = h.has_value() ? backend.borrow_for_test(*h)
                               : std::optional<detail::RequestArena::BorrowSnapshot>{};
        if (!h.has_value() || !b.has_value()) {
            fail_msg = "backend_ready handle must observe the borrow";
        } else if (!b->active || b->fd != fd || b->address != buf || b->length != 1) {
            fail_msg = "borrow must still be active with exact metadata at backend_ready";
        } else if (c.ready()) {
            fail_msg = "Completion must NOT be ready before reap publishes";
        } else if (backend.syscall_count_for_test() != 1) {
            fail_msg = "exactly one syscall must have executed";
        }
    }

    if (fail_msg == nullptr) {
        SLUICE_CHECK(backend.poll() == 1);
        SLUICE_CHECK(c.ready());
        SLUICE_CHECK(c.result().has_value());
        SLUICE_CHECK(c.result().value() == 1);
        auto h = backend.handle_for_completion_for_test(&c);
        auto b = h.has_value() ? backend.borrow_for_test(*h)
                               : std::optional<detail::RequestArena::BorrowSnapshot>{};
        SLUICE_CHECK(h.has_value() && b.has_value() && !b->active);
        SLUICE_CHECK(backend.poll() == 0);  // exactly-once publication
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kWaitTimeout);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- Row 12a (ThreadPool): registration in the RUNNING window ---------------
// Gate C holds the worker between mark_running and the syscall. Per ADR
// Decision 10, registration is ORTHOGONAL to execution state: a waiter
// registered while the op is RUNNING succeeds and reap delivers it exactly
// once together with the syscall's real result.
SLUICE_TEST_CASE(tp_running_window_waiter_registration) {
    ThreadPoolBackend::WorkerRunningPauseGate gate_c;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_running_pause_gate(&gate_c);
    ScopedGateResume guard_c(gate_c);

    TempPath tp("RC");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x56}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;

    // Worker RUNNING (paused between mark_running and the syscall).
    if (!wait_paused(gate_c, deadline)) {
        fail_msg = "Gate C did not pause in time";
    } else {
        auto h = backend.handle_for_completion_for_test(&c);
        if (!h.has_value()) {
            fail_msg = "handle_for_completion_for_test must find the bound Completion";
        } else if (!backend.register_waiter_for_test(c, WaiterToken{5, 4, 2},
                                                     detail::RoutingLease{77})
                        .has_value()) {
            fail_msg = "waiter registration in the running window must succeed "
                       "(ADR Decision 10)";
        } else {
            auto w = backend.waiter_for_test(*h);
            if (!w.has_value() ||
                w->registration != WaiterRegistration::open_registered ||
                w->token != WaiterToken{5, 4, 2} || w->lease_id != 77) {
                fail_msg = "running-window registration must be stored exactly";
            }
        }
    }

    // Resume: the real syscall executes and its result wins verbatim; the
    // running-window waiter is delivered exactly once at reap.
    if (fail_msg == nullptr) {
        guard_c.resume();
        if (!drain_bounded(backend,
                           std::chrono::steady_clock::now() + kWaitTimeout)) {
            fail_msg = "drain did not complete in time";
        } else if (!c.ready() || !c.result().has_value() ||
                   c.result().value() != 1) {
            fail_msg = "the real syscall result must win verbatim";
        } else if (backend.sink_deliveries() != 1 ||
                   !backend.sink_last_has_waiter() ||
                   backend.sink_last_token() != WaiterToken{5, 4, 2} ||
                   backend.sink_last_lease_id() != 77) {
            fail_msg = "running-window waiter must be delivered exactly once";
        }
    }

    if (fail_msg == nullptr) {
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        guard_c.resume();
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kWaitTimeout);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- Row 12a (ThreadPool): registration in the backend_ready window ---------
// The worker finished the syscall and record_terminal stored the terminal,
// but no poll/reap has run. The terminal winner does NOT close registration
// (ADR Decision 10): a waiter registered in this window succeeds and reap
// delivers it exactly once with the real result.
SLUICE_TEST_CASE(tp_backend_ready_window_waiter_registration) {
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});

    TempPath tp("BD");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x78}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (backend.backend_ready_count_for_test() == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            fail_msg = "terminal was not recorded before timeout";
            break;
        }
        std::this_thread::yield();
    }

    if (fail_msg == nullptr) {
        auto h = backend.handle_for_completion_for_test(&c);
        if (!h.has_value()) {
            fail_msg = "backend_ready handle must resolve";
        } else if (c.ready()) {
            fail_msg = "Completion must NOT be ready before reap publishes";
        } else if (!backend.register_waiter_for_test(c, WaiterToken{6, 5, 3},
                                                     detail::RoutingLease{88})
                        .has_value()) {
            fail_msg = "waiter registration in the backend_ready window must "
                       "succeed (ADR Decision 10)";
        } else {
            auto w = backend.waiter_for_test(*h);
            if (!w.has_value() ||
                w->registration != WaiterRegistration::open_registered ||
                w->token != WaiterToken{6, 5, 3} || w->lease_id != 88) {
                fail_msg = "backend_ready-window registration must be stored exactly";
            }
        }
    }

    // Reap: the real result + the backend_ready-window waiter, exactly once.
    if (fail_msg == nullptr) {
        SLUICE_CHECK(backend.poll() == 1);
        SLUICE_CHECK(c.ready());
        SLUICE_CHECK(c.result().has_value());
        SLUICE_CHECK(c.result().value() == 1);
        SLUICE_CHECK(backend.poll() == 0);  // exactly-once publication
        SLUICE_CHECK(backend.sink_deliveries() == 1);
        SLUICE_CHECK(backend.sink_last_has_waiter());
        SLUICE_CHECK((backend.sink_last_token() == WaiterToken{6, 5, 3}));
        SLUICE_CHECK(backend.sink_last_lease_id() == 88);
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kWaitTimeout);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- Row 13 (ThreadPool): wait-cancel removes ONLY the waiter ---------------
// Gate B holds the op enqueued; cancel_waiter returns the lease and reopens
// registration while the I/O stays accepted with its borrow active. After
// resume the REAL syscall still executes (wait-cancel != I/O cancel) and the
// sink delivers no waiter.
SLUICE_TEST_CASE(tp_wait_cancel_keeps_io) {
    ThreadPoolBackend::BeforeWorkerDequeuePauseGate gate;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_before_dequeue_pause_gate(&gate);
    ScopedGateResume guard(gate);
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    TempPath tp("W");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0xEF}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;

    if (!wait_paused(gate, deadline)) {
        fail_msg = "Gate W did not pause in time";
    } else {
        auto h = backend.handle_for_completion_for_test(&c);
        if (!h.has_value()) {
            fail_msg = "handle_for_completion_for_test must find the bound Completion";
        } else if (!backend.register_waiter_for_test(c, WaiterToken{2, 0, 0},
                                                     detail::RoutingLease{55})
                        .has_value()) {
            fail_msg = "waiter registration on the enqueued op must succeed";
        } else {
            auto rl = backend.cancel_waiter_for_test(c);
            if (!rl.has_value() || rl.value().id() != 55) {
                fail_msg = "cancel_waiter must return the registered lease";
            } else {
                auto w = backend.waiter_for_test(*h);
                auto b = backend.borrow_for_test(*h);
                if (!w.has_value() ||
                    w->registration != WaiterRegistration::open_no_waiter ||
                    w->delivery_present) {
                    fail_msg = "registration must be reopened with no stored delivery";
                } else if (!b.has_value() || !b->active) {
                    fail_msg = "wait-cancel must not end the borrow";
                } else if (stats.canceled_ops != 0) {
                    fail_msg = "wait-cancel must never tally canceled_ops";
                }
            }
        }
    }

    if (fail_msg == nullptr) {
        guard.resume();
        if (!drain_bounded(backend,
                           std::chrono::steady_clock::now() + kWaitTimeout)) {
            fail_msg = "drain did not complete in time";
        } else if (!c.ready()) {
            fail_msg = "I/O must still complete after wait-cancel";
        } else if (!c.result().has_value() || c.result().value() != 1) {
            fail_msg = "the real syscall result must win";
        } else if (backend.syscall_count_for_test() != 1) {
            fail_msg = "wait-cancel must NOT cancel the I/O (syscall executed)";
        } else if (backend.sink_deliveries() != 1 ||
                   backend.sink_last_has_waiter()) {
            fail_msg = "no waiter may be delivered after wait-cancel";
        } else if (stats.canceled_ops != 0) {
            fail_msg = "wait-cancel must never tally canceled_ops";
        }
    }

    if (fail_msg == nullptr) {
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        guard.resume();
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kWaitTimeout);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- Row 13 (ThreadPool): enqueued I/O cancel keeps the waiter --------------
// Gate B holds the op enqueued; an I/O cancel WINS the canceled terminal
// (canceled_ops tallies) but does NOT delete the waiter registration; the
// borrow stays active; reap delivers the canceled result AND the waiter
// exactly once, and no syscall ever runs.
SLUICE_TEST_CASE(tp_io_cancel_keeps_waiter) {
    ThreadPoolBackend::BeforeWorkerDequeuePauseGate gate;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    backend.set_before_dequeue_pause_gate(&gate);
    ScopedGateResume guard(gate);
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    TempPath tp("I");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x12}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    SLUICE_CHECK(backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value());

    const char* fail_msg = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;

    if (!wait_paused(gate, deadline)) {
        fail_msg = "Gate I did not pause in time";
    } else {
        auto h = backend.handle_for_completion_for_test(&c);
        if (!h.has_value()) {
            fail_msg = "handle_for_completion_for_test must find the bound Completion";
        } else if (!backend.register_waiter_for_test(c, WaiterToken{3, 1, 1},
                                                     detail::RoutingLease{66})
                        .has_value()) {
            fail_msg = "waiter registration on the enqueued op must succeed";
        } else {
            backend.cancel(c);  // enqueued I/O cancel wins the terminal
            if (stats.canceled_ops != 1) {
                fail_msg = "terminal_won must tally exactly one canceled op";
            } else {
                auto w = backend.waiter_for_test(*h);
                auto b = backend.borrow_for_test(*h);
                if (!w.has_value() ||
                    w->registration != WaiterRegistration::open_registered ||
                    w->token != WaiterToken{3, 1, 1} || w->lease_id != 66) {
                    fail_msg = "I/O cancel must NOT delete the waiter registration";
                } else if (!b.has_value() || !b->active) {
                    fail_msg = "I/O cancel must not end the borrow";
                } else if (c.ready()) {
                    fail_msg = "Completion must not be ready before reap";
                }
            }
        }
    }

    if (fail_msg == nullptr) {
        guard.resume();
        if (!drain_bounded(backend,
                           std::chrono::steady_clock::now() + kWaitTimeout)) {
            fail_msg = "drain did not complete in time";
        } else if (!c.ready()) {
            fail_msg = "canceled op must be ready after drain";
        } else if (c.result().has_value() ||
                   c.result().error().code != IoError::Code::canceled) {
            fail_msg = "canceled op must report IoError::canceled";
        } else if (backend.syscall_count_for_test() != 0) {
            fail_msg = "canceled enqueued op must not execute a syscall";
        } else if (backend.sink_deliveries() != 1 ||
                   !backend.sink_last_has_waiter() ||
                   backend.sink_last_token() != WaiterToken{3, 1, 1} ||
                   backend.sink_last_lease_id() != 66) {
            fail_msg = "waiter must be delivered exactly once with token A + lease 66";
        }
    }

    if (fail_msg == nullptr) {
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        guard.resume();
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kWaitTimeout);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// ---- Row 14a (ThreadPool): stale waiter authority harmless ------------------
// After release + reuse of the same physical slot, a stale-generation
// register/cancel_waiter handle resolves to not_found with ZERO side effect on
// the live N+1 occupant's registration; the new occupant still delivers its
// own waiter exactly once.
SLUICE_TEST_CASE(tp_stale_waiter_authority_harmless) {
    ThreadPoolBackend::BeforeWorkerDequeuePauseGate gate;
    ThreadPoolBackend backend(ThreadPoolConfig{/*capacity=*/1, /*workers=*/1});
    // The gate is armed only for the gen-N+1 submit (C2b pattern).
    sluice::AsyncStats stats;
    backend.attach_stats(&stats);

    TempPath tp("S");
    int fd = open_temp(tp.path());
    const std::byte seed[1] = {std::byte{0x34}};
    SLUICE_CHECK(::pwrite(fd, seed, 1, 0) == 1);

    std::byte buf[1]{};
    Completion<std::size_t> c;

    const char* fail_msg = nullptr;
    std::optional<detail::SlotHandle> h0;

    // Generation N: full lifecycle; capture the handle BEFORE the release.
    if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
        fail_msg = "first submit must succeed";
    } else if (!drain_bounded(backend,
                              std::chrono::steady_clock::now() + kWaitTimeout)) {
        fail_msg = "first drain did not complete in time";
    } else if (!c.ready() || !c.result().has_value() || c.result().value() != 1) {
        fail_msg = "first op must complete with the seeded byte";
    } else {
        h0 = backend.handle_for_completion_for_test(&c);
        if (!h0.has_value()) {
            fail_msg = "the bound Completion must resolve to a slot handle";
        }
    }

    std::optional<detail::SlotHandle> h1;
    if (fail_msg == nullptr) {
        c.reset();  // release handshake: slot freed, generation advances to N+1
        backend.set_before_dequeue_pause_gate(&gate);
        if (!backend.submit_read(ReadOp{fd, buf, 1, 0}, c).has_value()) {
            fail_msg = "second submit must reuse the released slot";
        } else if (!wait_paused(gate,
                                std::chrono::steady_clock::now() + kWaitTimeout)) {
            fail_msg = "gate did not pause before the N+1 dequeue";
        } else {
            h1 = backend.handle_for_completion_for_test(&c);
            if (!h1.has_value()) {
                fail_msg = "new occupant must resolve to a slot handle";
            } else if (h1->slot.value != h0->slot.value ||
                       h1->generation.value != h0->generation.value + 1) {
                fail_msg = "reuse must keep the slot and advance generation by one";
            } else if (!backend.register_waiter_for_test(c, WaiterToken{4, 2, 2},
                                                         detail::RoutingLease{200})
                            .has_value()) {
                fail_msg = "N+1 occupant waiter registration must succeed";
            }
        }
    }

    if (fail_msg == nullptr) {
        // Inject the stale N-handle through the REAL waiter authorities.
        auto stale_cancel = backend.cancel_waiter_handle_for_test(*h0);
        if (stale_cancel.has_value()) {
            fail_msg = "stale cancel_waiter must resolve to not_found";
        } else if (stale_cancel.error().code != IoError::Code::not_found) {
            fail_msg = "stale cancel_waiter must report not_found";
        } else {
            auto stale_register = backend.register_waiter_handle_for_test(
                *h0, WaiterToken{9, 9, 9}, detail::RoutingLease{300});
            if (stale_register.has_value()) {
                fail_msg = "stale register_waiter must resolve to invalid_state";
            } else if (stale_register.error().code != IoError::Code::invalid_state) {
                fail_msg = "stale register_waiter must report invalid_state";
            } else {
                auto w = backend.waiter_for_test(*h1);
                if (!w.has_value() ||
                    w->registration != WaiterRegistration::open_registered ||
                    w->token != WaiterToken{4, 2, 2} || w->lease_id != 200) {
                    fail_msg = "stale waiter authority must leave the N+1 "
                               "registration untouched";
                }
            }
        }
    }

    if (fail_msg == nullptr) {
        resume_threadpool_gate(gate);
        wait_gate_exit(gate, "tp_stale_waiter_authority_harmless");
    }

    if (fail_msg == nullptr) {
        if (!drain_bounded(backend,
                           std::chrono::steady_clock::now() + kWaitTimeout)) {
            fail_msg = "second drain did not complete in time";
        } else if (!c.ready() || !c.result().has_value() ||
                   c.result().value() != 1) {
            fail_msg = "new occupant must complete with ITS OWN result";
        } else if (backend.sink_deliveries() != 2 ||  // exactly one per generation
                   !backend.sink_last_has_waiter() ||
                   backend.sink_last_token() != WaiterToken{4, 2, 2} ||
                   backend.sink_last_lease_id() != 200) {
            fail_msg = "N+1 occupant waiter must be delivered exactly once "
                       "(one delivery per generation; last payload = B)";
        } else if (stats.canceled_ops != 0 || stats.completion_errors != 0) {
            fail_msg = "stale attempts must leave counters intact";
        }
    }

    if (fail_msg == nullptr) {
        c.reset();
        SLUICE_CHECK(backend.arena_slot_in_use() == 0);
    } else {
        resume_threadpool_gate(gate);
        wait_gate_exit(gate, "tp_stale_waiter_authority_harmless cleanup");
        (void)drain_bounded(backend,
                            std::chrono::steady_clock::now() + kWaitTimeout);
        if (c.ready()) c.reset();
    }

    ::close(fd);
    if (fail_msg != nullptr) SLUICE_FAIL(fail_msg);
}

// S0B-CORRECTIVE-1 W1 — the adjudicated register/cancel error-vocabulary
// split (unbound / cross-context / duplicate / post-reap / stale / no-
// registration), driven through the PUBLIC ThreadPoolBackend interface with
// a real worker-thread write.
SLUICE_TEST_CASE(threadpool_waiter_error_vocabulary_split) {
    TempPath tp{"waiter_vocab"};
    int fd = open_temp(tp.path());
    auto rc = waiter_error_vocabulary::run_waiter_error_vocabulary_cases<
        ThreadPoolBackend>(
        [] { return std::make_unique<ThreadPoolBackend>(
                  ThreadPoolConfig{/*capacity=*/4, /*workers=*/1}); },
        fd,
        [](ThreadPoolBackend& b, Completion<std::size_t>& c) {
            const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
            while (b.poll() == 0) {
                if (std::chrono::steady_clock::now() >= deadline) return false;
                std::this_thread::yield();
            }
            return c.ready();
        });
    ::close(fd);
    if (rc != nullptr) SLUICE_FAIL(rc);
}
