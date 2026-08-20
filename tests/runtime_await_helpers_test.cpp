// Runtime await-style helper contract tests (C7, #135).
//
// Covers the library coordinators the four applications now share:
//   await_take / await_drain / await_read_once / await_read_fill /
//   await_write_exact (include/sluice/async/await_op_helpers.hpp)
//   TaskResultSlot / run_task_to_result (include/sluice/async/task_result.hpp)
//
// Deterministic construction:
//  - auto-mode cases: FakeAsyncBackend's public scriptable surface (auto_*
//    completion modes) drives exact outcomes while the Runtime driver polls.
//  - staged-completion cases: the shared-state ScriptedBackendController
//    completes individual ops from a helper thread; every staged result
//    signals the backend's ready wait, so the parked Runtime driver wakes and
//    reaps (the split-wait contract). The controller waits are
//    condition-variable based with a bounded deadline — no sleep-ordering.
//  - one end-to-end case drives the REAL ThreadPoolBackend on a real file.
//
// These tests link the PRODUCTION sluice_async (public headers only).
#include "harness.hpp"
#include "support/scripted_async_backend.hpp"

#include <sluice/async/await_op_helpers.hpp>
#include <sluice/async/fake_backend.hpp>
#include <sluice/async/task_result.hpp>
#include <sluice/async/threadpool_backend.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

using namespace sluice::async;
using sluice::IoError;
using sluice::Result;

namespace {

// Runs `body(ctx)` as one Runtime task over `backend`. The driver overload
// runs the completion script on a helper thread while the task is in flight
// (the script returns false on its bounded deadline); the plain overload
// relies on the fake's auto modes. Returns the task's published outcome. The
// runtime lifecycle is exactly run_task_to_result's (wait for publish BEFORE
// stop).
template <class T, class Fn, class Driver>
Result<T> run_driven_task(unsigned workers, std::unique_ptr<AsyncBackend> backend,
                          Fn&& body, Driver driver) {
    static_assert(std::is_invocable_v<Fn&, RuntimeTaskContext&>);

    TaskResultSlot<Result<T>> slot;

    RuntimeBuilder builder;
    builder.backend(std::move(backend));
    builder.workers(workers);
    auto build_r = builder.build();
    if (!build_r.has_value()) return make_unexpected<T>(build_r.error());
    auto rt = std::move(build_r.value());
    auto start_r = rt->start();
    if (!start_r.has_value()) return make_unexpected<T>(start_r.error());

    auto sub_r = rt->submit([&body, &slot](RuntimeTaskContext& ctx) {
        try {
            slot.publish(body(ctx));
        } catch (...) {
            slot.publish(translate_task_exception<T>());
        }
    });
    if (!sub_r.has_value()) {
        (void)rt->shutdown();
        return make_unexpected<T>(sub_r.error());
    }

    std::atomic<bool> driver_ok{true};
    std::thread driver_th([&driver_ok, &driver] {
        driver_ok.store(driver());
    });

    Result<T> out = slot.wait_and_take();
    driver_th.join();
    rt->request_stop();
    (void)rt->drain();
    (void)rt->join();
    if (!driver_ok.load())
        return make_unexpected<T>(IoError{IoError::Code::backend_error});
    return out;
}

template <class T, class Fn>
Result<T> run_driven_task(unsigned workers, std::unique_ptr<AsyncBackend> backend,
                          Fn&& body) {
    return run_driven_task<T>(
        workers, std::move(backend), std::forward<Fn>(body),
        []() -> bool { return true; });
}

std::span<std::byte> bytes_of(std::byte* p, std::size_t n) {
    return std::span<std::byte>(p, n);
}

// Bounded observation wait for a pending read/write at `offset` (yield +
// poll; fail-closed on deadline). Needed because pending_count() counts
// STAGED-but-unapplied ops, so a count wait cannot distinguish "the retry op
// was submitted" from "the previous completion is still staged".
template <class Ctrl>
std::optional<std::uint64_t> wait_for_read_at(Ctrl& ctrl, std::uint64_t offset) {
    for (int i = 0; i < 100'000; ++i) {
        if (auto id = ctrl.find_read_by_offset(offset)) return id;
        std::this_thread::yield();
    }
    return ctrl.find_read_by_offset(offset);
}

template <class Ctrl>
std::optional<std::uint64_t> wait_for_write_at(Ctrl& ctrl, std::uint64_t offset) {
    for (int i = 0; i < 100'000; ++i) {
        if (auto id = ctrl.find_write_by_offset(offset)) return id;
        std::this_thread::yield();
    }
    return ctrl.find_write_by_offset(offset);
}

}  // namespace

// ---------------------------------------------------------------------------
// await_read_once — one-shot protocol over the fake's auto modes.
// ---------------------------------------------------------------------------

// Full completion: every op completes with exactly the requested length.
SLUICE_TEST_CASE(await_read_once_full) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* fake = backend.get();
    std::byte buf[8];
    Result<std::size_t> r = run_driven_task<std::size_t>(
        1, std::move(backend), [&](RuntimeTaskContext& ctx) {
            Completion<std::size_t> c;
            fake->auto_bytes(8);
            return await_read_once(ctx, /*fd=*/3, bytes_of(buf, 8), 100, c);
        });
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 8);
}

// Short completion returned verbatim (no retry inside the one-shot helper).
SLUICE_TEST_CASE(await_read_once_short_verbatim) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* fake = backend.get();
    std::byte buf[8];
    Result<std::size_t> r = run_driven_task<std::size_t>(
        1, std::move(backend), [&](RuntimeTaskContext& ctx) {
            Completion<std::size_t> c;
            fake->auto_bytes(3);
            return await_read_once(ctx, 3, bytes_of(buf, 8), 0, c);
        });
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 3);
}

// EOF: auto_eof completes reads with 0.
SLUICE_TEST_CASE(await_read_once_eof_zero) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* fake = backend.get();
    std::byte buf[4];
    Result<std::size_t> r = run_driven_task<std::size_t>(
        1, std::move(backend), [&](RuntimeTaskContext& ctx) {
            Completion<std::size_t> c;
            fake->auto_eof();
            return await_read_once(ctx, 3, bytes_of(buf, 4), 7, c);
        });
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 0);
}

// Terminal I/O error propagates; the Completion is idle afterwards.
SLUICE_TEST_CASE(await_read_once_error_propagates_and_resets) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* fake = backend.get();
    std::byte buf[4];
    std::atomic<bool> idle_after{false};
    Result<std::size_t> r = run_driven_task<std::size_t>(
        1, std::move(backend), [&](RuntimeTaskContext& ctx) {
            Completion<std::size_t> c;
            fake->auto_error(IoError{IoError::Code::permission_denied});
            auto rr = await_read_once(ctx, 3, bytes_of(buf, 4), 0, c);
            idle_after.store(!c.outstanding() && !c.ready());
            return rr;
        });
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::permission_denied);
    SLUICE_CHECK(idle_after.load());
}

// ---------------------------------------------------------------------------
// await_read_fill — short-retry loop; a partial EOF tail is SUCCESS data.
// ---------------------------------------------------------------------------

// short-then-full: first op completes 3 of 8, the second completes its full
// remaining length (the fake's auto_short_then_full mode).
SLUICE_TEST_CASE(await_read_fill_short_then_full) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* fake = backend.get();
    std::byte buf[8];
    std::atomic<int> ops{0}, short_ops{0};
    Result<std::size_t> r = run_driven_task<std::size_t>(
        1, std::move(backend), [&](RuntimeTaskContext& ctx) {
            Completion<std::size_t> c;
            fake->auto_short_then_full(3);
            AwaitOpTally tally;
            auto fr = await_read_fill(ctx, 3, bytes_of(buf, 8), 0, c, &tally);
            ops.store(static_cast<int>(tally.ops));
            short_ops.store(static_cast<int>(tally.short_ops));
            return fr;
        });
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 8);
    SLUICE_CHECK(ops.load() == 2);
    SLUICE_CHECK(short_ops.load() == 1);
}

// Partial-then-EOF: staged completions — 3 bytes then a 0-byte read. The
// helper returns 3 as SUCCESS (a partial tail is data, not an eof error).
SLUICE_TEST_CASE(await_read_fill_partial_eof_is_data) {
    auto pair = make_scripted_backend();
    std::byte buf[8];
    std::atomic<int> ops{0};
    Result<std::size_t> r = run_driven_task<std::size_t>(
        1, std::move(pair.backend),
        [&](RuntimeTaskContext& ctx) {
            Completion<std::size_t> c;
            AwaitOpTally tally;
            auto fr = await_read_fill(ctx, 3, bytes_of(buf, 8), 0, c, &tally);
            ops.store(static_cast<int>(tally.ops));
            return fr;
        },
        [ctrl = pair.controller]() mutable -> bool {
            using std::chrono::milliseconds;
            if (ctrl.wait_until_pending_for(1, milliseconds{5000}) !=
                WaitStatus::ready) {
                ctrl.complete_all_for_cleanup();
                return false;
            }
            auto id1 = ctrl.find_read_by_offset(0);
            if (!id1.has_value()) {
                ctrl.complete_all_for_cleanup();
                return false;
            }
            ctrl.complete_bytes(*id1, 3);
            auto id2 = wait_for_read_at(ctrl, 3);
            if (!id2.has_value()) {
                ctrl.complete_all_for_cleanup();
                return false;
            }
            ctrl.complete_eof(*id2);
            return true;
        });
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 3);
    SLUICE_CHECK(ops.load() == 2);
}

// ---------------------------------------------------------------------------
// await_write_exact — partial retry; zero progress is a deterministic error.
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(await_write_exact_partial_retry) {
    auto pair = make_scripted_backend();
    std::byte src[8];
    std::atomic<int> ops{0}, short_ops{0};
    Result<std::size_t> r = run_driven_task<std::size_t>(
        1, std::move(pair.backend),
        [&](RuntimeTaskContext& ctx) {
            Completion<std::size_t> c;
            AwaitOpTally tally;
            auto wr = await_write_exact(
                ctx, 4, std::span<const std::byte>(src, 8), 0, c, &tally);
            ops.store(static_cast<int>(tally.ops));
            short_ops.store(static_cast<int>(tally.short_ops));
            return wr;
        },
        [ctrl = pair.controller]() mutable -> bool {
            using std::chrono::milliseconds;
            // 5 then 3: two partial writes that sum to the exact length.
            if (ctrl.wait_until_pending_for(1, milliseconds{5000}) !=
                WaitStatus::ready) {
                ctrl.complete_all_for_cleanup();
                return false;
            }
            auto id1 = ctrl.find_write_by_offset(0);
            if (!id1.has_value()) {
                ctrl.complete_all_for_cleanup();
                return false;
            }
            ctrl.complete_bytes(*id1, 5);
            auto id2 = wait_for_write_at(ctrl, 5);
            if (!id2.has_value()) {
                ctrl.complete_all_for_cleanup();
                return false;
            }
            ctrl.complete_bytes(*id2, 3);
            return true;
        });
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 8);
    SLUICE_CHECK(ops.load() == 2);
    // Only the first completion is short (5 of 8); the retry completes 3 of 3.
    SLUICE_CHECK(short_ops.load() == 1);
}

SLUICE_TEST_CASE(await_write_exact_zero_progress_is_error) {
    auto pair = make_scripted_backend();
    std::byte src[8];
    Result<std::size_t> r = run_driven_task<std::size_t>(
        1, std::move(pair.backend),
        [&](RuntimeTaskContext& ctx) {
            Completion<std::size_t> c;
            return await_write_exact(
                ctx, 4, std::span<const std::byte>(src, 8), 0, c);
        },
        [ctrl = pair.controller]() mutable -> bool {
            using std::chrono::milliseconds;
            if (ctrl.wait_until_pending_for(1, milliseconds{5000}) !=
                WaitStatus::ready) {
                ctrl.complete_all_for_cleanup();
                return false;
            }
            auto id = ctrl.find_write_by_offset(0);
            if (!id.has_value()) {
                ctrl.complete_all_for_cleanup();
                return false;
            }
            ctrl.complete_bytes(*id, 0);
            return true;
        });
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::backend_error);
}

// ---------------------------------------------------------------------------
// await_take / await_drain.
// ---------------------------------------------------------------------------

// await_drain swallows the terminal error (the error-path cleanup protocol:
// the primary error was already captured elsewhere).
SLUICE_TEST_CASE(await_drain_swallows_terminal_error) {
    auto backend = std::make_unique<FakeAsyncBackend>();
    FakeAsyncBackend* fake = backend.get();
    Result<bool> r = run_driven_task<bool>(
        1, std::move(backend), [&](RuntimeTaskContext& ctx) {
            Completion<std::size_t> c;
            fake->auto_error(IoError{IoError::Code::no_space});
            (void)ctx.submit_read(ReadOp{3, nullptr, 0, 0}, c);
            return await_drain(ctx, c).has_value();
        });
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value());
}

// ---------------------------------------------------------------------------
// TaskResultSlot — exactly-once publish, first wins (no runtime needed).
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(task_result_slot_first_publish_wins) {
    TaskResultSlot<Result<int>> slot;
    slot.publish(Result<int>(1));
    slot.publish(Result<int>(2));  // dropped: the first outcome is terminal
    Result<int> r = slot.wait_and_take();
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 1);
}

// ---------------------------------------------------------------------------
// run_task_to_result — the lifecycle bridge.
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(run_task_to_result_happy_path) {
    auto r = run_task_to_result<std::size_t>(
        1, std::make_unique<FakeAsyncBackend>(),
        [](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            (void)ctx;
            slot.publish(Result<std::size_t>(42));
        });
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 42);
}

SLUICE_TEST_CASE(run_task_to_result_task_error) {
    auto r = run_task_to_result<std::size_t>(
        1, std::make_unique<FakeAsyncBackend>(),
        [](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            (void)ctx;
            slot.publish(
                make_unexpected<std::size_t>(IoError{IoError::Code::not_found}));
        });
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::not_found);
}

// An escaping exception is translated into the published outcome (the
// caller can never hang on a throwing task).
SLUICE_TEST_CASE(run_task_to_result_throwing_task_translated) {
    auto r = run_task_to_result<std::size_t>(
        1, std::make_unique<FakeAsyncBackend>(),
        [](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            (void)ctx;
            (void)slot;
            throw std::system_error{std::make_error_code(
                std::errc::no_such_file_or_directory)};
        });
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::backend_error);
    SLUICE_CHECK(r.error().os_errno == ENOENT);
}

SLUICE_TEST_CASE(run_task_to_result_rejects_invalid_config) {
    auto r = run_task_to_result<std::size_t>(
        0, std::make_unique<FakeAsyncBackend>(),
        [](RuntimeTaskContext&, TaskResultSlot<Result<std::size_t>>&) {});
    SLUICE_CHECK(!r.has_value());
    SLUICE_CHECK(r.error().code == IoError::Code::invalid_state);
}

// One-shot take contract: a SECOND wait_and_take is surfaced deterministically
// as std::bad_optional_access — never as a silent moved-from value.
SLUICE_TEST_CASE(task_result_slot_second_take_is_bad_optional_access) {
    TaskResultSlot<Result<int>> slot;
    slot.publish(Result<int>{1});
    SLUICE_CHECK(slot.wait_and_take().has_value());
    bool threw = false;
    try {
        (void)slot.wait_and_take();
    } catch (const std::bad_optional_access&) {
        threw = true;
    }
    SLUICE_CHECK(threw);
}

// Zero-length spans complete without submitting anything (the coordinator
// loops are vacuous) and are distinguishable from EOF only by the caller's
// own length knowledge — the documented contract, not an accident.
SLUICE_TEST_CASE(await_zero_length_spans_return_zero_without_submitting) {
    auto pair = make_scripted_backend();
    Result<std::size_t> w = run_driven_task<std::size_t>(
        1, std::move(pair.backend),
        [&](RuntimeTaskContext& ctx) {
            Completion<std::size_t> c;
            return await_write_exact(ctx, 4, std::span<const std::byte>{}, 0,
                                     c);
        },
        []() mutable -> bool { return true; });
    SLUICE_CHECK(w.has_value());
    SLUICE_CHECK(w.value() == 0);

    auto pair2 = make_scripted_backend();
    Result<std::size_t> rd = run_driven_task<std::size_t>(
        1, std::move(pair2.backend),
        [&](RuntimeTaskContext& ctx) {
            Completion<std::size_t> c;
            return await_read_fill(ctx, 4, std::span<std::byte>{}, 0, c);
        },
        []() mutable -> bool { return true; });
    SLUICE_CHECK(rd.has_value());
    SLUICE_CHECK(rd.value() == 0);
}

// Exact-boundary EOF: when the final partial completion fills dst exactly,
// the coordinator stops at the boundary with SUCCESS — EOF coinciding with
// the requested length is not an error and needs no extra probe read.
SLUICE_TEST_CASE(await_read_fill_exact_boundary_eof_is_success) {
    auto pair = make_scripted_backend();
    std::byte dst[8];
    Result<std::size_t> r = run_driven_task<std::size_t>(
        1, std::move(pair.backend),
        [&](RuntimeTaskContext& ctx) {
            Completion<std::size_t> c;
            return await_read_fill(ctx, 4, std::span<std::byte>(dst, 8), 0,
                                   c);
        },
        [ctrl = pair.controller]() mutable -> bool {
            using std::chrono::milliseconds;
            if (ctrl.wait_until_pending_for(1, milliseconds{5000}) !=
                WaitStatus::ready) {
                ctrl.complete_all_for_cleanup();
                return false;
            }
            auto id1 = ctrl.find_read_by_offset(0);
            if (!id1.has_value()) {
                ctrl.complete_all_for_cleanup();
                return false;
            }
            // 5 then 3: the second completion lands exactly on the boundary.
            ctrl.complete_bytes(*id1, 5);
            auto id2 = wait_for_read_at(ctrl, 5);
            if (!id2.has_value()) {
                ctrl.complete_all_for_cleanup();
                return false;
            }
            ctrl.complete_bytes(*id2, 3);
            return true;
        });
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == 8);
}

// ---------------------------------------------------------------------------
// End-to-end: the REAL ThreadPoolBackend against a real temp file — the
// helpers drive genuine pread through the whole Runtime path.
// ---------------------------------------------------------------------------

SLUICE_TEST_CASE(await_helpers_real_threadpool_roundtrip) {
    char path[] = "/tmp/sluice_await_helpers_test_XXXXXX";
    int fd = ::mkstemp(path);
    SLUICE_CHECK(fd >= 0);
    const char kText[] = "hello await helpers";
    const std::size_t kLen = sizeof(kText) - 1;
    SLUICE_CHECK(::pwrite(fd, kText, kLen, 0) == static_cast<ssize_t>(kLen));

    std::byte buf[64] = {};
    auto r = run_task_to_result<std::size_t>(
        1, std::make_unique<ThreadPoolBackend>(),
        [&](RuntimeTaskContext& ctx, TaskResultSlot<Result<std::size_t>>& slot) {
            Completion<std::size_t> c;
            slot.publish(
                await_read_fill(ctx, fd, bytes_of(buf, kLen), 0, c));
        });
    SLUICE_CHECK(r.has_value());
    SLUICE_CHECK(r.value() == kLen);
    SLUICE_CHECK(std::memcmp(buf, kText, kLen) == 0);

    ::close(fd);
    ::unlink(path);
}

SLUICE_MAIN()
