#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include <liburing.h>

#include <csignal>

namespace {

using namespace sluice::async;
using sluice::FileAccess;

// An application-owned event loop: registration uses the context
// notification fd with real poll(2); a readable fd is acknowledged through
// the documented context operation and followed by bounded progress passes
// that consume the full pass report. Immediate work is serviced now;
// dispatch_retry_remains records one future progress pass, because that
// accepted transport has no completion and no notification coming until a
// pass submits it — an fd-only wait would strand it. The schedule is
// obligation-scoped: it exists only while a pass reports the retry and is
// cleared the first pass that does not. Deliveries are accumulated across
// every pass the host runs — the submitting pass can be the servicing pass
// when the kernel finishes inside its submission window.
class ExternalLoopHost {
  public:
    explicit ExternalLoopHost(AsyncIoContext& ctx) : ctx_(ctx) {
        auto claimed = ctx.claim_progress_owner();
        if (!claimed.has_value())
            return;
        owner_ = std::move(claimed).value();
        nfd_ = ctx.progress_notification_fd();
    }

    int nfd() const noexcept { return nfd_; }

    // Stop driving and retire the external registration; the host must not
    // touch the notification source afterwards.
    void stop_and_detach() {
        ctx_.detach_progress_host();
        nfd_ = -1;
        owner_.reset();
    }

    bool retry_scheduled() const noexcept { return retry_deadline_.has_value(); }
    std::size_t delivered() const noexcept { return delivered_; }
    std::size_t passes() const noexcept { return passes_; }

    // One application event-loop wait. The timeout is the time remaining to
    // the scheduled retry pass when one exists, otherwise the plain horizon,
    // so the host is never in an fd-only wait while a retry obligation
    // stands. A readable fd runs acknowledge plus a bounded immediate drive;
    // an expired retry deadline runs exactly one progress pass. Returns the
    // completions the servicing delivered, or nothing when the horizon
    // expired with no fd event and no retry due.
    std::optional<std::size_t> wait_and_service(std::chrono::milliseconds horizon) {
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            std::chrono::milliseconds timeout = horizon;
            if (retry_deadline_) {
                if (*retry_deadline_ <= now) {
                    timeout = std::chrono::milliseconds::zero();
                } else {
                    const auto until_retry =
                        std::chrono::duration_cast<std::chrono::milliseconds>(*retry_deadline_ -
                                                                              now);
                    if (until_retry < timeout)
                        timeout = until_retry;
                }
            }
            struct pollfd p;
            p.fd = nfd_;
            p.events = POLLIN;
            p.revents = 0;
            const int rc = ::poll(&p, 1, static_cast<int>(timeout.count()));
            if (rc < 0) {
                if (errno == EINTR)
                    continue;
                return std::nullopt;
            }
            const std::size_t before = delivered_;
            if (rc > 0 && (p.revents & POLLIN) != 0) {
                acknowledge_and_drive();
                return delivered_ - before;
            }
            // The wait expired: the retry pass runs when its deadline has
            // passed, including during this very wait; while deadline time
            // remains, keep waiting it down — millisecond truncation makes
            // poll return slightly early. Without a schedule the horizon is
            // out and nothing is due.
            if (retry_deadline_) {
                if (*retry_deadline_ <= std::chrono::steady_clock::now()) {
                    run_retry_pass();
                    return delivered_ - before;
                }
                continue;
            }
            return std::nullopt;
        }
    }

    std::size_t acknowledge_and_drive() {
        ctx_.acknowledge_progress_notification();
        const std::size_t before = delivered_;
        drive_immediate();
        return delivered_ - before;
    }

    // Dispatching a newly accepted request is itself a progress obligation,
    // so the host runs one bounded pass after submitting before it may wait
    // on the notification fd. A retryable transport failure reported by that
    // pass becomes a scheduled continuation, never an fd-only wait. Returns
    // the completions that pass already delivered.
    std::size_t drive_submission() {
        const auto pass = ctx_.poll_progress();
        if (!pass.has_value())
            return 0;
        ++passes_;
        delivered_ += pass.value().completed;
        reconcile_retry_schedule(pass.value());
        return pass.value().completed;
    }

  private:
    static constexpr std::chrono::milliseconds kHostRetryInterval{10};

    void schedule_retry_pass() {
        if (!retry_deadline_)
            retry_deadline_ = std::chrono::steady_clock::now() + kHostRetryInterval;
    }

    void reconcile_retry_schedule(const AsyncBackend::ProgressPass& pass) {
#if defined(SLUICE_B1C_MUTANT_HOST_IGNORES_DISPATCH_RETRY)
        (void)pass;
#else
        if (pass.dispatch_retry_remains)
            schedule_retry_pass();
        else
            retry_deadline_.reset();
#endif
    }

    void drive_immediate() {
        for (int i = 0; i < 64; ++i) {
            const auto pass = ctx_.poll_progress();
            if (!pass.has_value())
                return;
            ++passes_;
            delivered_ += pass.value().completed;
            reconcile_retry_schedule(pass.value());
            if (pass.value().health_failed)
                return;
            if (!pass.value().immediate_work_remains)
                return;
        }
    }

    void run_retry_pass() {
        retry_deadline_.reset();
        const auto pass = ctx_.poll_progress();
        if (!pass.has_value())
            return;
        ++passes_;
        delivered_ += pass.value().completed;
        reconcile_retry_schedule(pass.value());
        if (pass.value().immediate_work_remains)
            drive_immediate();
    }

    AsyncIoContext& ctx_;
    std::optional<ProgressOwner> owner_;
    int nfd_ = -1;
    std::optional<std::chrono::steady_clock::time_point> retry_deadline_;
    std::size_t delivered_ = 0;
    std::size_t passes_ = 0;
};

int temp_file_fd(const std::string& content) {
    char path[] = "/tmp/sluice_uring_extloop_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0)
        return -1;
    ::unlink(path);
    if (!content.empty()) {
        const ssize_t n = ::write(fd, content.data(), content.size());
        if (n != static_cast<ssize_t>(content.size())) {
            ::close(fd);
            return -1;
        }
    }
    ::lseek(fd, 0, SEEK_SET);
    return fd;
}

bool fd_readable(int fd) {
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    const int rc = ::poll(&p, 1, 0);
    return rc > 0 && (p.revents & POLLIN) != 0;
}

// The §18 W-03 shape with a real ring: UringAsyncBackend → AsyncIoContext →
// external poll(2) loop whose poll set contains exactly the context progress
// fd. Multiple reads/writes, a control wake with zero completions, and
// coalesced completions are all served through the documented
// acknowledgement and progress operations. No Scheduler, no Fiber, no ring
// fd, no periodic timer.
bool external_poll_loop_uring_w03() {
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{16, 8});
    AsyncIoContext ctx(std::move(backend));
    ExternalLoopHost host(ctx);
    if (host.nfd() < 0)
        return false;

    const int source = temp_file_fd(std::string(256, 'a'));
    const int sink = temp_file_fd("");
    if (source < 0 || sink < 0)
        return false;

    std::vector<std::byte> r1buf(64, std::byte{0});
    std::vector<std::byte> r2buf(64, std::byte{0});
    std::vector<std::byte> r3buf(64, std::byte{0});
    std::vector<std::byte> wbuf(32, std::byte{'b'});
    Request<std::size_t> r1, r2, r3, w;
    // The public Request binding forbids nonterminal release: failure exits
    // drain their in-flight reads to publication before the bindings go.
    struct RequestPublicationDrain {
        AsyncIoContext& ctx;
        Request<std::size_t>* held[4] = {};
        explicit RequestPublicationDrain(AsyncIoContext& c) : ctx(c) {}
        void track(Request<std::size_t>& r) {
            for (Request<std::size_t>*& slot : held)
                if (slot == nullptr) {
                    slot = &r;
                    return;
                }
        }
        ~RequestPublicationDrain() {
            for (Request<std::size_t>* r : held) {
                if (r == nullptr)
                    continue;
                while (r->valid() && !r->ready())
                    (void)ctx.poll();
                r->discard();
            }
        }
    } drain{ctx};
    drain.track(r1);
    drain.track(r2);
    drain.track(r3);
    drain.track(w);

    auto r1_submitted =
        ctx.submit_read(ReadOp{NativeFileRef(source, FileAccess::read_only), r1buf.data(),
                               r1buf.size(), 0});
    if (!r1_submitted.has_value())
        return false;
    r1 = std::move(r1_submitted).value();
    std::size_t phase_a = host.drive_submission();
    const auto first_wake = host.wait_and_service(std::chrono::milliseconds{5000});
    if (!first_wake)
        return false;
    phase_a += *first_wake;
    phase_a += host.acknowledge_and_drive();
    if (phase_a != 1 || !r1.ready())
        return false;
    r1.discard();
    // A servicing pass consumes the kernel completion silently — the wake
    // was the kernel's own eventfd write at CQE publication — so the fd is
    // already quiet and one settling round delivers nothing.
    if (host.acknowledge_and_drive() != 0)
        return false;
    if (fd_readable(host.nfd()))
        return false;

    // Control wake with zero completions: the host acks, observes the sticky
    // control as a wait outcome, acknowledges it, and keeps serving later
    // real work.
    ctx.interrupt_progress_waiters();
    if (!host.wait_and_service(std::chrono::milliseconds{5000}))
        return false;
    if (host.acknowledge_and_drive() != 0)
        return false;
    {
        using WaitKind = AsyncIoContext::ProgressWaitOutcome::Kind;
        const auto control = ctx.wait_one(std::chrono::milliseconds{100});
        if (!control.has_value() || control.value().kind != WaitKind::control_interrupted)
            return false;
        ctx.acknowledge_progress_control();
        const auto settled = ctx.wait_one(std::chrono::milliseconds{50});
        if (!settled.has_value() || settled.value().kind != WaitKind::progress ||
            settled.value().completed != 0)
            return false;
    }
    if (fd_readable(host.nfd()))
        return false;

    auto r2_submitted =
        ctx.submit_read(ReadOp{NativeFileRef(source, FileAccess::read_only), r2buf.data(),
                               r2buf.size(), 16});
    if (!r2_submitted.has_value())
        return false;
    r2 = std::move(r2_submitted).value();
    auto r3_submitted =
        ctx.submit_read(ReadOp{NativeFileRef(source, FileAccess::read_only), r3buf.data(),
                               r3buf.size(), 64});
    if (!r3_submitted.has_value())
        return false;
    r3 = std::move(r3_submitted).value();
    auto w_submitted =
        ctx.submit_write(WriteOp{NativeFileRef(sink, FileAccess::read_write), wbuf.data(),
                                 wbuf.size(), 0});
    if (!w_submitted.has_value())
        return false;
    w = std::move(w_submitted).value();
    std::size_t batch = host.drive_submission();

    int settled_without_wake = 0;
    for (;;) {
        if (r2.ready() && r3.ready() && w.ready())
            break;
        const auto serviced = host.wait_and_service(std::chrono::milliseconds{5000});
        if (!serviced)
            return false;
        const std::size_t got = *serviced + host.acknowledge_and_drive();
        batch += got;
        if (got == 0) {
            ++settled_without_wake;
            if (settled_without_wake > 8)
                return false;
        }
    }
    if (batch != 3)
        return false;
    for (int i = 0; i < 8 && host.acknowledge_and_drive() > 0; ++i) {
    }
    if (host.acknowledge_and_drive() != 0)
        return false;

    // Quiescence: a genuinely idle notification fd blocks in poll(2).
    if (fd_readable(host.nfd()))
        return false;
    struct pollfd p;
    p.fd = host.nfd();
    p.events = POLLIN;
    p.revents = 0;
    if (::poll(&p, 1, 150) != 0)
        return false;

    const bool settled = r2.ready() && r3.ready() && w.ready();
    r2.discard();
    r3.discard();
    w.discard();
    ::close(source);
    ::close(sink);
    // Shutdown order: all accepted work settled, only then does the host
    // retire its registration and release the retained state. A retired
    // registration is gone (a second detach has nothing to retire), but the
    // context may lend the fd to a later host.
    const int borrowed = host.nfd();
    host.stop_and_detach();
    if (!settled)
        return false;
    ctx.detach_progress_host();
    const int relent = ctx.progress_notification_fd();
    ctx.detach_progress_host();
    return relent == borrowed;
}

// Kernel completions are all discovered through the notification-fd wake
// loop: no completion strands, the wake count stays bounded, and the fd
// returns to quiet after one settling round.
bool kernel_completions_delivered_without_stranding() {
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{16, 8});
    AsyncIoContext ctx(std::move(backend));
    ExternalLoopHost host(ctx);

    const int source = temp_file_fd(std::string(256, 'a'));
    if (source < 0)
        return false;

    constexpr int kCoalesced = 4;
    std::vector<std::vector<std::byte>> bufs(kCoalesced);
    std::vector<Request<std::size_t>> completions(kCoalesced);
    struct RequestPublicationDrain {
        AsyncIoContext& ctx;
        std::vector<Request<std::size_t>>& held;
        explicit RequestPublicationDrain(AsyncIoContext& c, std::vector<Request<std::size_t>>& rs)
            : ctx(c), held(rs) {}
        ~RequestPublicationDrain() {
            for (auto& r : held) {
                while (r.valid() && !r.ready())
                    (void)ctx.poll();
                r.discard();
            }
        }
    } drain{ctx, completions};
    for (int i = 0; i < kCoalesced; ++i) {
        bufs[static_cast<std::size_t>(i)].assign(64, std::byte{0});
        auto submitted = ctx.submit_read(
            ReadOp{NativeFileRef(source, FileAccess::read_only),
                   bufs[static_cast<std::size_t>(i)].data(),
                   bufs[static_cast<std::size_t>(i)].size(),
                   static_cast<std::uint64_t>(i) * 32});
        if (!submitted.has_value())
            return false;
        completions[static_cast<std::size_t>(i)] = std::move(submitted).value();
    }
    std::size_t delivered = host.drive_submission();

    int wakes = 0;
    while (delivered < static_cast<std::size_t>(kCoalesced)) {
        const auto serviced = host.wait_and_service(std::chrono::milliseconds{2000});
        if (!serviced)
            return false;
        ++wakes;
        if (wakes > 4 * kCoalesced)
            return false;
        delivered += *serviced;
        delivered += host.acknowledge_and_drive();
    }

    if (host.acknowledge_and_drive() != 0)
        return false;
    bool ok = !fd_readable(host.nfd());
    for (auto& c : completions) {
        ok = ok && c.ready();
        c.discard();
    }
    ::close(source);
    host.stop_and_detach();
    return ok;
}

// A zero-length operation completes inline at submission with no kernel CQE;
// the pending transition still signals the context notification fd, and the
// external host services it with no ring involvement.
bool external_host_sees_userspace_publication_wake() {
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{8, 8});
    AsyncIoContext ctx(std::move(backend));
    ExternalLoopHost host(ctx);

    std::vector<std::byte> buffer(4, std::byte{0});
    auto zero_submitted =
        ctx.submit_read(ReadOp{NativeFileRef(::fileno(tmpfile()), FileAccess::read_only),
                               buffer.data(), 0, 0});
    if (!zero_submitted.has_value())
        return false;
    auto zero = std::move(zero_submitted).value();
    if (!fd_readable(host.nfd()))
        return false;
    if (host.drive_submission() != 1)
        return false;
    if (!zero.ready())
        return false;
    if (host.acknowledge_and_drive() != 0)
        return false;
    zero.discard();
    const bool quiet = !fd_readable(host.nfd());
    host.stop_and_detach();
    return quiet;
}

// A saturated notification fd keeps asserting readiness across a real kernel
// completion; the drain re-arms through the kernel's own completion write,
// the completions survive, and one settling round returns the fd to quiet.
bool saturation_with_real_kernel_completion_preserves_wake() {
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{8, 8});
    AsyncIoContext ctx(std::move(backend));
    ExternalLoopHost host(ctx);

    ctx.saturate_progress_notification_for_test();
    if (!fd_readable(host.nfd()))
        return false;

    const int source = temp_file_fd(std::string(64, 's'));
    if (source < 0)
        return false;
    std::vector<std::byte> buf(32, std::byte{0});
    struct RequestPublicationDrain {
        AsyncIoContext& ctx;
        Request<std::size_t>& held;
        explicit RequestPublicationDrain(AsyncIoContext& c, Request<std::size_t>& r)
            : ctx(c), held(r) {}
        ~RequestPublicationDrain() {
            while (held.valid() && !held.ready())
                (void)ctx.poll();
            held.discard();
        }
    };
    auto c_submitted =
        ctx.submit_read(ReadOp{NativeFileRef(source, FileAccess::read_only), buf.data(),
                               buf.size(), 0});
    if (!c_submitted.has_value())
        return false;
    auto c = std::move(c_submitted).value();
    RequestPublicationDrain drain{ctx, c};
    std::size_t delivered = host.drive_submission();

    int rounds = 0;
    while (delivered < 1 && rounds < 8) {
        ++rounds;
        const auto serviced = host.wait_and_service(std::chrono::milliseconds{2000});
        if (!serviced)
            return false;
        delivered += *serviced;
        delivered += host.acknowledge_and_drive();
    }
    if (delivered != 1 || !c.ready())
        return false;
    c.discard();
    ::close(source);

    if (host.acknowledge_and_drive() != 0)
        return false;
    if (fd_readable(host.nfd()))
        return false;
    struct pollfd p;
    p.fd = host.nfd();
    p.events = POLLIN;
    p.revents = 0;
    const bool idle = ::poll(&p, 1, 150) == 0;
    host.stop_and_detach();
    return idle;
}

// A retryable transport failure persisting across the acceptance, the first
// fd wake and two scheduled retries: the host must hold an
// obligation-scoped one-shot retry schedule and never fall back to an
// fd-only wait, or the request strands outside the kernel with no CQE and
// no notification to ever wake the park. The stale host shape — park once
// neither completed work nor immediate work remains — is the M-C9 mutant:
// its schedule never arms, the attempts freeze after the wake's drive, and
// this test fails with the transport still outside the kernel.
struct PersistentEagainState {
    std::atomic<int> attempts{0};
};

int persistent_eagain_submit_hook(void* context, ::io_uring* ring) noexcept {
    auto* state = static_cast<PersistentEagainState*>(context);
    if (state->attempts.fetch_add(1, std::memory_order_relaxed) + 1 < 5)
        return -EAGAIN;
    return ::io_uring_submit(ring);
}

bool external_retryable_transport_does_not_strand() {
    PersistentEagainState state;
    UringBackendSubmitTestHooks hooks;
    hooks.context = &state;
    hooks.submit = &persistent_eagain_submit_hook;
    auto backend = std::make_unique<UringAsyncBackend>(UringConfig{16, 8}, hooks);
    AsyncIoContext ctx(std::move(backend));
    ExternalLoopHost host(ctx);
    if (host.nfd() < 0)
        return false;

    const int source = temp_file_fd(std::string(256, 'e'));
    if (source < 0)
        return false;
    std::vector<std::byte> buf(32, std::byte{0});
    Request<std::size_t> c;

    // Retires the accepted request on defect exits so teardown stays clean;
    // on the success path the completion is already ready and this no-ops.
    struct RetireOnExit {
        AsyncIoContext& ctx;
        Request<std::size_t>& completion;
        int fd;
        ~RetireOnExit() {
            for (int i = 0; i < 4000 && completion.valid() && !completion.ready(); ++i) {
                (void)ctx.poll_progress();
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            completion.discard();
            ::close(fd);
        }
    } retire_on_exit{ctx, c, source};

    // The dispatch-time submit (attempt 1) fails retryably at acceptance and
    // the retained transport signals the context fd; the post-submission
    // drive (attempt 2) fails again, so the host must already hold the
    // schedule before it ever waits.
    auto c_submitted =
        ctx.submit_read(ReadOp{NativeFileRef(source, FileAccess::read_only), buf.data(),
                               buf.size(), 0});
    if (!c_submitted.has_value())
        return false;
    c = std::move(c_submitted).value();
    if (host.drive_submission() != 0)
        return false;
    if (!host.retry_scheduled())
        return false;

    // First wake: the acknowledgement drains the acceptance notification,
    // the drive's pass fails retryably a third time, and the completion is
    // still absent — the obligation must stand and the schedule with it.
    if (!host.wait_and_service(std::chrono::milliseconds{5000}))
        return false;
    if (state.attempts.load(std::memory_order_relaxed) < 3)
        return false;
    if (c.ready())
        return false;
    if (!host.retry_scheduled())
        return false;

    // Each scheduled retry runs exactly one pass and re-arms only while the
    // pass still reports the obligation; the success pass submits the
    // retained transport and the real CQE wakes the context fd.
    for (int i = 0; i < 64 && !c.ready(); ++i) {
        if (!host.wait_and_service(std::chrono::milliseconds{5000}))
            return false;
    }
    if (!c.ready())
        return false;

    const int attempts = state.attempts.load(std::memory_order_relaxed);
    const bool ok = attempts == 5 && host.delivered() >= 1 && host.passes() <= 12;
    if (host.acknowledge_and_drive() != 0)
        return false;
    if (host.retry_scheduled())
        return false;
    if (fd_readable(host.nfd()))
        return false;
    const std::size_t quiet_passes = host.passes();
    struct pollfd quiet;
    quiet.fd = host.nfd();
    quiet.events = POLLIN;
    quiet.revents = 0;
    if (::poll(&quiet, 1, 150) != 0)
        return false;
    if (host.passes() != quiet_passes)
        return false;
    host.stop_and_detach();
    return ok;
}

}

int main() {
    ::alarm(120);
    struct NamedTest {
        const char* name;
        bool (*fn)();
    };
    const NamedTest tests[] = {
        {"external_poll_loop_uring_w03", external_poll_loop_uring_w03},
        {"kernel_completions_delivered_without_stranding",
         kernel_completions_delivered_without_stranding},
        {"external_host_sees_userspace_publication_wake",
         external_host_sees_userspace_publication_wake},
        {"saturation_with_real_kernel_completion_preserves_wake",
         saturation_with_real_kernel_completion_preserves_wake},
        {"external_retryable_transport_does_not_strand",
         external_retryable_transport_does_not_strand},
    };

    std::size_t passed = 0;
    for (const NamedTest& t : tests) {
        if (!t.fn()) {
            std::fprintf(stderr, "FAIL: %s\n", t.name);
            return 1;
        }
        ++passed;
        std::printf("ok %s\n", t.name);
        std::fflush(stdout);
    }
    std::printf("all %zu uring external loop tests passed\n", passed);
    return 0;
}
