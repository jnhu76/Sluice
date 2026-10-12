#include <sluice/async/completion.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <mutex>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "scheduler_test_access.hpp"

namespace {

using namespace sluice::async;
using sluice::File;
using sluice::FileAccess;
using sluice::FileOpen;
using sluice::IoError;
using sluice::make_unexpected;
using sluice::Result;
using sluice::async::detail::CoreSnapshot;
using sluice::async::detail::ObserverPhase;
using sluice::async::detail::RequestCore;
using sluice::async::detail::SlotIndex;

struct Tracker {
    const char* name;
    int failures = 0;

    void check(bool ok, const char* label) {
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "FAIL [%s] %s\n", name, label);
        }
    }
};

using SchedulerTestAccess = Scheduler::AsyncTestAccess;

template <class Gate> void wait_gate_paused(Gate& gate) {
    gate.paused.wait(false, std::memory_order_acquire);
}

template <class Gate> void resume_gate(Gate& gate) {
    gate.resume.store(true, std::memory_order_release);
    gate.resume.notify_all();
}

template <class Gate> void wait_gate_exited(Gate& gate) {
    gate.exited.wait(false, std::memory_order_acquire);
}

std::size_t wait_registry_live_becomes(Scheduler& sched, std::size_t expected) {
    std::size_t live = expected == 0 ? 1 : 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        live = SchedulerTestAccess::wait_registry_live_count(sched);
        if (live == expected)
            break;
        std::this_thread::yield();
    }
    return live;
}

bool scheduler_worker_parks_in_backend_domain(Scheduler& sched) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (SchedulerTestAccess::worker_park_domain(sched, 0) == WorkerState::ParkDomain::Backend)
            return true;
        std::this_thread::yield();
    }
    return false;
}

std::size_t slots_with_observer_registration(const RequestCore& core) {
    std::size_t registered = 0;
    for (std::uint32_t i = 0; i < core.capacity(); ++i) {
        const auto observed = core.observe_slot(SlotIndex{i});
        if (observed.has_value() && observed->observer_phase != ObserverPhase::unattached) {
            ++registered;
        }
    }
    return registered;
}

std::size_t slots_in_delivering_phase(const RequestCore& core) {
    std::size_t delivering = 0;
    for (std::uint32_t i = 0; i < core.capacity(); ++i) {
        const auto observed = core.observe_slot(SlotIndex{i});
        if (observed.has_value() && observed->observer_phase == ObserverPhase::delivering) {
            ++delivering;
        }
    }
    return delivering;
}

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_c1d_waiter_XXXXXX";
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

bool completion_wake_rides_the_observer_adapter(Tracker& t) {
    const std::string path = make_temp_file("sluice c1d wake payload");
    if (path.empty()) {
        t.check(false, "temp file created");
        return false;
    }
    auto file_open = File::open(path, FileOpen{.access = FileAccess::read_write});
    ::unlink(path.c_str());
    if (!file_open.has_value()) {
        t.check(false, "temp file opened");
        return false;
    }
    File file = std::move(file_open.value());

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* raw = backend.get();

    AsyncIoContext io(std::move(backend));
    Scheduler sched(io);

    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    raw->set_worker_claimed_pause_gate(&gate);

    std::promise<Result<std::size_t>> slot;
    std::vector<std::byte> buffer(19, std::byte{0});

    std::vector<std::byte> stack(256 * 1024);
    Fiber fiber([&](Fiber&) {
        Completion<std::size_t> c;
        auto sr = io.submit_read(ReadOp{NativeFileRef{file}, buffer.data(), 19, 0}, c);
        if (!sr.has_value()) {
            slot.set_value(make_unexpected<std::size_t>(sr.error()));
            return;
        }
        auto wr = sched.await_completion_size(c);
        if (!wr.has_value()) {
            slot.set_value(make_unexpected<std::size_t>(wr.error()));
            return;
        }
        auto r = c.result();
        c.reset();
        if (!r.has_value()) {
            slot.set_value(make_unexpected<std::size_t>(r.error()));
            return;
        }
        slot.set_value(r.value());
    });
    if (!sched.init_fiber(fiber, stack.data(), stack.size())) {
        t.check(false, "the fiber stack initializes");
        return false;
    }
    sched.spawn(fiber);
    std::thread driver([&] { sched.run(1); });

    wait_gate_paused(gate);
    t.check(wait_registry_live_becomes(sched, 1) == 1,
            "the suspended fiber holds exactly one host wait record");

    resume_gate(gate);
    wait_gate_exited(gate);
    raw->set_worker_claimed_pause_gate(nullptr);

    Result<std::size_t> result = slot.get_future().get();
    t.check(result.has_value() && result.value() == 19,
            "the fiber wakes through the observer adapter with the payload");
    t.check(std::memcmp(buffer.data(), "sluice c1d wake payload", 19) == 0,
            "the completion carries the published bytes");
    t.check(wait_registry_live_becomes(sched, 0) == 0, "the wait record retires with the delivery");

    driver.join();
    t.check(fiber.state() == FiberState::done, "the fiber finishes before stack retirement");
    t.check(SchedulerTestAccess::wait_registry_live_count(sched) == 0,
            "no wait record survives driver join");
    return t.failures == 0;
}

bool observer_cancel_wakes_the_fiber_and_the_operation_still_completes(Tracker& t) {
    const std::string path = make_temp_file("sluice c1d cancel payload");
    if (path.empty()) {
        t.check(false, "temp file created");
        return false;
    }
    auto file_open = File::open(path, FileOpen{.access = FileAccess::read_write});
    ::unlink(path.c_str());
    if (!file_open.has_value()) {
        t.check(false, "temp file opened");
        return false;
    }
    File file = std::move(file_open.value());

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* raw = backend.get();

    AsyncIoContext io(std::move(backend));
    Scheduler sched(io);

    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    raw->set_worker_claimed_pause_gate(&gate);

    std::promise<Result<std::size_t>> slot;
    std::vector<std::byte> buffer(21, std::byte{0});
    std::atomic<bool> await_was_canceled{false};
    std::atomic<Completion<std::size_t>*> pending{nullptr};

    std::vector<std::byte> stack(256 * 1024);
    Fiber fiber([&](Fiber&) {
        Completion<std::size_t> c;
        auto sr = io.submit_read(ReadOp{NativeFileRef{file}, buffer.data(), 21, 0}, c);
        if (!sr.has_value()) {
            slot.set_value(make_unexpected<std::size_t>(sr.error()));
            return;
        }
        pending.store(&c, std::memory_order_release);
        auto wr = sched.await_completion_size(c);
        pending.store(nullptr, std::memory_order_release);
        if (!wr.has_value() && wr.error().code == IoError::Code::canceled) {
            await_was_canceled.store(true, std::memory_order_release);
            await_was_canceled.notify_one();
            auto drained = sched.await_completion_size(c);
            if (!drained.has_value()) {
                slot.set_value(make_unexpected<std::size_t>(drained.error()));
                return;
            }
            auto result = c.result();
            c.reset();
            slot.set_value(std::move(result));
            return;
        }
        if (!wr.has_value()) {
            slot.set_value(make_unexpected<std::size_t>(wr.error()));
            return;
        }
        auto r = c.result();
        c.reset();
        if (!r.has_value()) {
            slot.set_value(make_unexpected<std::size_t>(r.error()));
            return;
        }
        slot.set_value(r.value());
    });
    if (!sched.init_fiber(fiber, stack.data(), stack.size())) {
        t.check(false, "the fiber stack initializes");
        return false;
    }
    sched.spawn(fiber);
    std::thread driver([&] { sched.run(1); });

    wait_gate_paused(gate);
    t.check(wait_registry_live_becomes(sched, 1) == 1,
            "the suspended fiber is registered before cancellation");

    Completion<std::size_t>* suspended = pending.load(std::memory_order_acquire);
    t.check(suspended != nullptr, "the suspended completion is addressable");
    auto canceled = suspended != nullptr ? sched.cancel_waiter(*suspended) : Result<bool>{false};
    t.check(canceled.has_value() && canceled.value(),
            "cancelling the registered observer wakes the fiber");
    await_was_canceled.wait(false, std::memory_order_acquire);
    t.check(io.outstanding() == 1 && !suspended->ready(),
            "waiter cancellation leaves the accepted borrow pending while physical work is gated");
    t.check(buffer.front() == std::byte{0}, "the held operation has not written its borrow");

    resume_gate(gate);
    wait_gate_exited(gate);
    raw->set_worker_claimed_pause_gate(nullptr);

    Result<std::size_t> result = slot.get_future().get();
    t.check(await_was_canceled.load(std::memory_order_acquire),
            "the canceled await returned the canceled disposition");
    t.check(result.has_value() && result.value() == 21,
            "the operation completed after its observer was canceled");
    t.check(std::memcmp(buffer.data(), "sluice c1d cancel payload", 21) == 0,
            "the completed operation carried the published bytes");
    t.check(wait_registry_live_becomes(sched, 0) == 0, "no wait record survives the canceled wait");

    driver.join();
    t.check(SchedulerTestAccess::wait_registry_live_count(sched) == 0,
            "no wait record survives driver join");
    t.check(io.outstanding() == 0, "accepted operations settle before teardown");
    return t.failures == 0;
}

bool already_terminal_await_returns_without_suspending(Tracker& t) {
    const std::string path = make_temp_file("sluice c1d zero payload");
    if (path.empty()) {
        t.check(false, "temp file created");
        return false;
    }
    auto file_open = File::open(path, FileOpen{.access = FileAccess::read_write});
    ::unlink(path.c_str());
    if (!file_open.has_value()) {
        t.check(false, "temp file opened");
        return false;
    }
    File file = std::move(file_open.value());

    AsyncIoContext io(std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1}));
    Scheduler sched(io);

    std::promise<Result<std::size_t>> slot;
    std::vector<std::byte> buffer(1, std::byte{0});

    std::vector<std::byte> stack(256 * 1024);
    Fiber fiber([&](Fiber& current) {
        Completion<std::size_t> c;
        auto sr = io.submit_read(ReadOp{NativeFileRef{file}, buffer.data(), 0, 0}, c);
        if (!sr.has_value()) {
            slot.set_value(make_unexpected<std::size_t>(sr.error()));
            return;
        }
        t.check(c.ready(), "the zero-length request is terminal before await");
        const std::array context_before{current.ctx.rsp, current.ctx.rbp, current.ctx.rip};
        auto wr = sched.await_completion_size(c);
        t.check(current.state() == FiberState::running, "the already-terminal fiber keeps running");
        t.check(context_before == std::array{current.ctx.rsp, current.ctx.rbp, current.ctx.rip},
                "already-terminal await performs no context switch");
        t.check(SchedulerTestAccess::wait_registry_live_count(sched) == 0,
                "already-terminal await leaves no armed wait record");
        if (!wr.has_value()) {
            slot.set_value(make_unexpected<std::size_t>(wr.error()));
            return;
        }
        auto r = c.result();
        c.reset();
        slot.set_value(r.has_value() ? Result<std::size_t>{r.value()}
                                     : make_unexpected<std::size_t>(r.error()));
    });
    if (!sched.init_fiber(fiber, stack.data(), stack.size())) {
        t.check(false, "the fiber stack initializes");
        return false;
    }
    sched.spawn(fiber);
    std::thread driver([&] { sched.run(1); });

    Result<std::size_t> result = slot.get_future().get();
    t.check(result.has_value() && result.value() == 0,
            "an await on a completion published at submit returns through the ready edge");
    t.check(SchedulerTestAccess::wait_registry_live_count(sched) == 0,
            "the already-terminal attach suspended no fiber");

    driver.join();
    t.check(SchedulerTestAccess::wait_registry_live_count(sched) == 0,
            "no wait record survives driver join");
    t.check(io.outstanding() == 0, "accepted operations settle before teardown");
    return t.failures == 0;
}

bool wait_record_exhaustion_fails_the_await_before_observer_attachment(Tracker& t) {
    const std::string path = make_temp_file("sluice obs04 exhaustion payload");
    if (path.empty()) {
        t.check(false, "temp file created");
        return false;
    }
    auto file_open = File::open(path, FileOpen{.access = FileAccess::read_write});
    ::unlink(path.c_str());
    if (!file_open.has_value()) {
        t.check(false, "temp file opened");
        return false;
    }
    File file = std::move(file_open.value());

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* raw = backend.get();

    AsyncIoContext io(std::move(backend));
    Scheduler sched(io, 1);
    RequestCore& core = *io.context_core_for_test();
    t.check(SchedulerTestAccess::configured_wait_capacity(sched) == 1,
            "the host wait-record pool is bounded to a single record");

    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    raw->set_worker_claimed_pause_gate(&gate);

    std::promise<Result<std::size_t>> slot_a;
    std::promise<Result<std::size_t>> slot_b;
    std::vector<std::byte> buffer_a(31, std::byte{0});
    std::vector<std::byte> buffer_b(15, std::byte{0});
    std::atomic<bool> await_b_failed_with_no_space{false};
    std::atomic<bool> holder_a_finished{false};

    std::vector<std::byte> stack_a(256 * 1024);
    Fiber fiber_a([&](Fiber&) {
        Completion<std::size_t> c;
        auto sr = io.submit_read(ReadOp{NativeFileRef{file}, buffer_a.data(), 31, 0}, c);
        if (!sr.has_value()) {
            slot_a.set_value(make_unexpected<std::size_t>(sr.error()));
            return;
        }
        auto wr = sched.await_completion_size(c);
        if (!wr.has_value()) {
            slot_a.set_value(make_unexpected<std::size_t>(wr.error()));
            return;
        }
        auto r = c.result();
        c.reset();
        if (!r.has_value()) {
            slot_a.set_value(make_unexpected<std::size_t>(r.error()));
            return;
        }
        slot_a.set_value(r.value());
        holder_a_finished.store(true, std::memory_order_release);
    });

    std::vector<std::byte> stack_b(256 * 1024);
    Fiber fiber_b([&](Fiber&) {
        Completion<std::size_t> c;
        auto sr = io.submit_read(ReadOp{NativeFileRef{file}, buffer_b.data(), 15, 0}, c);
        if (!sr.has_value()) {
            slot_b.set_value(make_unexpected<std::size_t>(sr.error()));
            return;
        }
        auto wr = sched.await_completion_size(c);
        if (!wr.has_value() && wr.error().code == IoError::Code::no_space) {
            await_b_failed_with_no_space.store(true, std::memory_order_release);
            sched.await_ready_flag(holder_a_finished);
            wr = sched.await_completion_size(c);
        }
        if (!wr.has_value()) {
            slot_b.set_value(make_unexpected<std::size_t>(wr.error()));
            return;
        }
        auto r = c.result();
        c.reset();
        if (!r.has_value()) {
            slot_b.set_value(make_unexpected<std::size_t>(r.error()));
            return;
        }
        slot_b.set_value(r.value());
    });
    if (!sched.init_fiber(fiber_a, stack_a.data(), stack_a.size()) ||
        !sched.init_fiber(fiber_b, stack_b.data(), stack_b.size())) {
        t.check(false, "both fiber stacks initialize");
        return false;
    }
    sched.spawn(fiber_a);
    sched.spawn(fiber_b);
    std::thread driver([&] { sched.run(1); });

    wait_gate_paused(gate);

    bool failure_observed = false;
    const auto failure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < failure_deadline) {
        if (await_b_failed_with_no_space.load(std::memory_order_acquire)) {
            failure_observed = true;
            break;
        }
        std::this_thread::yield();
    }
    t.check(failure_observed, "the exhausted await fails with no_space");

    t.check(SchedulerTestAccess::wait_registry_live_count(sched) == 1,
            "only the record holder owns a live wait record");
    t.check(slots_with_observer_registration(core) == 1,
            "the exhausted await left its request with no observer registration");
    t.check(core.snapshot().accepted_live == 2,
            "both operations stay outstanding after the host resource failure");

    resume_gate(gate);
    wait_gate_exited(gate);
    raw->set_worker_claimed_pause_gate(nullptr);

    Result<std::size_t> result_a = slot_a.get_future().get();
    t.check(result_a.has_value() && result_a.value() == 31,
            "the record holder completes with its payload");
    t.check(std::memcmp(buffer_a.data(), "sluice obs04 exhaustion payload", 31) == 0,
            "the record holder carries the published bytes");

    Result<std::size_t> result_b = slot_b.get_future().get();
    t.check(result_b.has_value() && result_b.value() == 15,
            "the refused await settles its request through the normal await edge");
    t.check(std::memcmp(buffer_b.data(), "sluice obs04 ex", 15) == 0,
            "the refused operation completed without cancellation");
    t.check(wait_registry_live_becomes(sched, 0) == 0,
            "no wait record survives the exhausted await");

    const CoreSnapshot settled_snapshot = core.snapshot();
    t.check(settled_snapshot.free_slots == core.capacity() && settled_snapshot.accepted_live == 0,
            "every slot reclaims with no observer pin after the failure path");

    driver.join();
    t.check(SchedulerTestAccess::wait_registry_live_count(sched) == 0,
            "no wait record survives driver join");
    t.check(io.outstanding() == 0, "accepted operations settle before teardown");
    return t.failures == 0;
}

bool cancel_during_the_staged_delivery_window_completes_normally(Tracker& t) {
    const std::string path = make_temp_file("sluice obs04 staged window payload");
    if (path.empty()) {
        t.check(false, "temp file created");
        return false;
    }
    auto file_open = File::open(path, FileOpen{.access = FileAccess::read_write});
    ::unlink(path.c_str());
    if (!file_open.has_value()) {
        t.check(false, "temp file opened");
        return false;
    }
    File file = std::move(file_open.value());

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1});
    ThreadPoolBackend* raw = backend.get();

    AsyncIoContext io(std::move(backend));
    Scheduler sched(io);
    RequestCore& core = *io.context_core_for_test();

    ThreadPoolBackend::WorkerClaimedPauseGate work_gate;
    ThreadPoolBackend::DeliveryClaimedPauseGate delivery_gate;
    raw->set_worker_claimed_pause_gate(&work_gate);
    raw->set_delivery_claimed_pause_gate(&delivery_gate);

    std::promise<Result<std::size_t>> slot;
    std::vector<std::byte> buffer(34, std::byte{0});
    std::atomic<Completion<std::size_t>*> pending{nullptr};

    std::vector<std::byte> stack(256 * 1024);
    Fiber fiber([&](Fiber&) {
        Completion<std::size_t> c;
        auto sr = io.submit_read(ReadOp{NativeFileRef{file}, buffer.data(), 34, 0}, c);
        if (!sr.has_value()) {
            slot.set_value(make_unexpected<std::size_t>(sr.error()));
            return;
        }
        pending.store(&c, std::memory_order_release);
        auto wr = sched.await_completion_size(c);
        pending.store(nullptr, std::memory_order_release);
        if (!wr.has_value()) {
            slot.set_value(make_unexpected<std::size_t>(wr.error()));
            return;
        }
        auto r = c.result();
        c.reset();
        if (!r.has_value()) {
            slot.set_value(make_unexpected<std::size_t>(r.error()));
            return;
        }
        slot.set_value(r.value());
    });
    if (!sched.init_fiber(fiber, stack.data(), stack.size())) {
        t.check(false, "the fiber stack initializes");
        return false;
    }
    sched.spawn(fiber);
    std::thread driver([&] { sched.run(1); });

    wait_gate_paused(work_gate);
    t.check(scheduler_worker_parks_in_backend_domain(sched),
            "the progress owner parks on the backend wait source");

    resume_gate(work_gate);
    wait_gate_paused(delivery_gate);

    t.check(SchedulerTestAccess::wait_registry_live_count(sched) == 1,
            "the staged delivery keeps its host wait record");
    t.check(slots_in_delivering_phase(core) == 1,
            "the observer is claimed for delivery while the record stays registered");

    Completion<std::size_t>* suspended = pending.load(std::memory_order_acquire);
    t.check(suspended != nullptr, "the suspended completion is addressable");

    std::atomic<int> cancel_outcome{-1};
    std::atomic<bool> cancel_completed{false};
    std::thread canceler([&] {
        auto canceled = sched.cancel_waiter(*suspended);
        cancel_outcome.store(canceled.has_value() ? (canceled.value() ? 1 : 0) : -1,
                             std::memory_order_release);
        cancel_completed.store(true, std::memory_order_release);
    });

    const auto cancel_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!cancel_completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < cancel_deadline) {
        std::this_thread::yield();
    }
    const bool cancel_completed_while_pinned = cancel_completed.load(std::memory_order_acquire);
    t.check(cancel_completed_while_pinned,
            "the canceling thread completes while the delivery gate stays pinned");

    if (cancel_completed_while_pinned) {
        canceler.join();

        t.check(cancel_outcome.load(std::memory_order_acquire) == 0,
                "a cancel during the staged delivery window is refused");
        t.check(SchedulerTestAccess::wait_registry_live_count(sched) == 1,
                "the refused cancel leaves the host wait record live");
        t.check(slots_in_delivering_phase(core) == 1,
                "the refused cancel leaves the observer claimed for delivery");
    } else {
        resume_gate(delivery_gate);
        canceler.join();
    }

    resume_gate(delivery_gate);

    raw->set_worker_claimed_pause_gate(nullptr);
    raw->set_delivery_claimed_pause_gate(nullptr);

    Result<std::size_t> result = slot.get_future().get();
    t.check(result.has_value() && result.value() == 34,
            "the wait completes through the normal delivery path");
    t.check(std::memcmp(buffer.data(), "sluice obs04 staged window payload", 34) == 0,
            "the delivered completion carries the published bytes");
    t.check(wait_registry_live_becomes(sched, 0) == 0,
            "no wait record survives the staged delivery");

    const CoreSnapshot settled_snapshot = core.snapshot();
    t.check(settled_snapshot.free_slots == core.capacity() && settled_snapshot.accepted_live == 0,
            "the delivered slot reclaims with no observer pin");

    driver.join();
    t.check(SchedulerTestAccess::wait_registry_live_count(sched) == 0,
            "no wait record survives driver join");
    t.check(io.outstanding() == 0, "accepted operations settle before teardown");
    return t.failures == 0;
}

} // namespace

int main() {
    std::mutex watchdog_mutex;
    std::condition_variable watchdog_cv;
    bool finished = false;
    std::thread watchdog([&] {
        std::unique_lock lock(watchdog_mutex);
        if (!watchdog_cv.wait_for(lock, std::chrono::seconds(60), [&] { return finished; })) {
            std::fprintf(stderr,
                         "FAIL: Scheduler waiter watchdog: controlled gate or driver stuck\n");
            std::_Exit(2);
        }
    });
    struct NamedTest {
        const char* name;
        bool (*fn)(Tracker&);
    };
    const NamedTest tests[] = {
        {"completion_wake_rides_the_observer_adapter", completion_wake_rides_the_observer_adapter},
        {"observer_cancel_wakes_the_fiber_and_the_operation_still_completes",
         observer_cancel_wakes_the_fiber_and_the_operation_still_completes},
        {"already_terminal_await_returns_without_suspending",
         already_terminal_await_returns_without_suspending},
        {"wait_record_exhaustion_fails_the_await_before_observer_attachment",
         wait_record_exhaustion_fails_the_await_before_observer_attachment},
        {"cancel_during_the_staged_delivery_window_completes_normally",
         cancel_during_the_staged_delivery_window_completes_normally},
    };

    int failures = 0;
    for (const NamedTest& test : tests) {
        Tracker tracker{test.name};
        if (!test.fn(tracker)) {
            ++failures;
            std::fprintf(stderr, "FAIL: %s\n", test.name);
        }
    }
    {
        std::lock_guard lock(watchdog_mutex);
        finished = true;
    }
    watchdog_cv.notify_one();
    watchdog.join();
    if (failures != 0)
        return 1;
    std::printf("all %zu runtime waiter observer tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
