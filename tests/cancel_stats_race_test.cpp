// AUD-001 regression: the cancel path's canceled_ops increment must live in
// the lock domain that owns the counter. Both backends tallied the metric
// after releasing their backend mutex, while a concurrent cancel (or, for
// io_uring, the owner's kernel-cancel completion) writes the same plain
// std::uint64_t under that mutex. THREAD-01 permits concurrent cancel from
// different threads, so the unlocked increments form unsynchronized
// write-write pairs.
//
// The schedule is deterministic: requests are accepted but left unclaimed
// (ThreadPool: the worker is parked after claiming one pipe-backed read;
// io_uring: the prepared transport is full, so later submissions stay in the
// dispatch queue), then two pre-created threads cancel one request each and
// both win before execution, so both take the unwon-lock increment path. The
// TSan build reports the write-write pair against the base tree and is silent
// once the increment is taken under the backend mutex. The count is also a
// semantic oracle: both wins must be counted exactly once.
#include <sluice/async/async_io_context.hpp>
#include <sluice/async/detail/async_backend.hpp>
#include <sluice/async/detail/threadpool_backend.hpp>
#include <sluice/async/request.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#if defined(SLUICE_HAS_LIBURING)
#include <liburing.h>
#include <sluice/async/detail/uring_backend.hpp>
#endif

namespace {

using namespace sluice::async;
using sluice::AsyncStats;
using sluice::File;
using sluice::IoError;

using sluice::Result;

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

template <class Gate> void wait_gate_paused(Gate& gate) {
    while (!gate.paused.load(std::memory_order_acquire))
        std::this_thread::yield();
}

template <class Gate> void resume_gate(Gate& gate) {
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
}

// ThreadPool case: one worker claims the first read and parks before its
// syscall; the two target submissions stay queued and unclaimed, so both
// cancels win before execution.
void threadpool_cancel_cancel_race() {
    AsyncStats stats;
    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx = detail::make_context(std::move(backend), &stats);

    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    raw->set_worker_claimed_pause_gate(&gate);

    char path[] = "/tmp/sluice_aud001_tp_XXXXXX";
    const int fd = ::mkstemp(path);
    check(fd >= 0, "the file fixture is created");
    if (fd < 0)
        return;
    const char payload[8] = {'0', '1', '2', '3', '4', '5', '6', '7'};
    check(::write(fd, payload, sizeof(payload)) == static_cast<ssize_t>(sizeof(payload)),
          "the file fixture is populated");
    ::close(fd);
    File file = std::move(File::open(path, sluice::FileOpen{}).value());
    ::unlink(path);

    std::vector<std::byte> buffer_a(4, std::byte{0});
    std::vector<std::byte> buffer_b(4, std::byte{0});
    std::vector<std::byte> buffer_c(4, std::byte{0});

    auto submitted_a = ctx.submit_read(ReadOp{NativeFileRef(file), buffer_a.data(), 4, 0});
    check(submitted_a.has_value(), "the claimed read is accepted");
    if (!submitted_a.has_value())
        return;
    Request<std::size_t> request_a = std::move(submitted_a).value();
    wait_gate_paused(gate);

    auto submitted_b = ctx.submit_read(ReadOp{NativeFileRef(file), buffer_b.data(), 4, 0});
    auto submitted_c = ctx.submit_read(ReadOp{NativeFileRef(file), buffer_c.data(), 4, 0});
    check(submitted_b.has_value() && submitted_c.has_value(),
          "the queued cancels are accepted while the worker is parked");
    if (!submitted_b.has_value() || !submitted_c.has_value())
        return;
    Request<std::size_t> request_b = std::move(submitted_b).value();
    Request<std::size_t> request_c = std::move(submitted_c).value();

    const detail::RequestKey key_b = detail::RequestBridge::key_of(request_b.id());
    const detail::RequestKey key_c = detail::RequestBridge::key_of(request_c.id());

    std::optional<detail::PublicCancel> disposition_b;
    std::optional<detail::PublicCancel> disposition_c;
    std::thread cancel_b([&] { disposition_b = raw->cancel_key_for_test(key_b); });
    std::thread cancel_c([&] { disposition_c = raw->cancel_key_for_test(key_c); });
    cancel_b.join();
    cancel_c.join();

    check(disposition_b.has_value() && *disposition_b == detail::PublicCancel::won_before_execution,
          "cancel B wins before execution");
    check(disposition_c.has_value() && *disposition_c == detail::PublicCancel::won_before_execution,
          "cancel C wins before execution");
    check(stats.canceled_ops == 2, "both wins are counted exactly once");

    raw->set_worker_claimed_pause_gate(nullptr);
    resume_gate(gate);

    for (int i = 0; i < 1000000 && (!request_a.ready() || !request_b.ready() || !request_c.ready());
         ++i) {
        (void)ctx.poll();
        std::this_thread::yield();
    }
    const auto observed_a = request_a.take_result();
    const auto observed_b = request_b.take_result();
    const auto observed_c = request_c.take_result();
    check(observed_a.readiness == RequestReadiness::ready && observed_a.result.has_value() &&
              observed_a.result.value() == 4,
          "the claimed read completes with its physical count");
    check(observed_b.readiness == RequestReadiness::ready && !observed_b.result.has_value() &&
              observed_b.result.error().code == IoError::Code::canceled,
          "canceled B settles with the canceled terminal");
    check(observed_c.readiness == RequestReadiness::ready && !observed_c.result.has_value() &&
              observed_c.result.error().code == IoError::Code::canceled,
          "canceled C settles with the canceled terminal");
}

#if defined(SLUICE_HAS_LIBURING)

// The first submit call reports nothing submitted while leaving the prepared
// hand-off intact, so the SQ fills and later requests stay in the dispatch
// queue unclaimed. After the cancels, the test switches to a fake consuming
// submit to settle the prepared operations.
struct SwitchableSubmit {
    std::atomic<int> mode{0};
};

int switchable_submit(void* context, ::io_uring* ring) noexcept {
    auto* state = static_cast<SwitchableSubmit*>(context);
    if (state->mode.load(std::memory_order_acquire) == 0)
        return 0;
    const unsigned ready = ::io_uring_sq_ready(ring);
    *ring->sq.ktail = ring->sq.sqe_tail;
    *ring->sq.khead = ring->sq.sqe_tail;
    return static_cast<int>(ready);
}

void uring_cancel_cancel_race() {
    SwitchableSubmit submit_state;
    UringBackendSubmitTestHooks hooks;
    hooks.submit = switchable_submit;
    hooks.context = &submit_state;
    std::unique_ptr<UringAsyncBackend> backend;
    try {
        backend = std::make_unique<UringAsyncBackend>(UringConfig{4, 2}, hooks);
    } catch (...) {
        std::printf("SKIP: io_uring profile unavailable\n");
        return;
    }
    if (!backend->available()) {
        std::printf("SKIP: io_uring profile unavailable\n");
        return;
    }
    UringAsyncBackend* raw = backend.get();
    AsyncStats stats;
    AsyncIoContext ctx = detail::make_context(std::move(backend), &stats);

    char path[] = "/tmp/sluice_aud001_XXXXXX";
    const int fd = ::mkstemp(path);
    check(fd >= 0, "the file fixture is created");
    if (fd < 0)
        return;
    const char payload[8] = {'0', '1', '2', '3', '4', '5', '6', '7'};
    check(::write(fd, payload, sizeof(payload)) == static_cast<ssize_t>(sizeof(payload)),
          "the file fixture is populated");
    File file = std::move(File::open(path, sluice::FileOpen{}).value());
    ::unlink(path);

    std::vector<std::byte> fill_a(4, std::byte{0});
    std::vector<std::byte> fill_b(4, std::byte{0});
    std::vector<std::byte> buffer_c(4, std::byte{0});
    std::vector<std::byte> buffer_d(4, std::byte{0});

    // queue_depth is 2: the first two submissions hold both prepared slots,
    // so C and D remain in the dispatch queue unclaimed.
    auto submitted_fill_a = ctx.submit_read(ReadOp{NativeFileRef(file), fill_a.data(), 4, 0});
    auto submitted_fill_b = ctx.submit_read(ReadOp{NativeFileRef(file), fill_b.data(), 4, 4});
    auto submitted_c = ctx.submit_read(ReadOp{NativeFileRef(file), buffer_c.data(), 4, 0});
    auto submitted_d = ctx.submit_read(ReadOp{NativeFileRef(file), buffer_d.data(), 4, 4});
    check(submitted_fill_a.has_value() && submitted_fill_b.has_value() && submitted_c.has_value() &&
              submitted_d.has_value(),
          "the filled and queued submissions are accepted");
    if (!submitted_fill_a.has_value() || !submitted_fill_b.has_value() ||
        !submitted_c.has_value() || !submitted_d.has_value())
        return;
    Request<std::size_t> request_fill_a = std::move(submitted_fill_a).value();
    Request<std::size_t> request_fill_b = std::move(submitted_fill_b).value();
    Request<std::size_t> request_c = std::move(submitted_c).value();
    Request<std::size_t> request_d = std::move(submitted_d).value();
    check(raw->dispatch_size_for_test() == 2 && raw->transport_ledger_size_for_test() == 2,
          "C and D wait in the dispatch queue behind the full prepared transport");

    const detail::RequestKey key_c = detail::RequestBridge::key_of(request_c.id());
    const detail::RequestKey key_d = detail::RequestBridge::key_of(request_d.id());

    std::optional<detail::PublicCancel> disposition_c;
    std::optional<detail::PublicCancel> disposition_d;
    std::thread cancel_c([&] { disposition_c = raw->cancel_key_for_test(key_c); });
    std::thread cancel_d([&] { disposition_d = raw->cancel_key_for_test(key_d); });
    cancel_c.join();
    cancel_d.join();

    check(disposition_c.has_value() && *disposition_c == detail::PublicCancel::won_before_execution,
          "cancel C wins before execution");
    check(disposition_d.has_value() && *disposition_d == detail::PublicCancel::won_before_execution,
          "cancel D wins before execution");
    check(stats.canceled_ops == 2, "both wins are counted exactly once");

    // Settle the prepared fillers: let the fake submit consume the SQEs and
    // deliver their synthetic completions.
    submit_state.mode.store(1, std::memory_order_release);
    check(raw->live_cookie_for_offset_for_test(0).has_value() &&
              raw->live_cookie_for_offset_for_test(4).has_value(),
          "the fillers carry their live routing");
    const std::uint64_t cookie_a = *raw->live_cookie_for_offset_for_test(0);
    const std::uint64_t cookie_b = *raw->live_cookie_for_offset_for_test(4);
    (void)ctx.poll();
    raw->inject_cqe_for_test(cookie_a, 4);
    raw->inject_cqe_for_test(cookie_b, 4);
    for (int i = 0; i < 1000000 && (!request_fill_a.ready() || !request_fill_b.ready() ||
                                    !request_c.ready() || !request_d.ready());
         ++i) {
        (void)ctx.poll();
        std::this_thread::yield();
    }
    const auto observed_fill_a = request_fill_a.take_result();
    const auto observed_fill_b = request_fill_b.take_result();
    const auto observed_c = request_c.take_result();
    const auto observed_d = request_d.take_result();
    check(observed_fill_a.readiness == RequestReadiness::ready &&
              observed_fill_a.result.has_value() && observed_fill_a.result.value() == 4,
          "the first prepared read completes with its physical count");
    check(observed_fill_b.readiness == RequestReadiness::ready &&
              observed_fill_b.result.has_value() && observed_fill_b.result.value() == 4,
          "the second prepared read completes with its physical count");
    check(observed_c.readiness == RequestReadiness::ready && !observed_c.result.has_value() &&
              observed_c.result.error().code == IoError::Code::canceled,
          "canceled C settles with the canceled terminal");
    check(observed_d.readiness == RequestReadiness::ready && !observed_d.result.has_value() &&
              observed_d.result.error().code == IoError::Code::canceled,
          "canceled D settles with the canceled terminal");
}

#endif

} // namespace

int main() {
    threadpool_cancel_cancel_race();
#if defined(SLUICE_HAS_LIBURING)
    uring_cancel_cancel_race();
#endif
    if (g_failures != 0) {
        std::fprintf(stderr, "%d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("PASS: canceled_ops cancel-path synchronization\n");
    return 0;
}
