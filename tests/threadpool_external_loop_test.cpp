#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include <csignal>

namespace {

using namespace sluice::async;
using sluice::FileAccess;

// A W-03 host loop: the application owns the event loop, the request
// lifetime, and the buffers. Registration uses the context notification fd
// with real poll(2); a readable fd is acknowledged through the documented
// context operation and followed by bounded progress passes until the pass
// reports no remaining immediate work.
class ExternalLoopHost {
  public:
    explicit ExternalLoopHost(AsyncIoContext& ctx) : ctx_(ctx) {
        nfd_ = ctx.progress_notification_fd();
    }

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
    // bounded progress passes until neither completed work nor immediate work
    // remains. Returns the number of public completions delivered.
    std::size_t acknowledge_and_drive() {
        ctx_.acknowledge_progress_notification();
        std::size_t delivered = 0;
        for (int i = 0; i < 64; ++i) {
            const auto pass = ctx_.poll_progress();
            delivered += pass.completed;
            if (pass.completed == 0 && !pass.immediate_work_remains)
                break;
        }
        return delivered;
    }

  private:
    AsyncIoContext& ctx_;
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

bool submit_zero_op(AsyncIoContext& ctx, Completion<std::size_t>& c) {
    std::vector<std::byte> buffer(4, std::byte{0});
    return ctx.submit_read(ReadOp{NativeFileRef(::fileno(tmpfile()), FileAccess::read_only),
                                  buffer.data(), 0, 0},
                           c)
        .has_value();
}

// The §24 acceptance shape: ThreadPoolBackend → AsyncIoContext → external
// poll(2) loop → progress_notification_fd → documented ack/progress
// operations, with multiple requests, a zero-completion control wake, and
// coalesced completions; no Scheduler, no Fiber, no condition variable, no
// periodic timer.
bool external_poll_loop_threadpool_w03() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{16, 2});
    AsyncIoContext ctx(std::move(backend));
    ExternalLoopHost host(ctx);
    if (host.nfd() < 0)
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
    Completion<std::size_t> r1, r2, r3, w;

    if (!ctx.submit_read(ReadOp{NativeFileRef(source, FileAccess::read_only), r1buf.data(),
                                r1buf.size(), 0},
                         r1)
             .has_value())
        return false;
    if (!host.wait_readable(std::chrono::milliseconds{5000}))
        return false;
    const std::size_t phase_a = host.acknowledge_and_drive();
    if (phase_a != 1 || !r1.ready())
        return false;

    ctx.interrupt_progress_waiters();
    if (!host.wait_readable(std::chrono::milliseconds{5000}))
        return false;
    if (host.acknowledge_and_drive() != 0)
        return false;
    if (fd_readable(host.nfd()))
        return false;

    Completion<std::size_t> z1, z2, z3;
    if (!submit_zero_op(ctx, z1) || !submit_zero_op(ctx, z2) || !submit_zero_op(ctx, z3))
        return false;
    if (!host.wait_readable(std::chrono::milliseconds{5000}))
        return false;
    if (host.acknowledge_and_drive() != 3)
        return false;
    if (!z1.ready() || !z2.ready() || !z3.ready())
        return false;
    z1.reset();
    z2.reset();
    z3.reset();

    if (!ctx.submit_read(ReadOp{NativeFileRef(source, FileAccess::read_only), r2buf.data(),
                                r2buf.size(), 16},
                         r2)
             .has_value())
        return false;
    if (!ctx.submit_read(ReadOp{NativeFileRef(source, FileAccess::read_only), r3buf.data(),
                                r3buf.size(), 64},
                         r3)
             .has_value())
        return false;
    if (!ctx.submit_write(WriteOp{NativeFileRef(sink, FileAccess::read_write), wbuf.data(),
                                  wbuf.size(), 0},
                          w)
             .has_value())
        return false;

    Completion<std::size_t> late;
    std::thread helper([&] {
        // Submit from application-observed state, not from the notification
        // fd: the host and this helper must not compete for the shared
        // level-triggered readiness.
        for (int i = 0; i < 20000000; ++i) {
            if (r2.ready() && r3.ready() && w.ready())
                break;
            std::this_thread::yield();
        }
        (void)submit_zero_op(ctx, late);
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

    const bool ok = r1.ready() && r2.ready() && r3.ready() && w.ready() && late.ready();
    r1.reset();
    r2.reset();
    r3.reset();
    w.reset();
    late.reset();
    return ok;
}

// One readable event may stand for several signals: five coalesced zero-op
// completions are all discovered through a single wake.
bool coalesced_signals_delivered_through_one_wake() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{16, 1});
    AsyncIoContext ctx(std::move(backend));
    ExternalLoopHost host(ctx);

    Completion<std::size_t> warm;
    if (!submit_zero_op(ctx, warm))
        return false;
    if (!host.wait_readable(std::chrono::milliseconds{5000}))
        return false;
    if (host.acknowledge_and_drive() != 1)
        return false;
    warm.reset();
    if (fd_readable(host.nfd()))
        return false;

    constexpr int kCoalesced = 5;
    std::vector<Completion<std::size_t>> zeros(kCoalesced);
    for (int i = 0; i < kCoalesced; ++i) {
        if (!submit_zero_op(ctx, zeros[static_cast<std::size_t>(i)]))
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
        z.reset();
    }
    return ok;
}

// A wake that carries no completion must not confuse the host: the loop acks,
// drives an empty pass, and keeps serving later real work.
bool spurious_wake_is_harmless_and_completions_survive() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{8, 1});
    AsyncIoContext ctx(std::move(backend));
    ExternalLoopHost host(ctx);

    ctx.interrupt_progress_waiters();
    if (!host.wait_readable(std::chrono::milliseconds{5000}))
        return false;
    if (host.acknowledge_and_drive() != 0)
        return false;

    std::vector<std::byte> buf(32, std::byte{0});
    const int fd = temp_file_fd(std::string(64, 'c'));
    Completion<std::size_t> c;
    if (!ctx.submit_read(ReadOp{NativeFileRef(fd, FileAccess::read_only), buf.data(), buf.size(), 0},
                         c)
             .has_value())
        return false;
    if (!host.wait_readable(std::chrono::milliseconds{5000}))
        return false;
    if (host.acknowledge_and_drive() != 1)
        return false;

    const bool ok = c.ready();
    c.reset();
    return ok;
}

// A saturated notification fd keeps asserting readiness: the next signal
// observes EAGAIN, the host still wakes, and one acknowledgement drains it.
bool saturated_notification_preserves_wake() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{8, 1});
    AsyncIoContext ctx(std::move(backend));
    ExternalLoopHost host(ctx);

    ctx.saturate_progress_notification_for_test();
    if (!fd_readable(host.nfd()))
        return false;

    Completion<std::size_t> zero;
    if (!submit_zero_op(ctx, zero))
        return false;
    if (!host.wait_readable(std::chrono::milliseconds{5000}))
        return false;
    if (host.acknowledge_and_drive() != 1)
        return false;
    if (!zero.ready())
        return false;
    zero.reset();

    if (fd_readable(host.nfd()))
        return false;
    struct pollfd p;
    p.fd = host.nfd();
    p.events = POLLIN;
    p.revents = 0;
    return ::poll(&p, 1, 150) == 0;
}

// R14: the host acknowledges stale readiness while a new ThreadPool progress
// signal races it; the obligation is either serviced by the post-ack pass or
// re-asserts readiness — it can never strand.
bool host_acknowledgement_racing_new_signal_never_strands() {
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{8, 1});
    AsyncIoContext ctx(std::move(backend));
    ExternalLoopHost host(ctx);

    Completion<std::size_t> warm;
    if (!submit_zero_op(ctx, warm))
        return false;
    if (!host.wait_readable(std::chrono::milliseconds{5000}))
        return false;
    if (host.acknowledge_and_drive() != 1)
        return false;
    warm.reset();
    if (fd_readable(host.nfd()))
        return false;

    std::atomic<bool> acked{false};
    Completion<std::size_t> racing;
    std::thread signaler([&] {
        while (!acked.load(std::memory_order_acquire))
            std::this_thread::yield();
        (void)submit_zero_op(ctx, racing);
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

    const bool ok = settled && racing.ready() && wakes <= 2;
    racing.reset();
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
