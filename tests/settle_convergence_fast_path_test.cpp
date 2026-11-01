#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/test_access.hpp>
#include <sluice/async/request.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#if defined(SLUICE_SETTLE_CONVERGENCE_URING)
#include <sluice/async/detail/uring_backend.hpp>
#else
#include <sluice/async/detail/threadpool_backend.hpp>
#endif

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::IoError;

struct Tracker {
    const char* name;
    int failures = 0;
    bool skipped = false;

    void check(bool ok, const char* label) {
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "FAIL [%s] %s\n", name, label);
        }
    }

    void skip(const char* reason) {
        skipped = true;
        std::printf("SKIP [%s] %s\n", name, reason);
    }
};

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_settle_convergence_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return {};
    if (!content.empty()) {
        const ssize_t n = ::write(fd, content.data(), content.size());
        if (n != static_cast<ssize_t>(content.size())) {
            ::close(fd);
            return {};
        }
    }
    ::close(fd);
    return path;
}

std::optional<File> open_temp_file(Tracker& t, const std::string& content) {
    const std::string path = make_temp_file(content);
    if (path.empty()) {
        t.check(false, "the temporary fixture file is created");
        return std::nullopt;
    }
    auto opened = File::open(path);
    ::unlink(path.c_str());
    if (!opened.has_value()) {
        t.check(false, "the temporary fixture file opens");
        return std::nullopt;
    }
    return std::optional<File>{std::move(opened.value())};
}

#if defined(SLUICE_SETTLE_CONVERGENCE_URING)

using Backend = UringAsyncBackend;

std::unique_ptr<Backend> make_backend() {
    try {
        return std::make_unique<UringAsyncBackend>(UringConfig{4, 8});
    } catch (...) {
        return nullptr;
    }
}

#else

using Backend = ThreadPoolBackend;

std::unique_ptr<Backend> make_backend() {
    return std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
}

#endif

struct SettleCounters {
    int pass = 0;
    int snapshot = 0;
    int converged = 0;
    int collect = 0;
};

SettleCounters read_settle_counters(const AsyncIoContext& ctx) {
    return SettleCounters{detail::TestAccess::context_progress_pass_calls(ctx),
                          detail::TestAccess::context_progress_snapshot_calls(ctx),
                          detail::TestAccess::context_settlement_converged_calls(ctx),
                          detail::TestAccess::context_cancel_outstanding_calls(ctx)};
}

struct SettleDelta {
    int pass = 0;
    int snapshot = 0;
    int converged = 0;
    int collect = 0;
};

SettleDelta settle_delta(const SettleCounters& before, const SettleCounters& after) {
    return SettleDelta{after.pass - before.pass, after.snapshot - before.snapshot,
                       after.converged - before.converged,
                       after.collect - before.collect};
}

// Converged-entry form: all submitted work is settled and published before
// shutdown, so the settle window sees exactly the entry probe and, on the
// baseline, the one quiesced pass plus its token snapshot.
bool converged_shutdown_counter_form(Tracker& t, ShutdownPolicy policy) {
    auto file = open_temp_file(t, "settle convergence fast path payload");
    if (!file.has_value())
        return true;
    auto backend = make_backend();
    if (backend == nullptr) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    std::vector<std::byte> first(16, std::byte{0});
    std::vector<std::byte> second(16, std::byte{0});
    auto a = ctx.submit_read(ReadOp{NativeFileRef{*file}, first.data(), first.size(), 0});
    auto b = ctx.submit_read(ReadOp{NativeFileRef{*file}, second.data(), second.size(), 8});
    if (!a.has_value() || !b.has_value()) {
        t.check(false, "both reads are accepted");
        return true;
    }
    Request<std::size_t> ra = std::move(a).value();
    Request<std::size_t> rb = std::move(b).value();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10000);
    while (!(ra.ready() && rb.ready())) {
        (void)ctx.poll();
        if (std::chrono::steady_clock::now() >= deadline) {
            t.check(false, "both reads become ready before shutdown");
            return true;
        }
        std::this_thread::yield();
    }
    t.check(ra.ready() && rb.ready(), "both results are ready before the shutdown");

    const SettleCounters before = read_settle_counters(ctx);
    const auto closed = ctx.shutdown(policy);
    const SettleCounters after = read_settle_counters(ctx);
    const SettleDelta delta = settle_delta(before, after);

    t.check(closed.has_value() && closed.value() == ShutdownOutcome::completed,
            "the converged shutdown reports completion");
    t.check(ctx.execution_closed(), "execution is closed after the shutdown");
#if defined(SLUICE_RESEARCH_ELIDE_014)
    t.check(delta.pass == 0, "the converged shutdown runs zero backend passes");
    t.check(delta.snapshot == 0, "the converged shutdown takes zero token snapshots");
#else
    t.check(delta.pass == 1, "the converged baseline shutdown runs exactly one backend pass");
    t.check(delta.snapshot == 1, "the converged baseline shutdown takes exactly one token snapshot");
#endif
    t.check(delta.converged == 1,
            "the converged shutdown pays exactly one convergence check on either arm");
    if (policy == ShutdownPolicy::cancel_then_drain) {
        t.check(delta.collect == 1,
                "the cancel_then_drain initial cancellation collects outstanding once");
    } else {
        t.check(delta.collect == 0, "a drain shutdown does not collect outstanding");
    }

    auto observed = ra.try_result();
    t.check(observed.readiness == RequestReadiness::ready && observed.result.has_value() &&
                observed.result.value() == 16,
            "the first retained result survives the shutdown");
    rb.discard();

    std::vector<std::byte> late_buffer(4, std::byte{0});
    auto late = ctx.submit_read(
        ReadOp{NativeFileRef{*file}, late_buffer.data(), late_buffer.size(), 0});
    t.check(!late.has_value(), "a submission against the closed admission is refused");
    return true;
}

bool converged_drain_shutdown_counter_form(Tracker& t) {
    return converged_shutdown_counter_form(t, ShutdownPolicy::drain);
}

bool converged_cancel_then_drain_shutdown_counter_form(Tracker& t) {
    return converged_shutdown_counter_form(t, ShutdownPolicy::cancel_then_drain);
}

// Non-converged-entry form: a submitted read is still unreaped at the settle
// entry, so the fast-path probe must be false and the full settle loop must
// run with its per-iteration shape intact. Only the entry check may differ
// between the arms; every per-iteration counter keeps its identity.
bool non_converged_entry_keeps_settle_loop_shape(Tracker& t) {
    auto file = open_temp_file(t, "settle convergence in-flight payload");
    if (!file.has_value())
        return true;
    auto backend = make_backend();
    if (backend == nullptr) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    std::vector<std::byte> buffer(16, std::byte{0});
    auto submitted = ctx.submit_read(ReadOp{NativeFileRef{*file}, buffer.data(), buffer.size(), 0});
    if (!submitted.has_value()) {
        t.check(false, "the in-flight read is accepted");
        return true;
    }
    Request<std::size_t> ra = std::move(submitted).value();

    const SettleCounters before = read_settle_counters(ctx);
    const auto closed = ctx.shutdown(ShutdownPolicy::cancel_then_drain);
    const SettleCounters after = read_settle_counters(ctx);
    const SettleDelta delta = settle_delta(before, after);

    t.check(closed.has_value() && closed.value() == ShutdownOutcome::completed,
            "the in-flight shutdown reports completion");
    t.check(ctx.execution_closed(), "execution is closed after the shutdown");
    t.check(delta.pass >= 1, "a non-converged entry runs the settle loop");
    t.check(delta.snapshot == delta.pass,
            "each settle iteration takes exactly one token snapshot");
    t.check(delta.collect == 1,
            "the cancel_then_drain initial cancellation collects outstanding once");
#if defined(SLUICE_RESEARCH_ELIDE_014)
    t.check(delta.converged == delta.pass + 1,
            "the fast path adds exactly the one entry convergence check to the loop shape");
#else
    t.check(delta.converged == delta.pass,
            "each settle iteration checks convergence exactly once");
#endif

    auto observed = ra.try_result();
    const bool canceled = observed.readiness == RequestReadiness::ready &&
                          !observed.result.has_value() &&
                          observed.result.error().code == IoError::Code::canceled;
    const bool completed = observed.readiness == RequestReadiness::ready &&
                           observed.result.has_value() && observed.result.value() == 16;
    t.check(canceled || completed,
            "the in-flight read terminates canceled or with its full read, never fabricated");
    return true;
}

#if defined(SLUICE_SETTLE_CONVERGENCE_URING)
// Uring in-flight form: an empty open pipe can never complete on its own, so
// cancel_then_drain is the only terminal and the canceled delivery is
// deterministic.
bool inflight_cancel_shutdown_cancels_without_fabricated_success(Tracker& t) {
    auto backend = make_backend();
    if (backend == nullptr) {
        t.skip("io_uring is unavailable on this host");
        return true;
    }
    AsyncIoContext ctx = detail::make_context(std::move(backend));

    int fds[2];
    if (::pipe(fds) != 0) {
        t.check(false, "the in-flight fixture pipe is created");
        return true;
    }
    std::vector<std::byte> buffer(16, std::byte{0});
    auto submitted = ctx.submit_read(
        ReadOp{NativeFileRef{fds[0], sluice::FileAccess::read_only}, buffer.data(),
               buffer.size(), 0});
    if (!submitted.has_value()) {
        t.check(false, "the pipe read is accepted");
        ::close(fds[0]);
        ::close(fds[1]);
        return true;
    }
    Request<std::size_t> ra = std::move(submitted).value();

    const SettleCounters before = read_settle_counters(ctx);
    const auto closed = ctx.shutdown(ShutdownPolicy::cancel_then_drain);
    const SettleCounters after = read_settle_counters(ctx);
    const SettleDelta delta = settle_delta(before, after);

    t.check(closed.has_value() && closed.value() == ShutdownOutcome::completed,
            "the cancel shutdown reports completion");
    t.check(ctx.execution_closed(), "execution is closed after the shutdown");
    t.check(delta.pass >= 1, "a non-converged entry runs the settle loop");
    t.check(delta.snapshot == delta.pass,
            "each settle iteration takes exactly one token snapshot");
    t.check(delta.collect == 1,
            "the cancel_then_drain initial cancellation collects outstanding once");
#if defined(SLUICE_RESEARCH_ELIDE_014)
    t.check(delta.converged == delta.pass + 1,
            "the fast path adds exactly the one entry convergence check to the loop shape");
#else
    t.check(delta.converged == delta.pass,
            "each settle iteration checks convergence exactly once");
#endif

    auto observed = ra.try_result();
    t.check(observed.readiness == RequestReadiness::ready && !observed.result.has_value() &&
                observed.result.error().code == IoError::Code::canceled,
            "the blocked pipe read is canceled without a fabricated success");
    ::close(fds[0]);
    ::close(fds[1]);
    return true;
}
#endif

int run_named(bool (*scenario)(Tracker&), const char* name) {
    Tracker tracker{name};
    scenario(tracker);
    if (!tracker.skipped && tracker.failures == 0)
        std::printf("PASS [%s]\n", name);
    return tracker.failures;
}

}

int main() {
#if defined(SLUICE_SETTLE_CONVERGENCE_URING)
    {
        auto probe = make_backend();
        if (probe == nullptr || !probe->available()) {
            std::printf("SKIP all settle convergence tests: io_uring is unavailable\n");
            return 0;
        }
    }
#endif
    int failures = 0;
    failures += run_named(converged_drain_shutdown_counter_form,
                          "converged_drain_shutdown_counter_form");
    failures += run_named(converged_cancel_then_drain_shutdown_counter_form,
                          "converged_cancel_then_drain_shutdown_counter_form");
    failures += run_named(non_converged_entry_keeps_settle_loop_shape,
                          "non_converged_entry_keeps_settle_loop_shape");
#if defined(SLUICE_SETTLE_CONVERGENCE_URING)
    failures += run_named(inflight_cancel_shutdown_cancels_without_fabricated_success,
                          "inflight_cancel_shutdown_cancels_without_fabricated_success");
#endif
    if (failures != 0)
        std::fprintf(stderr, "%d settle convergence check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
