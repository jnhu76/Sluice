#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/async_backend.hpp>
#include <sluice/async/detail/threadpool_backend.hpp>
#include <sluice/async/request.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using namespace sluice::async;
using sluice::AsyncStats;
using sluice::FileAccess;
using sluice::IoError;
using sluice::async::NativeFileRef;
using sluice::async::ReadOp;
using sluice::async::ThreadPoolBackend;
using sluice::async::ThreadPoolConfig;

using WaitKind = AsyncIoContext::ProgressWaitOutcome::Kind;

int temp_file_fd(const std::string& content) {
    char path[] = "/tmp/sluice_wait_one_oracle_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return -1;
    ::unlink(path);
    const ssize_t written = ::write(fd, content.data(), content.size());
    if (written != static_cast<ssize_t>(content.size())) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// A request binding fail-fasts on a nonterminal release, so a binding that may
// still be short of publication is drained to readiness through owner polls
// before it is discarded.
bool drain_to_publication(AsyncIoContext& ctx, Request<std::size_t>& request) {
    for (int i = 0; i < 200000 && request.valid() && !request.ready(); ++i)
        (void)ctx.poll();
    if (!request.valid() || !request.ready())
        return false;
    request.discard();
    return true;
}

// Design §3.2-2a: every wait_one entry that passes the closure check counts
// exactly once, ahead of drive admission; the idle context answers each
// bounded wait with a zero-completion progress report.
bool quiet_idle_waits_count_exactly_once_per_entry() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncStats stats;
    AsyncIoContext ctx = detail::make_context(std::move(backend), &stats);
    constexpr int kEntries = 8;
    for (int i = 0; i < kEntries; ++i) {
        const auto waited = ctx.wait_one(std::chrono::milliseconds{0});
        if (!waited.has_value() || waited.value().kind != WaitKind::progress ||
            waited.value().completed != 0)
            return false;
    }
    return stats.wait_calls == static_cast<std::uint64_t>(kEntries);
}

// Design §3.2-2a nullable path: the same entry sequence with stats disabled
// takes the null counter without a count, an exception or a behavior change.
bool stats_null_idle_waits_neither_count_nor_throw() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncIoContext ctx = detail::make_context(std::move(backend));
    for (int i = 0; i < 8; ++i) {
        const auto waited = ctx.wait_one(std::chrono::milliseconds{0});
        if (!waited.has_value() || waited.value().kind != WaitKind::progress ||
            waited.value().completed != 0)
            return false;
    }
    return true;
}

// Design §3.2-2b: an entry refused by the closure guard produces invalid_state
// and must not touch the counter.
bool closed_entry_rejects_without_counting() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncStats stats;
    AsyncIoContext ctx = detail::make_context(std::move(backend), &stats);
    if (!ctx.wait_one(std::chrono::milliseconds{0}).has_value())
        return false;
    const std::uint64_t before = stats.wait_calls;
    const auto settled = ctx.shutdown(sluice::async::ShutdownPolicy::drain);
    if (!settled.has_value())
        return false;
    for (int i = 0; i < 3; ++i) {
        const auto refused = ctx.wait_one(std::chrono::milliseconds{0});
        if (refused.has_value() || refused.error().code != IoError::Code::invalid_state)
            return false;
    }
    return stats.wait_calls == before;
}

// Design §3.2-2c: counting happens before drive admission, so an entry the
// drive gate refuses still counts. The pinned-owner foreign wait is refused
// deterministically, one count per refused entry.
bool gate_refused_entry_still_counts_before_admission() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncStats stats;
    AsyncIoContext ctx = detail::make_context(std::move(backend), &stats);
    auto claimed = ctx.claim_progress_owner();
    if (!claimed.has_value())
        return false;
    std::optional<ProgressOwner> owner = std::move(claimed).value();

    std::atomic<bool> foreign_rejected{false};
    std::thread foreign([&] {
        bool all_rejected = true;
        for (int i = 0; i < 3; ++i) {
            const auto refused = ctx.wait_one(std::chrono::milliseconds{0});
            all_rejected = all_rejected && !refused.has_value() &&
                           refused.error().code == IoError::Code::invalid_state;
        }
        foreign_rejected.store(all_rejected, std::memory_order_release);
    });
    foreign.join();
    if (!foreign_rejected.load(std::memory_order_acquire))
        return false;
    const bool counted = stats.wait_calls == 3;
    owner.reset();
    return counted;
}

// Design §3.2-2d: the outcome kinds and their completion semantics stay on the
// external-loop contract — deadline expiry on a held completion, progress with
// the completion count, sticky control, and the idle zero-completion report —
// and every entry, whatever its outcome, counts exactly once.
bool kind_assignment_matches_external_loop_contract() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncStats stats;
    AsyncIoContext ctx = detail::make_context(std::move(backend), &stats);
    auto claimed = ctx.claim_progress_owner();
    if (!claimed.has_value())
        return false;
    std::optional<ProgressOwner> owner = std::move(claimed).value();

    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    raw->set_worker_claimed_pause_gate(&gate);

    const int fd = temp_file_fd(std::string(64, 'd'));
    if (fd < 0)
        return false;
    std::vector<std::byte> buf(32, std::byte{0});
    auto submitted = ctx.submit_read(
        ReadOp{NativeFileRef(fd, FileAccess::read_only), buf.data(), buf.size(), 0});
    if (!submitted.has_value()) {
        ::close(fd);
        return false;
    }
    Request<std::size_t> held = std::move(submitted).value();

    const auto expired = ctx.wait_one(std::chrono::milliseconds{100});
    if (!expired.has_value() || expired.value().kind != WaitKind::deadline_expired ||
        expired.value().completed != 0 || held.ready()) {
        raw->set_worker_claimed_pause_gate(nullptr);
        gate.resume.store(true, std::memory_order_release);
        gate.resume.notify_all();
        (void)drain_to_publication(ctx, held);
        ::close(fd);
        return false;
    }

    raw->set_worker_claimed_pause_gate(nullptr);
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();

    const auto progressed = ctx.wait_one(std::chrono::milliseconds{5000});
    if (!progressed.has_value() || progressed.value().kind != WaitKind::progress ||
        progressed.value().completed != 1) {
        (void)drain_to_publication(ctx, held);
        ::close(fd);
        return false;
    }
    if (!drain_to_publication(ctx, held)) {
        ::close(fd);
        return false;
    }
    ::close(fd);

    ctx.interrupt_progress_waiters();
    const auto control = ctx.wait_one(std::chrono::milliseconds{100});
    if (!control.has_value() || control.value().kind != WaitKind::control_interrupted)
        return false;
    ctx.acknowledge_progress_control();
    const auto settled = ctx.wait_one(std::chrono::milliseconds{50});
    if (!settled.has_value() || settled.value().kind != WaitKind::progress ||
        settled.value().completed != 0)
        return false;

    owner.reset();
    return stats.wait_calls == 4;
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"quiet_idle_waits_count_exactly_once_per_entry",
         quiet_idle_waits_count_exactly_once_per_entry},
        {"stats_null_idle_waits_neither_count_nor_throw",
         stats_null_idle_waits_neither_count_nor_throw},
        {"closed_entry_rejects_without_counting", closed_entry_rejects_without_counting},
        {"gate_refused_entry_still_counts_before_admission",
         gate_refused_entry_still_counts_before_admission},
        {"kind_assignment_matches_external_loop_contract",
         kind_assignment_matches_external_loop_contract},
    };
    int failed = 0;
    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            ++failed;
        }
    }
    const int total = static_cast<int>(sizeof(tests) / sizeof(tests[0]));
    if (failed != 0) {
        std::fprintf(stderr, "%d of %d wait_one entry semantics tests failed\n", failed,
                     total);
        return 1;
    }
    std::printf("all %d wait_one entry semantics tests passed\n", total);
    return 0;
}
