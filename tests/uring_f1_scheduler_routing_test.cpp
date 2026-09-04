// Phase F1 (issue #98) — the PRODUCTION Scheduler consumes identity-bearing
// reap on the REAL liburing path.
//
// This target links the PRODUCTION sluice_async library (which carries
// SLUICE_HAS_LIBURING + the liburing package publicly when the build enables
// the gate), so the whole path exercised here is authoritative production
// code with NO internal-testing seams: a fiber submits a real ring read,
// Scheduler::await_completion registers an arena waiter (WaiterToken +
// RoutingLease) plus a Scheduler routing record, the ring CQE reaches a
// terminal, the Uring reap calls the Scheduler-owned ReadyRoutingSink with
// the by-value ReadyEvent, and the drain routes the fiber exactly once with
// the real result (ADR Decisions 9/10). The legacy Completion*-keyed
// ready() re-scan is not on this path.
//
// Stub mode proves build/API honesty only: UringAsyncBackend::available()
// is false, the case returns early, and the evidence-meta line classifies
// the run mode=stub so the aggregate gate records INCOMPLETE (required
// modes: real) instead of an accidental pass.
//
// Design: docs/history/implementation-plans/phase-f1-scheduler-ready-sink.md
// Gate:   docs/history/closeout/phase-f1-compliance-gate.md (Gate 4 evidence)
#include "harness.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/error.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <vector>

#if defined(SLUICE_HAS_LIBURING)
#include <unistd.h>
#endif

using namespace sluice::async;
using sluice::IoError;

// ---------------------------------------------------------------------------
// Evidence-meta (G2): exactly one [evidence-meta] line per gate-driven run.
//
// Mirrors the c2c evidence-mode protocol: the line is emitted in BOTH build
// modes so a stub run is attributed mode=stub and classified INCOMPLETE via
// required_modes=("real",) — never an accidental INCOMPLETE from a missing
// case or an unclassifiable run.
// ---------------------------------------------------------------------------
SLUICE_TEST_CASE(uring_f1_evidence_mode) {
#if defined(SLUICE_HAS_LIBURING)
    UringAsyncBackend backend{UringConfig{4, 4}};
    std::printf("[evidence-meta] evidence=uring_f1_scheduler_routing_integration mode=real\n");
    SLUICE_CHECK(backend.available());
#else
    std::printf("[evidence-meta] evidence=uring_f1_scheduler_routing_integration mode=stub\n");
#endif
}

// ---------------------------------------------------------------------------
// Temporary file helper (real fd for the ring read).
// ---------------------------------------------------------------------------
namespace {
struct TempPath {
    TempPath() : path_((std::filesystem::temp_directory_path() /
                        ("sluice_f1_uring_" + std::to_string(++counter_))).string()) {}
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

// 64 KiB scratch stack for an E4 fiber. 16-byte aligned (mirrors
// evented_scheduler_test).
struct FiberStack {
    static constexpr std::size_t kBytes = 64 * 1024;
    alignas(16) std::vector<std::byte> bytes{kBytes};
    std::byte* base() noexcept { return bytes.data(); }
    std::size_t size() const noexcept { return bytes.size(); }
};
}  // namespace

// ---- The production Scheduler routes the real Uring reap exactly once. ----
SLUICE_TEST_CASE(uring_f1_scheduler_routing) {
#if defined(SLUICE_HAS_LIBURING)
    if constexpr (!fiber_ctx::supported) return;

    auto backend_up = std::make_unique<UringAsyncBackend>(UringConfig{4, 4});
    UringAsyncBackend* backend = backend_up.get();
    if (!backend->available())
        return;
    AsyncIoContext ctx(std::move(backend_up));
    Scheduler sched(ctx);

    TempPath tmp;
    int fd = ::open(tmp.path().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    SLUICE_CHECK(fd >= 0);
    std::byte seed[8] = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
                         std::byte{0x44}, std::byte{0x55}, std::byte{0x66},
                         std::byte{0x77}, std::byte{0x88}};
    SLUICE_CHECK(::write(fd, seed, sizeof(seed)) ==
                 static_cast<ssize_t>(sizeof(seed)));

    Completion<std::size_t> c;
    std::byte buf[8]{};
    int resumed = 0;
    std::size_t observed = 0;
    Fiber fa;
    fa.set_entry([&](Fiber&) {
        SLUICE_CHECK(ctx.submit_read(ReadOp{fd, buf, 8, 0}, c).has_value());
        auto r = sched.await_completion_size(c);
        SLUICE_CHECK(r.has_value());
        resumed = 1;
        observed = c.result().value_or(0);
    });
    FiberStack stack;
    SLUICE_CHECK(sched.init_fiber(fa, stack.base(), stack.size()));
    sched.spawn(fa);
    sched.run_until_idle();

    // The identity route resumed the fiber exactly once with the real
    // result; the Completion published normally and resets cleanly.
    SLUICE_CHECK(resumed == 1);
    SLUICE_CHECK(observed == 8);
    SLUICE_CHECK(c.ready());
    c.reset();
    ::close(fd);
#endif  // defined(SLUICE_HAS_LIBURING)
}

SLUICE_MAIN()
