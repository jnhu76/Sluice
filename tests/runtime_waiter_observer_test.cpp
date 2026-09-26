#include <sluice/async/application_runtime.hpp>
#include <sluice/async/await_op_helpers.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/task_result.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <atomic>
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
    for (int i = 0; i < 500000; ++i) {
        live = SchedulerTestAccess::wait_registry_live_count(sched);
        if (live == expected)
            break;
        std::this_thread::yield();
    }
    return live;
}

bool scheduler_worker_parks_in_backend_domain(Scheduler& sched) {
    for (int i = 0; i < 500000; ++i) {
        if (SchedulerTestAccess::worker_park_domain(sched, 0) == WorkerState::ParkDomain::Backend)
            return true;
        std::this_thread::yield();
    }
    return false;
}

bool scheduler_global_lock_becomes_held(Scheduler& sched) {
    for (int i = 0; i < 500000; ++i) {
        if (SchedulerTestAccess::try_lock_global_for_test(sched)) {
            SchedulerTestAccess::unlock_global_for_test(sched);
            std::this_thread::yield();
            continue;
        }
        return true;
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

    RuntimeBuilder builder;
    builder.backend(std::move(backend));
    builder.workers(1);
    auto build = builder.build();
    t.check(build.has_value(), "the runtime builds");
    if (!build.has_value())
        return false;
    std::unique_ptr<ApplicationRuntime> rt = std::move(build.value());
    t.check(rt->start().has_value(), "the runtime starts");
    Scheduler& sched = rt->test_scheduler_for_worker_topology();

    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    raw->set_worker_claimed_pause_gate(&gate);

    TaskResultSlot<Result<std::size_t>> slot;
    std::vector<std::byte> buffer(19, std::byte{0});

    auto submitted = rt->submit([&](RuntimeTaskContext& ctx) {
        Completion<std::size_t> c;
        auto sr = ctx.submit_read(ReadOp{NativeFileRef{file}, buffer.data(), 19, 0}, c);
        if (!sr.has_value()) {
            slot.publish(make_unexpected<std::size_t>(sr.error()));
            return;
        }
        auto wr = ctx.await_completion(c);
        if (!wr.has_value()) {
            slot.publish(make_unexpected<std::size_t>(wr.error()));
            return;
        }
        auto r = c.result();
        c.reset();
        if (!r.has_value()) {
            slot.publish(make_unexpected<std::size_t>(r.error()));
            return;
        }
        slot.publish(r.value());
    });
    t.check(submitted.has_value(), "the task is admitted");

    wait_gate_paused(gate);
    t.check(wait_registry_live_becomes(sched, 1) == 1,
            "the suspended fiber holds exactly one host wait record");

    resume_gate(gate);
    wait_gate_exited(gate);
    raw->set_worker_claimed_pause_gate(nullptr);

    Result<std::size_t> result = slot.wait_and_take();
    t.check(result.has_value() && result.value() == 19,
            "the fiber wakes through the observer adapter with the payload");
    t.check(std::memcmp(buffer.data(), "sluice c1d wake payload", 19) == 0,
            "the completion carries the published bytes");
    t.check(wait_registry_live_becomes(sched, 0) == 0,
            "the wait record retires with the delivery");

    rt->request_stop();
    t.check(rt->drain().has_value(), "the runtime drains");
    t.check(rt->join().has_value(), "the runtime joins");
    t.check(SchedulerTestAccess::wait_registry_live_count(sched) == 0,
            "no wait record survives the runtime");
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

    RuntimeBuilder builder;
    builder.backend(std::move(backend));
    builder.workers(1);
    auto build = builder.build();
    t.check(build.has_value(), "the runtime builds");
    if (!build.has_value())
        return false;
    std::unique_ptr<ApplicationRuntime> rt = std::move(build.value());
    t.check(rt->start().has_value(), "the runtime starts");
    Scheduler& sched = rt->test_scheduler_for_worker_topology();

    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    raw->set_worker_claimed_pause_gate(&gate);

    TaskResultSlot<Result<std::size_t>> slot;
    std::vector<std::byte> buffer(21, std::byte{0});
    std::atomic<bool> await_was_canceled{false};
    std::atomic<Completion<std::size_t>*> pending{nullptr};

    auto submitted = rt->submit([&](RuntimeTaskContext& ctx) {
        Completion<std::size_t> c;
        auto sr = ctx.submit_read(ReadOp{NativeFileRef{file}, buffer.data(), 21, 0}, c);
        if (!sr.has_value()) {
            slot.publish(make_unexpected<std::size_t>(sr.error()));
            return;
        }
        pending.store(&c, std::memory_order_release);
        auto wr = ctx.await_completion(c);
        pending.store(nullptr, std::memory_order_release);
        if (!wr.has_value() && wr.error().code == IoError::Code::canceled) {
            await_was_canceled.store(true, std::memory_order_release);
            auto drained = await_drain(ctx, c);
            if (!drained.has_value()) {
                slot.publish(make_unexpected<std::size_t>(drained.error()));
                return;
            }
            slot.publish(std::size_t{21});
            return;
        }
        if (!wr.has_value()) {
            slot.publish(make_unexpected<std::size_t>(wr.error()));
            return;
        }
        auto r = c.result();
        c.reset();
        if (!r.has_value()) {
            slot.publish(make_unexpected<std::size_t>(r.error()));
            return;
        }
        slot.publish(r.value());
    });
    t.check(submitted.has_value(), "the task is admitted");

    wait_gate_paused(gate);
    t.check(wait_registry_live_becomes(sched, 1) == 1,
            "the suspended fiber is registered before cancellation");

    Completion<std::size_t>* suspended = pending.load(std::memory_order_acquire);
    t.check(suspended != nullptr, "the suspended completion is addressable");
    auto canceled = suspended != nullptr ? sched.cancel_waiter(*suspended)
                                         : Result<bool>{false};
    t.check(canceled.has_value() && canceled.value(),
            "cancelling the registered observer wakes the fiber");

    resume_gate(gate);
    wait_gate_exited(gate);
    raw->set_worker_claimed_pause_gate(nullptr);

    Result<std::size_t> result = slot.wait_and_take();
    t.check(await_was_canceled.load(std::memory_order_acquire),
            "the canceled await returned the canceled disposition");
    t.check(result.has_value() && result.value() == 21,
            "the operation completed after its observer was canceled");
    t.check(std::memcmp(buffer.data(), "sluice c1d cancel payload", 21) == 0,
            "the completed operation carried the published bytes");
    t.check(wait_registry_live_becomes(sched, 0) == 0,
            "no wait record survives the canceled wait");

    rt->request_stop();
    t.check(rt->drain().has_value(), "the runtime drains");
    t.check(rt->join().has_value(), "the runtime joins");
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

    RuntimeBuilder builder;
    builder.backend(std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 1}));
    builder.workers(1);
    auto build = builder.build();
    t.check(build.has_value(), "the runtime builds");
    if (!build.has_value())
        return false;
    std::unique_ptr<ApplicationRuntime> rt = std::move(build.value());
    t.check(rt->start().has_value(), "the runtime starts");
    Scheduler& sched = rt->test_scheduler_for_worker_topology();

    TaskResultSlot<Result<std::size_t>> slot;
    std::vector<std::byte> buffer(1, std::byte{0});

    auto submitted = rt->submit([&](RuntimeTaskContext& ctx) {
        Completion<std::size_t> c;
        auto sr = ctx.submit_read(ReadOp{NativeFileRef{file}, buffer.data(), 0, 0}, c);
        if (!sr.has_value()) {
            slot.publish(make_unexpected<std::size_t>(sr.error()));
            return;
        }
        auto wr = ctx.await_completion(c);
        if (!wr.has_value()) {
            slot.publish(make_unexpected<std::size_t>(wr.error()));
            return;
        }
        auto r = c.result();
        c.reset();
        slot.publish(r.has_value() ? Result<std::size_t>{r.value()}
                                   : make_unexpected<std::size_t>(r.error()));
    });
    t.check(submitted.has_value(), "the task is admitted");

    Result<std::size_t> result = slot.wait_and_take();
    t.check(result.has_value() && result.value() == 0,
            "an await on a completion published at submit returns through the ready edge");
    t.check(SchedulerTestAccess::wait_registry_live_count(sched) == 0,
            "the already-terminal attach suspended no fiber");

    rt->request_stop();
    t.check(rt->drain().has_value(), "the runtime drains");
    t.check(rt->join().has_value(), "the runtime joins");
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

    RuntimeBuilder builder;
    builder.backend(std::move(backend));
    builder.workers(1);
    builder.test_wait_capacity(1);
    auto build = builder.build();
    t.check(build.has_value(), "the runtime builds");
    if (!build.has_value())
        return false;
    std::unique_ptr<ApplicationRuntime> rt = std::move(build.value());
    t.check(rt->start().has_value(), "the runtime starts");
    Scheduler& sched = rt->test_scheduler_for_worker_topology();
    RequestCore& core = *rt->test_io_context().context_core_for_test();
    t.check(SchedulerTestAccess::configured_wait_capacity(sched) == 1,
            "the host wait-record pool is bounded to a single record");

    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    raw->set_worker_claimed_pause_gate(&gate);

    TaskResultSlot<Result<std::size_t>> slot_a;
    TaskResultSlot<Result<std::size_t>> slot_b;
    std::vector<std::byte> buffer_a(31, std::byte{0});
    std::vector<std::byte> buffer_b(15, std::byte{0});
    std::atomic<bool> await_b_failed_with_no_space{false};
    std::atomic<bool> holder_a_finished{false};

    auto submitted_a = rt->submit([&](RuntimeTaskContext& ctx) {
        Completion<std::size_t> c;
        auto sr = ctx.submit_read(ReadOp{NativeFileRef{file}, buffer_a.data(), 31, 0}, c);
        if (!sr.has_value()) {
            slot_a.publish(make_unexpected<std::size_t>(sr.error()));
            return;
        }
        auto wr = ctx.await_completion(c);
        if (!wr.has_value()) {
            slot_a.publish(make_unexpected<std::size_t>(wr.error()));
            return;
        }
        auto r = c.result();
        c.reset();
        if (!r.has_value()) {
            slot_a.publish(make_unexpected<std::size_t>(r.error()));
            return;
        }
        slot_a.publish(r.value());
        holder_a_finished.store(true, std::memory_order_release);
    });
    t.check(submitted_a.has_value(), "the record-holder task is admitted");

    auto submitted_b = rt->submit([&](RuntimeTaskContext& ctx) {
        Completion<std::size_t> c;
        auto sr = ctx.submit_read(ReadOp{NativeFileRef{file}, buffer_b.data(), 15, 0}, c);
        if (!sr.has_value()) {
            slot_b.publish(make_unexpected<std::size_t>(sr.error()));
            return;
        }
        auto wr = ctx.await_completion(c);
        if (!wr.has_value() && wr.error().code == IoError::Code::no_space) {
            await_b_failed_with_no_space.store(true, std::memory_order_release);
            ctx.suspend(holder_a_finished);
            wr = ctx.await_completion(c);
        }
        if (!wr.has_value()) {
            slot_b.publish(make_unexpected<std::size_t>(wr.error()));
            return;
        }
        auto r = c.result();
        c.reset();
        if (!r.has_value()) {
            slot_b.publish(make_unexpected<std::size_t>(r.error()));
            return;
        }
        slot_b.publish(r.value());
    });
    t.check(submitted_b.has_value(), "the exhausted task is admitted");

    wait_gate_paused(gate);

    bool failure_observed = false;
    for (int i = 0; i < 500000; ++i) {
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

    Result<std::size_t> result_a = slot_a.wait_and_take();
    t.check(result_a.has_value() && result_a.value() == 31,
            "the record holder completes with its payload");
    t.check(std::memcmp(buffer_a.data(), "sluice obs04 exhaustion payload", 31) == 0,
            "the record holder carries the published bytes");

    Result<std::size_t> result_b = slot_b.wait_and_take();
    t.check(result_b.has_value() && result_b.value() == 15,
            "the refused await settles its request through the normal await edge");
    t.check(std::memcmp(buffer_b.data(), "sluice obs04 ex", 15) == 0,
            "the refused operation completed without cancellation");
    t.check(wait_registry_live_becomes(sched, 0) == 0,
            "no wait record survives the exhausted await");

    const CoreSnapshot settled_snapshot = core.snapshot();
    t.check(settled_snapshot.free_slots == core.capacity() &&
                settled_snapshot.accepted_live == 0,
            "every slot reclaims with no observer pin after the failure path");

    rt->request_stop();
    t.check(rt->drain().has_value(), "the runtime drains");
    t.check(rt->join().has_value(), "the runtime joins");
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

    RuntimeBuilder builder;
    builder.backend(std::move(backend));
    builder.workers(1);
    auto build = builder.build();
    t.check(build.has_value(), "the runtime builds");
    if (!build.has_value())
        return false;
    std::unique_ptr<ApplicationRuntime> rt = std::move(build.value());
    t.check(rt->start().has_value(), "the runtime starts");
    Scheduler& sched = rt->test_scheduler_for_worker_topology();
    RequestCore& core = *rt->test_io_context().context_core_for_test();

    ThreadPoolBackend::WorkerClaimedPauseGate work_gate;
    ThreadPoolBackend::DeliveryClaimedPauseGate delivery_gate;
    raw->set_worker_claimed_pause_gate(&work_gate);
    raw->set_delivery_claimed_pause_gate(&delivery_gate);

    TaskResultSlot<Result<std::size_t>> slot;
    std::vector<std::byte> buffer(34, std::byte{0});
    std::atomic<Completion<std::size_t>*> pending{nullptr};

    auto submitted = rt->submit([&](RuntimeTaskContext& ctx) {
        Completion<std::size_t> c;
        auto sr = ctx.submit_read(ReadOp{NativeFileRef{file}, buffer.data(), 34, 0}, c);
        if (!sr.has_value()) {
            slot.publish(make_unexpected<std::size_t>(sr.error()));
            return;
        }
        pending.store(&c, std::memory_order_release);
        auto wr = ctx.await_completion(c);
        pending.store(nullptr, std::memory_order_release);
        if (!wr.has_value()) {
            slot.publish(make_unexpected<std::size_t>(wr.error()));
            return;
        }
        auto r = c.result();
        c.reset();
        if (!r.has_value()) {
            slot.publish(make_unexpected<std::size_t>(r.error()));
            return;
        }
        slot.publish(r.value());
    });
    t.check(submitted.has_value(), "the task is admitted");

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
    std::thread canceler([&] {
        auto canceled = sched.cancel_waiter(*suspended);
        cancel_outcome.store(canceled.has_value() ? (canceled.value() ? 1 : 0) : -1,
                             std::memory_order_release);
    });
    t.check(scheduler_global_lock_becomes_held(sched),
            "the canceling thread enters the scheduler window");

    resume_gate(delivery_gate);
    canceler.join();

    t.check(cancel_outcome.load(std::memory_order_acquire) == 0,
            "a cancel during the staged delivery window is refused");

    raw->set_worker_claimed_pause_gate(nullptr);
    raw->set_delivery_claimed_pause_gate(nullptr);

    Result<std::size_t> result = slot.wait_and_take();
    t.check(result.has_value() && result.value() == 34,
            "the wait completes through the normal delivery path");
    t.check(std::memcmp(buffer.data(), "sluice obs04 staged window payload", 34) == 0,
            "the delivered completion carries the published bytes");
    t.check(wait_registry_live_becomes(sched, 0) == 0,
            "no wait record survives the staged delivery");

    const CoreSnapshot settled_snapshot = core.snapshot();
    t.check(settled_snapshot.free_slots == core.capacity() &&
                settled_snapshot.accepted_live == 0,
            "the delivered slot reclaims with no observer pin");

    rt->request_stop();
    t.check(rt->drain().has_value(), "the runtime drains");
    t.check(rt->join().has_value(), "the runtime joins");
    return t.failures == 0;
}

}

int main() {
    struct NamedTest {
        const char* name;
        bool (*fn)(Tracker&);
    };
    const NamedTest tests[] = {
        {"completion_wake_rides_the_observer_adapter",
         completion_wake_rides_the_observer_adapter},
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
    if (failures != 0)
        return 1;
    std::printf("all %zu runtime waiter observer tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
