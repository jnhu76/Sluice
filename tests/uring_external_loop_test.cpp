#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/uring_backend.hpp>
#include <sluice/result.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include <csignal>

namespace {

using namespace sluice::async;
using sluice::FileAccess;

// An application-owned event loop: registration uses the context
// notification fd with real poll(2); a readable fd is acknowledged through
// the documented context operation and followed by bounded progress passes
// until the pass reports no remaining immediate work. Deliveries are
// accumulated across every pass the host runs — the submitting pass can be
// the servicing pass when the kernel finishes inside its submission window.
class ExternalLoopHost {
  public:
    explicit ExternalLoopHost(AsyncIoContext& ctx) : ctx_(ctx) {
        nfd_ = ctx.progress_notification_fd();
    }

    int nfd() const noexcept { return nfd_; }

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

    // PROG-01: dispatching a newly accepted request is itself a progress
    // obligation, so the host runs one bounded pass after submitting before
    // it may park on the notification fd. Returns the completions that pass
    // already delivered.
    std::size_t drive_submission() { return ctx_.poll_progress().completed; }

  private:
    AsyncIoContext& ctx_;
    int nfd_ = -1;
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
    Completion<std::size_t> r1, r2, r3, w;

    if (!ctx.submit_read(ReadOp{NativeFileRef(source, FileAccess::read_only), r1buf.data(),
                                r1buf.size(), 0},
                         r1)
             .has_value())
        return false;
    std::size_t phase_a = host.drive_submission();
    if (!host.wait_readable(std::chrono::milliseconds{5000}))
        return false;
    phase_a += host.acknowledge_and_drive();
    if (phase_a != 1 || !r1.ready())
        return false;
    r1.reset();
    // A servicing pass is the first observer of the kernel completion
    // transition and re-asserts readiness once; one settling round must
    // return the fd to quiet.
    if (host.acknowledge_and_drive() != 0)
        return false;
    if (fd_readable(host.nfd()))
        return false;

    // Control wake with zero completions: the host acks, drives an empty
    // pass, and keeps serving later real work.
    ctx.interrupt_progress_waiters();
    if (!host.wait_readable(std::chrono::milliseconds{5000}))
        return false;
    if (host.acknowledge_and_drive() != 0)
        return false;
    if (fd_readable(host.nfd()))
        return false;

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
    std::size_t batch = host.drive_submission();

    int settled_without_wake = 0;
    for (;;) {
        if (r2.ready() && r3.ready() && w.ready())
            break;
        if (!host.wait_readable(std::chrono::milliseconds{5000}))
            return false;
        const std::size_t got = host.acknowledge_and_drive();
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

    const bool ok = r2.ready() && r3.ready() && w.ready();
    r2.reset();
    r3.reset();
    w.reset();
    ::close(source);
    ::close(sink);
    return ok;
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
    std::vector<Completion<std::size_t>> completions(kCoalesced);
    for (int i = 0; i < kCoalesced; ++i) {
        bufs[static_cast<std::size_t>(i)].assign(64, std::byte{0});
        if (!ctx
                 .submit_read(ReadOp{NativeFileRef(source, FileAccess::read_only),
                                     bufs[static_cast<std::size_t>(i)].data(),
                                     bufs[static_cast<std::size_t>(i)].size(),
                                     static_cast<std::uint64_t>(i) * 32},
                              completions[static_cast<std::size_t>(i)])
                 .has_value())
            return false;
    }
    std::size_t delivered = host.drive_submission();

    int wakes = 0;
    while (delivered < static_cast<std::size_t>(kCoalesced)) {
        if (!host.wait_readable(std::chrono::milliseconds{2000}))
            return false;
        ++wakes;
        if (wakes > 4 * kCoalesced)
            return false;
        delivered += host.acknowledge_and_drive();
    }

    if (host.acknowledge_and_drive() != 0)
        return false;
    bool ok = !fd_readable(host.nfd());
    for (auto& c : completions) {
        ok = ok && c.ready();
        c.reset();
    }
    ::close(source);
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
    Completion<std::size_t> zero;
    if (!ctx
             .submit_read(ReadOp{NativeFileRef(::fileno(tmpfile()), FileAccess::read_only),
                                 buffer.data(), 0, 0},
                          zero)
             .has_value())
        return false;
    if (!fd_readable(host.nfd()))
        return false;
    if (host.drive_submission() != 1)
        return false;
    if (!zero.ready())
        return false;
    if (host.acknowledge_and_drive() != 0)
        return false;
    zero.reset();
    return !fd_readable(host.nfd());
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
    Completion<std::size_t> c;
    if (!ctx.submit_read(ReadOp{NativeFileRef(source, FileAccess::read_only), buf.data(),
                                buf.size(), 0},
                         c)
             .has_value())
        return false;
    std::size_t delivered = host.drive_submission();

    int rounds = 0;
    while (delivered < 1 && rounds < 8) {
        ++rounds;
        if (!host.wait_readable(std::chrono::milliseconds{2000}))
            return false;
        delivered += host.acknowledge_and_drive();
    }
    if (delivered != 1 || !c.ready())
        return false;
    c.reset();
    ::close(source);

    if (host.acknowledge_and_drive() != 0)
        return false;
    if (fd_readable(host.nfd()))
        return false;
    struct pollfd p;
    p.fd = host.nfd();
    p.events = POLLIN;
    p.revents = 0;
    return ::poll(&p, 1, 150) == 0;
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
