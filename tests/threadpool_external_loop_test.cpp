#include <sluice/async/async_io_context.hpp>
#include <sluice/async/batch.hpp>
#include <sluice/async/op_helpers.hpp>
#include <sluice/async/threadpool_backend.hpp>
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

#include <csignal>
#include <sys/wait.h>

namespace {

using namespace sluice::async;
using sluice::FileAccess;

// An application-owned event loop and the context's single pinned progress
// owner: registration uses the context notification fd with real poll(2); a
// readable fd is acknowledged through the documented context operation and
// followed by bounded progress passes that consume the full pass report. A
// dispatch retry obligation would require a scheduled continuation instead of
// an fd-only wait; this backend's passes never report one, so no retry
// schedule exists here.
class ExternalLoopHost {
  public:
    explicit ExternalLoopHost(AsyncIoContext& ctx) : ctx_(ctx) {
        auto claimed = ctx.claim_progress_owner();
        if (!claimed.has_value())
            return;
        owner_ = std::move(claimed).value();
        nfd_ = ctx.progress_notification_fd();
    }

    bool pinned() const noexcept { return nfd_ >= 0; }

    int nfd() const noexcept { return nfd_; }

    // Blocks in poll(2) until the notification fd is readable or the watchdog
    // budget expires. Returns true when the fd was readable.
    bool wait_readable(std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        for (;;) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0)
                return false;
            struct pollfd p;
            p.fd = nfd_;
            p.events = POLLIN;
            p.revents = 0;
            const int rc = ::poll(&p, 1, static_cast<int>(remaining.count()));
            if (rc < 0) {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (rc == 0)
                continue;
            return (p.revents & POLLIN) != 0;
        }
    }

    // The documented wake handling: acknowledge stale readiness, then run
    // bounded progress passes until neither completed work nor immediate
    // work remains. This backend cannot leave a dispatch retry obligation,
    // so that pass field never alters the loop here.
    std::size_t acknowledge_and_drive() {
        ctx_.acknowledge_progress_notification();
        std::size_t delivered = 0;
        for (int i = 0; i < 64; ++i) {
            const auto pass = ctx_.poll_progress();
            if (!pass.has_value())
                break;
            delivered += pass.value().completed;
            if (pass.value().health_failed)
                break;
            if (pass.value().completed == 0 && !pass.value().immediate_work_remains)
                break;
        }
        return delivered;
    }

    // Stop driving and retire the external registration: the supported
    // shutdown order settles accepted work first, unregisters the fd from
    // the loop, then detaches, then stops touching the notification source.
    void stop_and_detach() {
        ctx_.detach_progress_host();
        nfd_ = -1;
        owner_.reset();
    }

  private:
    AsyncIoContext& ctx_;
    std::optional<ProgressOwner> owner_;
    int nfd_ = -1;
};

int temp_file_fd(const std::string& content) {
    char path[] = "/tmp/sluice_tp_extloop_XXXXXX";
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

bool host_readable_sleep(AsyncIoContext& ctx, std::chrono::milliseconds budget);

Request<std::size_t> submit_zero_op(AsyncIoContext& ctx) {
    std::vector<std::byte> buffer(4, std::byte{0});
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef(::fileno(tmpfile()), FileAccess::read_only),
                               buffer.data(), 0, 0});
    if (!submitted.has_value())
        return {};
    return std::move(submitted).value();
}

// The public Request binding fail-fasts on a nonterminal release, so a
// test-local binding that may still be in flight is drained to publication
// before it is released. Driving goes through the test's own progress-owner
// path (a pass from a foreign thread is rejected) and is bounded: an
// unpublishable binding reaches discard() as a contract violation instead of
// hanging the test.
template <class Drive> struct RequestPublicationDrain {
    explicit RequestPublicationDrain(Drive drive) : drive_(drive) {}

    void track(Request<std::size_t>& request) { held_.push_back(&request); }
    void track(std::vector<Request<std::size_t>>& requests) {
        for (Request<std::size_t>& request : requests)
            held_.push_back(&request);
    }
    void release_owner_before_draining(std::optional<ProgressOwner>* owner) { owner_ = owner; }

    ~RequestPublicationDrain() {
        if (owner_ != nullptr)
            owner_->reset();
        for (Request<std::size_t>* request : held_) {
            for (int i = 0; i < kDrainAttempts && request->valid() && !request->ready(); ++i)
                drive_();
            request->discard();
        }
    }

  private:
    static constexpr int kDrainAttempts = 200000;
    Drive drive_;
    std::optional<ProgressOwner>* owner_ = nullptr;
    std::vector<Request<std::size_t>*> held_;
};

// Releases a paused worker gate at scope exit so a drain in a later-declared
// guard can reach publication; the explicit release mid-test is idempotent
// with this one.
struct WorkerGateReleaseOnExit {
    ThreadPoolBackend* backend = nullptr;
    ThreadPoolBackend::WorkerClaimedPauseGate* gate = nullptr;
    ~WorkerGateReleaseOnExit() {
        if (backend != nullptr)
            backend->set_worker_claimed_pause_gate(nullptr);
        if (gate != nullptr) {
            gate->resume.store(true, std::memory_order_release);
            gate->resume.notify_all();
        }
    }
};

// Each violating scenario runs in its own forked child, freshly constructed
// after the fork, so the parent never holds a violating object and the
// child's always-on diagnostic is the only way the scenario can end.
bool child_dies_running(void (*scenario)()) {
    const pid_t pid = ::fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        ::alarm(30);
        scenario();
        std::_Exit(0);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid)
        return false;
    return WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0);
}

// The §24 acceptance shape: ThreadPoolBackend → AsyncIoContext → external
// poll(2) loop → progress_notification_fd → documented ack/progress
// operations, with multiple requests, a sticky control wake, and coalesced
// completions; no Scheduler, no Fiber, no condition variable, no periodic
// timer.
bool external_poll_loop_threadpool_w03() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{16, 2});
    AsyncIoContext ctx(std::move(backend));
    ExternalLoopHost host(ctx);
    if (!host.pinned())
        return false;

    const int source = temp_file_fd(std::string(256, 'a'));
    const int sink = temp_file_fd("");
    if (source < 0 || sink < 0)
        return false;

    // Application-owned retained state: buffers and requests stay alive for
    // the whole loop.
    std::vector<std::byte> r1buf(64, std::byte{0});
    std::vector<std::byte> r2buf(64, std::byte{0});
    std::vector<std::byte> r3buf(64, std::byte{0});
    std::vector<std::byte> wbuf(32, std::byte{'b'});
    Request<std::size_t> r1, r2, r3, w;
    RequestPublicationDrain drain{[&host] { (void)host.acknowledge_and_drive(); }};
    drain.track(r1);
    drain.track(r2);
    drain.track(r3);
    drain.track(w);

    auto r1_submitted = ctx.submit_read(ReadOp{NativeFileRef(source, FileAccess::read_only),
                                               r1buf.data(), r1buf.size(), 0});
    if (!r1_submitted.has_value())
        return false;
    r1 = std::move(r1_submitted).value();
    if (!host.wait_readable(std::chrono::milliseconds{5000}))
        return false;
    const std::size_t phase_a = host.acknowledge_and_drive();
    if (phase_a != 1 || !r1.ready())
        return false;

    // A control wake carries no completion; the host observes the sticky
    // control as a wait outcome, acknowledges it, and only then may later
    // waits proceed.
    ctx.interrupt_progress_waiters();
    if (!host.wait_readable(std::chrono::milliseconds{5000}))
        return false;
    if (host.acknowledge_and_drive() != 0)
        return false;
    using WaitKind = AsyncIoContext::ProgressWaitOutcome::Kind;
    {
        const auto control = ctx.wait_one(std::chrono::milliseconds{100});
        if (!control.has_value() || control.value().kind != WaitKind::control_interrupted)
            return false;
    }
    ctx.acknowledge_progress_control();
    {
        const auto settled = ctx.wait_one(std::chrono::milliseconds{50});
        if (!settled.has_value() || settled.value().kind != WaitKind::progress ||
            settled.value().completed != 0)
            return false;
    }
    if (fd_readable(host.nfd()))
        return false;

    Request<std::size_t> z1 = submit_zero_op(ctx);
    Request<std::size_t> z2 = submit_zero_op(ctx);
    Request<std::size_t> z3 = submit_zero_op(ctx);
    drain.track(z1);
    drain.track(z2);
    drain.track(z3);
    if (!z1.valid() || !z2.valid() || !z3.valid())
        return false;
    if (!host.wait_readable(std::chrono::milliseconds{5000}))
        return false;
    if (host.acknowledge_and_drive() != 3)
        return false;
    if (!z1.ready() || !z2.ready() || !z3.ready())
        return false;
    z1.discard();
    z2.discard();
    z3.discard();

    auto r2_submitted = ctx.submit_read(ReadOp{NativeFileRef(source, FileAccess::read_only),
                                               r2buf.data(), r2buf.size(), 16});
    if (!r2_submitted.has_value())
        return false;
    r2 = std::move(r2_submitted).value();
    auto r3_submitted = ctx.submit_read(ReadOp{NativeFileRef(source, FileAccess::read_only),
                                               r3buf.data(), r3buf.size(), 64});
    if (!r3_submitted.has_value())
        return false;
    r3 = std::move(r3_submitted).value();
    auto w_submitted = ctx.submit_write(
        WriteOp{NativeFileRef(sink, FileAccess::read_write), wbuf.data(), wbuf.size(), 0});
    if (!w_submitted.has_value())
        return false;
    w = std::move(w_submitted).value();

    Request<std::size_t> late;
    drain.track(late);
    std::thread helper([&] {
        // Submit from application-observed state, not from the notification
        // fd: the host and this helper must not compete for the shared
        // level-triggered readiness.
        for (int i = 0; i < 20000000; ++i) {
            if (r2.ready() && r3.ready() && w.ready())
                break;
            std::this_thread::yield();
        }
        late = submit_zero_op(ctx);
    });
    struct JoinOnExit {
        std::thread& t;
        ~JoinOnExit() {
            if (t.joinable())
                t.join();
        }
    } join_helper{helper};

    int settled_without_wake = 0;
    for (;;) {
        if (r2.ready() && r3.ready() && w.ready() && late.ready())
            break;
        if (!host.wait_readable(std::chrono::milliseconds{5000}))
            return false;
        const std::size_t delivered = host.acknowledge_and_drive();
        if (delivered == 0) {
            ++settled_without_wake;
            if (settled_without_wake > 8)
                return false;
        }
    }
    for (int i = 0; i < 8 && host.acknowledge_and_drive() > 0; ++i) {
    }
    if (host.acknowledge_and_drive() != 0)
        return false;

    // Quiescence: a genuinely idle fd is not readable and blocks.
    if (fd_readable(host.nfd()))
        return false;
    struct pollfd p;
    p.fd = host.nfd();
    p.events = POLLIN;
    p.revents = 0;
    if (::poll(&p, 1, 150) != 0)
        return false;

    const bool settled = r1.ready() && r2.ready() && r3.ready() && w.ready() && late.ready();
    r1.discard();
    r2.discard();
    r3.discard();
    w.discard();
    late.discard();
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

// One readable event may stand for several signals: five coalesced zero-op
// completions are all discovered through a single wake.
bool coalesced_signals_delivered_through_one_wake() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{16, 1});
    AsyncIoContext ctx(std::move(backend));
    ExternalLoopHost host(ctx);
    if (!host.pinned())
        return false;

    Request<std::size_t> warm = submit_zero_op(ctx);
    RequestPublicationDrain warm_drain{
        [&host] { (void)host.acknowledge_and_drive(); }};
    warm_drain.track(warm);
    if (!warm.valid())
        return false;
    if (!host.wait_readable(std::chrono::milliseconds{5000}))
        return false;
    if (host.acknowledge_and_drive() != 1)
        return false;
    warm.discard();
    if (fd_readable(host.nfd()))
        return false;

    constexpr int kCoalesced = 5;
    std::vector<Request<std::size_t>> zeros(kCoalesced);
    RequestPublicationDrain zeros_drain{
        [&host] { (void)host.acknowledge_and_drive(); }};
    zeros_drain.track(zeros);
    for (Request<std::size_t>& zero : zeros) {
        zero = submit_zero_op(ctx);
        if (!zero.valid())
            return false;
    }

    // The counter holds every coalesced signal behind one readable state.
    if (!fd_readable(host.nfd()))
        return false;
    if (host.acknowledge_and_drive() != static_cast<std::size_t>(kCoalesced))
        return false;
    bool ok = !fd_readable(host.nfd());
    for (auto& z : zeros) {
        ok = ok && z.ready();
        z.discard();
    }
    host.stop_and_detach();
    return ok;
}

// A wake that carries no completion must not confuse the host: the loop acks,
// observes sticky control, acknowledges it, drives an empty pass, and keeps
// serving later real work.
bool spurious_wake_is_harmless_and_completions_survive() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{8, 1});
    AsyncIoContext ctx(std::move(backend));
    ExternalLoopHost host(ctx);
    if (!host.pinned())
        return false;

    ctx.interrupt_progress_waiters();
    if (!host.wait_readable(std::chrono::milliseconds{5000}))
        return false;
    if (host.acknowledge_and_drive() != 0)
        return false;
    {
        using WaitKind = AsyncIoContext::ProgressWaitOutcome::Kind;
        const auto control = ctx.wait_one(std::chrono::milliseconds{100});
        if (!control.has_value() || control.value().kind != WaitKind::control_interrupted)
            return false;
    }
    ctx.acknowledge_progress_control();

    std::vector<std::byte> buf(32, std::byte{0});
    const int fd = temp_file_fd(std::string(64, 'c'));
    auto c_submitted = ctx.submit_read(
        ReadOp{NativeFileRef(fd, FileAccess::read_only), buf.data(), buf.size(), 0});
    if (!c_submitted.has_value())
        return false;
    Request<std::size_t> c = std::move(c_submitted).value();
    RequestPublicationDrain drain{[&host] { (void)host.acknowledge_and_drive(); }};
    drain.track(c);
    if (!host.wait_readable(std::chrono::milliseconds{5000}))
        return false;
    if (host.acknowledge_and_drive() != 1)
        return false;

    const bool ok = c.ready();
    c.discard();
    host.stop_and_detach();
    return ok;
}

// A saturated notification fd keeps asserting readiness: the next signal
// observes EAGAIN, the host still wakes, and one acknowledgement drains it.
bool saturated_notification_preserves_wake() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{8, 1});
    AsyncIoContext ctx(std::move(backend));
    ExternalLoopHost host(ctx);
    if (!host.pinned())
        return false;

    ctx.saturate_progress_notification_for_test();
    if (!fd_readable(host.nfd()))
        return false;

    Request<std::size_t> zero = submit_zero_op(ctx);
    RequestPublicationDrain drain{[&host] { (void)host.acknowledge_and_drive(); }};
    drain.track(zero);
    if (!zero.valid())
        return false;
    if (!host.wait_readable(std::chrono::milliseconds{5000}))
        return false;
    if (host.acknowledge_and_drive() != 1)
        return false;
    if (!zero.ready())
        return false;
    zero.discard();

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

// R14: the host acknowledges stale readiness while a new ThreadPool progress
// signal races it; the obligation is either serviced by the post-ack pass or
// re-asserts readiness — it can never strand.
bool host_acknowledgement_racing_new_signal_never_strands() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{8, 1});
    AsyncIoContext ctx(std::move(backend));
    ExternalLoopHost host(ctx);
    if (!host.pinned())
        return false;

    Request<std::size_t> warm = submit_zero_op(ctx);
    RequestPublicationDrain drain{[&host] { (void)host.acknowledge_and_drive(); }};
    drain.track(warm);
    if (!warm.valid())
        return false;
    if (!host.wait_readable(std::chrono::milliseconds{5000}))
        return false;
    if (host.acknowledge_and_drive() != 1)
        return false;
    warm.discard();
    if (fd_readable(host.nfd()))
        return false;

    std::atomic<bool> acked{false};
    Request<std::size_t> racing;
    RequestPublicationDrain racing_drain{
        [&host] { (void)host.acknowledge_and_drive(); }};
    racing_drain.track(racing);
    std::thread signaler([&] {
        while (!acked.load(std::memory_order_acquire))
            std::this_thread::yield();
        racing = submit_zero_op(ctx);
    });

    // Acknowledge stale readiness (nothing pending here, so this is the
    // host-side drain racing the imminent signal).
    acked.store(true, std::memory_order_release);
    ctx.acknowledge_progress_notification();

    int wakes = 0;
    bool settled = false;
    while (wakes < 4) {
        if (!fd_readable(host.nfd())) {
            if (racing.ready()) {
                settled = true;
                break;
            }
            struct pollfd p;
            p.fd = host.nfd();
            p.events = POLLIN;
            p.revents = 0;
            const int rc = ::poll(&p, 1, 2000);
            if (rc <= 0)
                break;
        }
        ++wakes;
        if (host.acknowledge_and_drive() > 0 && racing.ready()) {
            settled = true;
            break;
        }
    }
    signaler.join();

    // Every exit path must leave no delayed zero-op obligation behind: the
    // drain-wins interleaving can observe the completion before any pass ran.
    for (int i = 0; i < 8 && host.acknowledge_and_drive() > 0; ++i) {
    }

    const bool ok = settled && racing.ready() && wakes <= 2;
    racing.discard();
    host.stop_and_detach();
    return ok;
}

// C2-D stop/settle/detach: the supported host shutdown order is stop
// admitting, settle every accepted request, then retire the registration. A
// second detach is an idempotent no-op and the context lends nothing after
// detachment.
bool host_stop_settle_detach_retires_registration() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{8, 2});
    AsyncIoContext ctx(std::move(backend));
    ExternalLoopHost host(ctx);
    if (!host.pinned())
        return false;

    std::vector<std::byte> buf(32, std::byte{0});
    Request<std::size_t> a = submit_zero_op(ctx);
    Request<std::size_t> b = submit_zero_op(ctx);
    RequestPublicationDrain drain{[&host] { (void)host.acknowledge_and_drive(); }};
    drain.track(a);
    drain.track(b);
    if (!a.valid() || !b.valid())
        return false;
    while (!a.ready() || !b.ready()) {
        if (!host.wait_readable(std::chrono::milliseconds{5000}))
            return false;
        host.acknowledge_and_drive();
    }
    a.discard();
    b.discard();
    // Discharge the delayed control/reclaim obligations of the settled
    // zero-op requests before retiring the registration.
    for (int i = 0; i < 8 && host.acknowledge_and_drive() > 0; ++i) {
    }
    if (host.acknowledge_and_drive() != 0)
        return false;

    host.stop_and_detach();
    ctx.detach_progress_host();
    return true;
}

// After detach, the retired registration grants no authority over a later
// context, even when the kernel reuses the numeric descriptor. The new
// context's registration is its own: a fresh borrow, its own wake, its own
// detach.
bool stale_detached_registration_cannot_authorize_a_new_context() {
    int reused_fd = -1;
    {
        auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
        AsyncIoContext ctx(std::move(backend));
        const int borrowed = ctx.progress_notification_fd();
        if (borrowed < 0)
            return false;
        ctx.interrupt_progress_waiters();
        if (!fd_readable(borrowed))
            return false;
        ctx.detach_progress_host();
    }
    {
        auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
        AsyncIoContext ctx(std::move(backend));
        const int fresh = ctx.progress_notification_fd();
        if (fresh < 0)
            return false;
        // Descriptor reuse is expected here but not contractually required;
        // the authority separation is what the contract requires.
        ctx.interrupt_progress_waiters();
        if (!fd_readable(fresh))
            return false;
        ctx.acknowledge_progress_notification();
        if (fd_readable(fresh))
            return false;
        ctx.detach_progress_host();
        reused_fd = fresh;
    }
    return reused_fd >= 0;
}

void scenario_move_with_live_owner_capability() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncIoContext ctx(std::move(backend));
    auto owner = ctx.claim_progress_owner();
    if (!owner.has_value())
        return;
    AsyncIoContext moved(std::move(ctx));
    (void)moved;
}

void scenario_destroy_with_live_owner_capability() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    auto* ctx = new AsyncIoContext(std::move(backend));
    auto owner = ctx->claim_progress_owner();
    if (!owner.has_value()) {
        delete ctx;
        return;
    }
    // The context dies while the owner handle is live; the handle's own
    // destructor would only release a claim on freed storage afterwards.
    delete ctx;
}

void scenario_destroy_with_live_interest() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    auto* ctx = new AsyncIoContext(std::move(backend));
    (void)ctx->progress_notification_fd();
    // The context dies while its notification fd is still registered with an
    // external event loop: the registration was never retired.
    delete ctx;
}

bool contract_violations_fail_fast() {
    if (!child_dies_running(scenario_move_with_live_owner_capability))
        return false;
    if (!child_dies_running(scenario_destroy_with_live_owner_capability))
        return false;
    return child_dies_running(scenario_destroy_with_live_interest);
}

// A pinned context rejects every driving attempt from another thread while
// the owner capability is live, and exactly one capability can exist.
bool pinned_context_rejects_foreign_drivers() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{8, 2});
    AsyncIoContext ctx(std::move(backend));
    auto claimed = ctx.claim_progress_owner();
    if (!claimed.has_value())
        return false;
    std::optional<ProgressOwner> owner = std::move(claimed).value();
    if (ctx.claim_progress_owner().has_value())
        return false;

    Request<std::size_t> foreign_completion;
    RequestPublicationDrain drain{[&ctx] { (void)ctx.poll(); }};
    drain.track(foreign_completion);
    drain.release_owner_before_draining(&owner);
    std::atomic<bool> foreign_drive_rejected{false};
    std::atomic<bool> foreign_wait_rejected{false};
    std::atomic<bool> foreign_submit_accepted{false};
    std::thread foreign([&] {
        const auto pass = ctx.poll_progress();
        foreign_drive_rejected.store(!pass.has_value() &&
                                         pass.error().code == sluice::IoError::Code::invalid_state,
                                     std::memory_order_release);
        const auto wr = ctx.wait_one(std::chrono::milliseconds{100});
        foreign_wait_rejected.store(!wr.has_value() &&
                                        wr.error().code == sluice::IoError::Code::invalid_state,
                                    std::memory_order_release);
        // Submission stays legal, but the submitter is not the driver: it
        // must not consume the request while the owner drives.
        foreign_completion = submit_zero_op(ctx);
        foreign_submit_accepted.store(foreign_completion.valid(), std::memory_order_release);
    });
    foreign.join();

    const bool ok = foreign_drive_rejected.load() && foreign_wait_rejected.load() &&
                    foreign_submit_accepted.load();
    // The owner drains the helper's accepted zero-op itself.
    if (!host_readable_sleep(ctx, std::chrono::milliseconds{5000}))
        return false;
    ctx.acknowledge_progress_notification();
    const auto pass = ctx.poll_progress();
    if (!pass.has_value() || pass.value().completed != 1) {
        owner.reset();
        ctx.detach_progress_host();
        return false;
    }
    owner.reset();
    ctx.detach_progress_host();
    return ok;
}

bool host_readable_sleep(AsyncIoContext& ctx, std::chrono::milliseconds budget) {
    const int fd = ctx.progress_notification_fd();
    if (fd < 0)
        return false;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        if (fd_readable(fd))
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
}

// Deadline expiry is only a wait bound: it reports deadline_expired, leaves
// the accepted request outstanding, and a later wait completes it.
bool deadline_expiry_does_not_cancel_or_settle() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    auto claimed = ctx.claim_progress_owner();
    if (!claimed.has_value())
        return false;
    std::optional<ProgressOwner> owner = std::move(claimed).value();

    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    raw->set_worker_claimed_pause_gate(&gate);

    const int fd = temp_file_fd(std::string(64, 'd'));
    std::vector<std::byte> buf(32, std::byte{0});
    auto c_submitted = ctx.submit_read(
        ReadOp{NativeFileRef(fd, FileAccess::read_only), buf.data(), buf.size(), 0});
    if (!c_submitted.has_value())
        return false;
    Request<std::size_t> c = std::move(c_submitted).value();
    RequestPublicationDrain drain{[&ctx] { (void)ctx.poll(); }};
    drain.track(c);
    drain.release_owner_before_draining(&owner);
    WorkerGateReleaseOnExit gate_release{raw, &gate};

    using WaitKind = AsyncIoContext::ProgressWaitOutcome::Kind;
    const auto first = ctx.wait_one(std::chrono::milliseconds{100});
    if (!first.has_value() || first.value().kind != WaitKind::deadline_expired)
        return false;
    if (first.value().completed != 0)
        return false;
    if (c.ready())
        return false;

    raw->set_worker_claimed_pause_gate(nullptr);
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();

    const auto second = ctx.wait_one(std::chrono::milliseconds{5000});
    if (!second.has_value() || second.value().kind != WaitKind::progress ||
        second.value().completed != 1)
        return false;
    const bool ready = c.ready();
    c.discard();
    return ready;
}

// The unbounded wait distinguishes an idle context (progress with zero
// completions) from control, and control stays pending across waits until the
// owner acknowledges it.
bool idle_and_control_outcomes_are_distinct_and_sticky() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncIoContext ctx(std::move(backend));
    auto claimed = ctx.claim_progress_owner();
    if (!claimed.has_value())
        return false;
    std::optional<ProgressOwner> owner = std::move(claimed).value();

    using WaitKind = AsyncIoContext::ProgressWaitOutcome::Kind;
    const auto idle = ctx.wait_one(std::chrono::milliseconds{50});
    if (!idle.has_value() || idle.value().kind != WaitKind::progress ||
        idle.value().completed != 0)
        return false;

    ctx.interrupt_progress_waiters();
    for (int round = 0; round < 3; ++round) {
        const auto control = ctx.wait_one(std::chrono::milliseconds{50});
        if (!control.has_value() || control.value().kind != WaitKind::control_interrupted)
            return false;
    }
    ctx.acknowledge_progress_control();
    const auto after = ctx.wait_one(std::chrono::milliseconds{50});
    if (!after.has_value() || after.value().kind != WaitKind::progress ||
        after.value().completed != 0)
        return false;
    owner.reset();
    return true;
}

// Control arriving before the wait invocation is not lost, and notification
// acknowledgement does not erase unacknowledged control.
bool control_before_wait_and_across_notification_ack_survives() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncIoContext ctx(std::move(backend));
    auto claimed = ctx.claim_progress_owner();
    if (!claimed.has_value())
        return false;
    std::optional<ProgressOwner> owner = std::move(claimed).value();
    const int fd = ctx.progress_notification_fd();
    if (fd < 0)
        return false;

    ctx.interrupt_progress_waiters();
    ctx.acknowledge_progress_notification();

    using WaitKind = AsyncIoContext::ProgressWaitOutcome::Kind;
    const auto r = ctx.wait_one(std::chrono::milliseconds{100});
    if (!r.has_value() || r.value().kind != WaitKind::control_interrupted)
        return false;

    // Progress and control can coalesce in one wake; the control report wins
    // and the next wait proceeds to the idle outcome normally.
    ctx.acknowledge_progress_control();
    const auto settled = ctx.wait_one(std::chrono::milliseconds{50});
    if (!settled.has_value() || settled.value().kind != WaitKind::progress ||
        settled.value().completed != 0)
        return false;
    owner.reset();
    ctx.detach_progress_host();
    return true;
}

// The mandated sticky-control regression: acknowledging the control the owner
// observed must not erase a control that arrived before the acknowledgement.
bool acknowledging_observed_control_keeps_a_later_control() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncIoContext ctx(std::move(backend));
    auto claimed = ctx.claim_progress_owner();
    if (!claimed.has_value())
        return false;
    std::optional<ProgressOwner> owner = std::move(claimed).value();

    using WaitKind = AsyncIoContext::ProgressWaitOutcome::Kind;

    ctx.interrupt_progress_waiters();
    const auto observed_a = ctx.wait_one(std::chrono::milliseconds{100});
    if (!observed_a.has_value() || observed_a.value().kind != WaitKind::control_interrupted)
        return false;

    // B arrives before A is acknowledged.
    ctx.interrupt_progress_waiters();

    ctx.acknowledge_progress_control();
    const auto observed_b = ctx.wait_one(std::chrono::milliseconds{100});
    if (!observed_b.has_value() || observed_b.value().kind != WaitKind::control_interrupted)
        return false;

    ctx.acknowledge_progress_control();
    const auto settled = ctx.wait_one(std::chrono::milliseconds{50});
    if (!settled.has_value() || settled.value().kind != WaitKind::progress ||
        settled.value().completed != 0)
        return false;
    owner.reset();
    return true;
}

// A health failure of the wait machinery is a sticky owner-visible outcome,
// never a zero-completion report.
bool injected_health_failure_is_sticky_and_distinguishable() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncIoContext ctx(std::move(backend));
    auto claimed = ctx.claim_progress_owner();
    if (!claimed.has_value())
        return false;
    std::optional<ProgressOwner> owner = std::move(claimed).value();

    using WaitKind = AsyncIoContext::ProgressWaitOutcome::Kind;
    ctx.set_wait_health_failed_for_test();
    const auto sick_pass = ctx.poll_progress();
    if (!sick_pass.has_value() || !sick_pass.value().health_failed)
        return false;
    for (int round = 0; round < 3; ++round) {
        const auto r = ctx.wait_one(std::chrono::milliseconds{50});
        if (!r.has_value() || r.value().kind != WaitKind::health_failure)
            return false;
        if (r.value().completed != 0)
            return false;
    }
    owner.reset();
    return true;
}

// A batch wait stops on the sticky health verdict instead of spinning past
// it: the failure surfaces through the error channel, never as a
// zero-completion report, and the internally claimed owner is released so
// the caller's own passes can retire the accepted work.
bool batch_await_stops_on_health_failure() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    raw->set_worker_claimed_pause_gate(&gate);

    const int fd = temp_file_fd(std::string(64, 'h'));
    std::vector<std::byte> held_buf(32, std::byte{0});
    std::vector<std::byte> batch_buf(32, std::byte{0});
    auto held_submitted = ctx.submit_read(ReadOp{NativeFileRef(fd, FileAccess::read_only),
                                                 held_buf.data(), held_buf.size(), 0});
    if (!held_submitted.has_value())
        return false;
    Request<std::size_t> held = std::move(held_submitted).value();
    RequestPublicationDrain drain{[&ctx] { (void)ctx.poll(); }};
    drain.track(held);
    WorkerGateReleaseOnExit gate_release{raw, &gate};
    while (!gate.paused.load(std::memory_order_acquire))
        std::this_thread::yield();

    Batch batch;
    BatchOp op;
    op.kind = BatchOp::Kind::read;
    op.read = ReadOp{NativeFileRef(fd, FileAccess::read_only), batch_buf.data(), batch_buf.size(),
                     32};
    batch.add(op);

    ctx.set_wait_health_failed_for_test();
    const auto stopped = batch.await_one(ctx);
    const bool surfaced_as_error =
        !stopped.has_value() && stopped.error().code == sluice::IoError::Code::backend_error;

    raw->set_worker_claimed_pause_gate(nullptr);
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();

    bool retired = false;
    for (int i = 0; i < 2000 && !retired; ++i) {
        (void)ctx.poll();
        retired = held.ready() && ctx.outstanding() == 0;
        if (!retired)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    // Harvesting slot readiness is the batch protocol's own step: a call with
    // nothing outstanding claims nothing and only collects completed slots.
    const auto harvested = batch.await_one(ctx);
    while (batch.next().has_value()) {
    }

    held.discard();
    return surfaced_as_error && retired && harvested.has_value() && harvested.value() == 1 &&
           batch.pending_count() == 0;
}

// Driving on an unpinned context is a single domain: a second concurrent
// drive is rejected instead of serialized into accidental success.
bool competing_concurrent_drives_cannot_both_succeed() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{8, 2});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    raw->set_worker_claimed_pause_gate(&gate);

    const int fd = temp_file_fd(std::string(64, 'e'));
    std::vector<std::byte> buf(32, std::byte{0});
    auto c_submitted = ctx.submit_read(
        ReadOp{NativeFileRef(fd, FileAccess::read_only), buf.data(), buf.size(), 0});
    if (!c_submitted.has_value())
        return false;
    Request<std::size_t> c = std::move(c_submitted).value();
    RequestPublicationDrain drain{[&ctx] { (void)ctx.poll(); }};
    drain.track(c);
    WorkerGateReleaseOnExit gate_release{raw, &gate};

    // Thread A holds the drive domain inside a blocked wait_one; thread B's
    // wait_one and poll_progress must both be rejected.
    std::atomic<bool> a_parked{false};
    std::optional<AsyncIoContext::ProgressWaitOutcome> a_result;
    std::thread driver_a([&] {
        a_parked.store(true, std::memory_order_release);
        auto wr = ctx.wait_one(std::chrono::milliseconds{8000});
        if (wr.has_value())
            a_result = wr.value();
    });
    while (!a_parked.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    const auto b_wait = ctx.wait_one(std::chrono::milliseconds{50});
    const auto b_poll = ctx.poll_progress();
    const bool b_rejected = !b_wait.has_value() && !b_poll.has_value();

    raw->set_worker_claimed_pause_gate(nullptr);
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
    driver_a.join();

    const bool ok = b_rejected && a_result.has_value() &&
                    a_result->kind == AsyncIoContext::ProgressWaitOutcome::Kind::progress;
    c.discard();
    return ok;
}

// A nested public drive from a delivery hook inside an active pass is a
// contract violation, not a second driver.
bool nested_drive_from_delivery_hook_is_rejected() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{8, 1});
    AsyncIoContext ctx(std::move(backend));
    auto claimed = ctx.claim_progress_owner();
    if (!claimed.has_value())
        return false;
    std::optional<ProgressOwner> owner = std::move(claimed).value();

    class NestedProbe final : public detail::SynchronousReadySink {
      public:
        explicit NestedProbe(AsyncIoContext& ctx) : ctx_(ctx) {}

        void on_ready(detail::ReadyEvent) noexcept override {
            const auto nested = ctx_.poll_progress();
            nested_rejected_.store(!nested.has_value() &&
                                       nested.error().code == sluice::IoError::Code::invalid_state,
                                   std::memory_order_release);
        }

        bool nested_rejected() const noexcept {
            return nested_rejected_.load(std::memory_order_acquire);
        }

      private:
        AsyncIoContext& ctx_;
        std::atomic<bool> nested_rejected_{false};
    } probe(ctx);
    ctx.set_ready_sink(&probe);

    Request<std::size_t> c = submit_zero_op(ctx);
    RequestPublicationDrain drain{[&ctx] { (void)ctx.poll(); }};
    drain.track(c);
    drain.release_owner_before_draining(&owner);
    if (!c.valid())
        return false;
    const auto pass = ctx.poll_progress();
    ctx.set_ready_sink(nullptr);

    const bool ok = pass.has_value() && pass.value().completed == 1 && probe.nested_rejected();
    c.discard();
    owner.reset();
    return ok;
}

// PROG-01: the first drive attaches a fixed owner. While attached, every
// other thread's drive is rejected instead of silently succeeding, submission
// from other threads stays legal, and a released claim lets the next driver
// attach sequentially.
bool first_drive_attaches_a_fixed_owner() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{8, 2});
    AsyncIoContext ctx(std::move(backend));

    const auto self_pass = ctx.poll_progress();
    if (!self_pass.has_value())
        return false;

    Request<std::size_t> foreign_completion;
    RequestPublicationDrain drain{[&ctx] { (void)ctx.poll(); }};
    drain.track(foreign_completion);
    std::atomic<bool> foreign_drive_rejected{false};
    std::atomic<bool> foreign_wait_rejected{false};
    std::atomic<bool> foreign_submit_accepted{false};
    std::thread foreign([&] {
        const auto pass = ctx.poll_progress();
        foreign_drive_rejected.store(!pass.has_value() &&
                                         pass.error().code == sluice::IoError::Code::invalid_state,
                                     std::memory_order_release);
        const auto wr = ctx.wait_one(std::chrono::milliseconds{50});
        foreign_wait_rejected.store(!wr.has_value() &&
                                        wr.error().code == sluice::IoError::Code::invalid_state,
                                    std::memory_order_release);
        foreign_completion = submit_zero_op(ctx);
        foreign_submit_accepted.store(foreign_completion.valid(), std::memory_order_release);
    });
    foreign.join();

    if (!foreign_drive_rejected.load() || !foreign_wait_rejected.load() ||
        !foreign_submit_accepted.load())
        return false;

    // The attached thread claims explicitly, and exactly one handle is live.
    auto claimed = ctx.claim_progress_owner();
    if (!claimed.has_value() || ctx.claim_progress_owner().has_value())
        return false;
    std::optional<ProgressOwner> owner = std::move(claimed).value();
    const auto owner_pass = ctx.poll_progress();
    if (!owner_pass.has_value() || owner_pass.value().completed != 1)
        return false;

    // Release detaches: another thread may attach as the next driver.
    owner.reset();
    std::atomic<bool> reattached{false};
    std::thread next_driver([&] {
        const auto pass = ctx.poll_progress();
        reattached.store(pass.has_value(), std::memory_order_release);
    });
    next_driver.join();
    return reattached.load();
}

// Zero-work helpers make no driving demand: an empty read_all/write_all
// succeeds even while another thread holds the owner claim.
bool zero_work_helpers_skip_owner_acquisition() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    AsyncIoContext ctx(std::move(backend));
    auto claimed = ctx.claim_progress_owner();
    if (!claimed.has_value())
        return false;
    std::optional<ProgressOwner> owner = std::move(claimed).value();

    std::vector<std::byte> none;
    const auto rd = read_all(ctx, NativeFileRef(0, FileAccess::read_only), none, 0);
    const auto wr = write_all(ctx, NativeFileRef(1, FileAccess::write_only), none, 0);
    const bool ok = rd.has_value() && rd.value() == 0 && wr.has_value() && wr.value() == 0;
    owner.reset();
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
        {"external_poll_loop_threadpool_w03", external_poll_loop_threadpool_w03},
        {"coalesced_signals_delivered_through_one_wake",
         coalesced_signals_delivered_through_one_wake},
        {"spurious_wake_is_harmless_and_completions_survive",
         spurious_wake_is_harmless_and_completions_survive},
        {"saturated_notification_preserves_wake", saturated_notification_preserves_wake},
        {"host_acknowledgement_racing_new_signal_never_strands",
         host_acknowledgement_racing_new_signal_never_strands},
        {"host_stop_settle_detach_retires_registration",
         host_stop_settle_detach_retires_registration},
        {"stale_detached_registration_cannot_authorize_a_new_context",
         stale_detached_registration_cannot_authorize_a_new_context},
        {"contract_violations_fail_fast", contract_violations_fail_fast},
        {"pinned_context_rejects_foreign_drivers", pinned_context_rejects_foreign_drivers},
        {"first_drive_attaches_a_fixed_owner", first_drive_attaches_a_fixed_owner},
        {"deadline_expiry_does_not_cancel_or_settle", deadline_expiry_does_not_cancel_or_settle},
        {"idle_and_control_outcomes_are_distinct_and_sticky",
         idle_and_control_outcomes_are_distinct_and_sticky},
        {"control_before_wait_and_across_notification_ack_survives",
         control_before_wait_and_across_notification_ack_survives},
        {"acknowledging_observed_control_keeps_a_later_control",
         acknowledging_observed_control_keeps_a_later_control},
        {"injected_health_failure_is_sticky_and_distinguishable",
         injected_health_failure_is_sticky_and_distinguishable},
        {"batch_await_stops_on_health_failure", batch_await_stops_on_health_failure},
        {"competing_concurrent_drives_cannot_both_succeed",
         competing_concurrent_drives_cannot_both_succeed},
        {"nested_drive_from_delivery_hook_is_rejected",
         nested_drive_from_delivery_hook_is_rejected},
        {"zero_work_helpers_skip_owner_acquisition", zero_work_helpers_skip_owner_acquisition},
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
    std::printf("all %zu threadpool external loop tests passed\n", passed);
    return 0;
}
