#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/async/request_handle.hpp>
#include <sluice/async/threadpool_backend.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <limits>
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
using sluice::Result;
using sluice::async::detail::CoreSnapshot;
using sluice::async::detail::PublicCancel;
using sluice::async::detail::PublicLookup;
using sluice::async::detail::RequestCore;
using sluice::async::detail::RequestKey;
using sluice::async::detail::SlotIndex;

constexpr std::uint64_t kUnrepresentableOffset = std::numeric_limits<std::uint64_t>::max();

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

std::string make_temp_file(const std::string& content) {
    char path[] = "/tmp/sluice_b1b_cutover_XXXXXX";
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
    auto opened = File::open(path, sluice::FileOpen{});
    ::unlink(path.c_str());
    if (!opened.has_value()) {
        t.check(false, "the temporary fixture file opens");
        return std::nullopt;
    }
    return std::optional<File>{std::move(opened.value())};
}

bool resolves_as(const Result<RequestHandleState>& r, RequestHandleState expected) {
    return r.has_value() && r.value() == expected;
}

template <class Gate> struct GateGuard {
    Gate& gate;
    bool rearmed = false;
    ~GateGuard() {
        if (!rearmed) {
            resume_threadpool_gate(gate);
            rearm_threadpool_gate(gate);
        }
    }
};

bool pre_accept_failure_leaves_no_core_residue(Tracker& t) {
    auto file = open_temp_file(t, "sluice b1b residue\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{2, 1});
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    std::byte buffer{std::byte{0}};
    Completion<std::size_t> c;
    auto rejected = ctx.submit_read(
        ReadOp{NativeFileRef(*file), &buffer, 1, kUnrepresentableOffset}, c);
    t.check(!rejected.has_value() && rejected.error().code == IoError::Code::invalid_argument,
            "an unrepresentable range is rejected as invalid_argument");
    t.check(c.idle(), "the rejected submission leaves the completion idle");

    const CoreSnapshot snap = core.snapshot();
    t.check(snap.reserved == 0 && snap.accepted_live == 0 && snap.public_bindings == 0 &&
                snap.execution_refs == 0 && snap.free_slots == core.capacity(),
            "the rejected submission leaves no core residue and no leaked capacity");
    return true;
}

bool range_precedes_capacity_under_saturation(Tracker& t) {
    auto file = open_temp_file(t, "sluice b1b saturation\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{1, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    raw->set_worker_claimed_pause_gate(&gate);
    GateGuard guard{gate};

    std::vector<std::byte> buffer(4, std::byte{0});
    Completion<std::size_t> first;
    auto submitted = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0}, first);
    t.check(submitted.has_value(), "the first request occupies the only slot");
    wait_threadpool_gate_paused(gate);

    Completion<std::size_t> invalid_range;
    auto range_rejected =
        ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 1, kUnrepresentableOffset},
                        invalid_range);
    t.check(!range_rejected.has_value() &&
                range_rejected.error().code == IoError::Code::invalid_argument,
            "an invalid range outranks capacity under saturation");

    Completion<std::size_t> valid;
    auto capacity_rejected =
        ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 1, 0}, valid);
    t.check(!capacity_rejected.has_value() &&
                capacity_rejected.error().code == IoError::Code::would_block,
            "an otherwise valid request reports capacity exhaustion immediately");
    t.check(core.snapshot().accepted_live == 1, "the table holds exactly the accepted request");

    resume_threadpool_gate(gate);
    wait_threadpool_gate_exited(gate);
    guard.rearmed = true;
    rearm_threadpool_gate(gate);
    raw->set_worker_claimed_pause_gate(nullptr);

    while (!first.ready())
        (void)ctx.poll();
    first.reset();
    return true;
}

bool accepted_pre_dispatch_work_has_a_core_owner(Tracker& t) {
    auto file = open_temp_file(t, "sluice b1b ownership gap\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{2, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    ThreadPoolBackend::AcceptedPreDispatchPauseGate gate;
    raw->set_accepted_pre_dispatch_pause_gate(&gate);
    GateGuard guard{gate};

    std::vector<std::byte> buffer(4, std::byte{0});
    Completion<std::size_t> c;
    std::optional<Result<void>> submit_result;
    std::thread submitter([&] {
        submit_result = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0}, c);
    });
    wait_threadpool_gate_paused(gate);

    const CoreSnapshot snap = core.snapshot();
    t.check(snap.accepted_live == 1 && snap.public_bindings == 1 && snap.execution_refs == 1,
            "the accepted-undispatched request holds a core slot, binding and execution ref");
    const auto slot0 = core.observe_slot(SlotIndex{0});
    t.check(slot0.has_value() && slot0->phase == RequestCore::SlotPhase::accepted &&
                !slot0->terminal_chosen && !slot0->execution_claimed && slot0->execution_refs == 1,
            "acceptance installed the pending-dispatch execution responsibility");
    t.check(raw->dispatch_size_for_test() == 0, "the dispatch queue does not hold it yet");

    resume_threadpool_gate(gate);
    submitter.join();
    guard.rearmed = true;
    rearm_threadpool_gate(gate);
    raw->set_accepted_pre_dispatch_pause_gate(nullptr);
    t.check(submit_result.has_value() && submit_result->has_value(),
            "the submission itself succeeds");

    while (!c.ready())
        (void)ctx.poll();
    t.check(c.result().has_value() && c.result().value() == 4, "the request completes normally");
    c.reset();
    t.check(core.snapshot().accepted_live == 0, "the slot reclaims after release");
    return true;
}

bool cancel_during_accept_dispatch_window_converges_through_terminal(Tracker& t) {
    auto file = open_temp_file(t, "sluice b1b window cancel\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{2, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    ThreadPoolBackend::AcceptedPreDispatchPauseGate gate;
    raw->set_accepted_pre_dispatch_pause_gate(&gate);
    GateGuard guard{gate};

    std::vector<std::byte> buffer(4, std::byte{0});
    Completion<std::size_t> c;
    std::thread submitter([&] {
        (void)ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0}, c);
    });
    wait_threadpool_gate_paused(gate);

    const auto key = raw->request_key_for_test(c);
    t.check(key.has_value(), "the completion binding carries the core request key");
    const PublicCancel disposition = raw->cancel_key_for_test(*key);
    t.check(disposition == PublicCancel::won_before_execution,
            "cancel wins while dispatch handoff has not completed");

    resume_threadpool_gate(gate);
    submitter.join();
    guard.rearmed = true;
    rearm_threadpool_gate(gate);
    raw->set_accepted_pre_dispatch_pause_gate(nullptr);

    while (raw->dispatch_size_for_test() != 0)
        std::this_thread::yield();
    while (!c.ready())
        (void)ctx.poll();
    t.check(!c.result().has_value() && c.result().error().code == IoError::Code::canceled,
            "the canceled request converges through its terminal path");
    t.check(raw->syscall_count_for_test() == 0, "no physical syscall ever ran");
    c.reset();
    t.check(core.snapshot().accepted_live == 0, "the canceled slot reclaims");
    return true;
}

bool post_accept_dispatch_failure_is_a_terminal_not_a_rejection(Tracker& t) {
    auto file = open_temp_file(t, "sluice b1b dispatch failure\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{2, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    ThreadPoolBackend::DispatchFailureInjection injection;
    injection.armed.store(true, std::memory_order_release);
    raw->set_dispatch_failure_injection(&injection);

    std::vector<std::byte> buffer(4, std::byte{0});
    Completion<std::size_t> c;
    auto submitted = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0}, c);
    t.check(submitted.has_value(), "the submission is accepted, not rejected");
    t.check(injection.fired.load() == 1, "the injected dispatch failure fired once");
    t.check(raw->syscall_count_for_test() == 0, "no worker ever ran the operation");
    t.check(raw->publication_pending_size_for_test() == 1,
            "the failed handoff queued its own publication");

    (void)ctx.poll();
    t.check(c.ready(), "the terminal is published");
    t.check(!c.result().has_value() &&
                c.result().error().code == IoError::Code::backend_error,
            "the post-accept failure surfaces as the request's terminal error");
    const auto slot0 = core.observe_slot(SlotIndex{0});
    t.check(slot0.has_value() && slot0->published && slot0->binding_live,
            "the core published the terminal while the public binding is still live");
    c.reset();
    t.check(core.snapshot().accepted_live == 0, "the failed slot reclaims after release");
    return true;
}

bool claim_chain_and_running_cancel_do_not_release_the_borrow(Tracker& t) {
    auto file = open_temp_file(t, "sluice b1b running cancel\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{2, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    raw->set_worker_claimed_pause_gate(&gate);
    GateGuard guard{gate};

    std::vector<std::byte> buffer(4, std::byte{0});
    Completion<std::size_t> c;
    auto submitted = ctx.submit_read_request(
        ReadOp{NativeFileRef(*file), buffer.data(), 4, 0}, c);
    t.check(submitted.has_value(), "the request is accepted");
    const RequestHandle handle = submitted.value();
    wait_threadpool_gate_paused(gate);

    const auto slot0 = core.observe_slot(SlotIndex{0});
    t.check(slot0.has_value() && slot0->execution_claimed && slot0->execution_refs == 1 &&
                !slot0->terminal_chosen,
            "the worker dequeue claimed execution and the borrow-touching ref is live");
    t.check(!c.ready() && ctx.poll() == 0, "nothing publishes while execution is live");

    ctx.cancel(c);
    const auto after_cancel = core.observe_slot(SlotIndex{0});
    t.check(after_cancel.has_value() && after_cancel->cancel_intent &&
                after_cancel->execution_refs == 1 && !after_cancel->terminal_chosen,
            "cancel of a running blocking syscall records intent without retiring the borrow");
    t.check(resolves_as(ctx.request_state(handle), RequestHandleState::outstanding),
            "the request stays outstanding after the cancel intent");

    resume_threadpool_gate(gate);
    wait_threadpool_gate_exited(gate);
    guard.rearmed = true;
    rearm_threadpool_gate(gate);
    raw->set_worker_claimed_pause_gate(nullptr);

    while (!c.ready())
        (void)ctx.poll();
    t.check(c.result().has_value() && c.result().value() == 4,
            "the physical outcome wins over the earlier cancel intent");
    c.reset();
    return true;
}

bool last_borrow_access_precedes_final_execution_retirement(Tracker& t) {
    auto file = open_temp_file(t, "sluice b1b last access\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{2, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    ThreadPoolBackend::WorkerOutcomePreTerminalPauseGate gate;
    raw->set_worker_outcome_pre_terminal_pause_gate(&gate);
    GateGuard guard{gate};

    std::vector<std::byte> buffer(4, std::byte{0});
    Completion<std::size_t> c;
    (void)ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0}, c);
    wait_threadpool_gate_paused(gate);

    t.check(raw->syscall_count_for_test() == 1, "the physical syscall has completed");
    const auto slot0 = core.observe_slot(SlotIndex{0});
    t.check(slot0.has_value() && slot0->execution_refs == 1 && !slot0->terminal_chosen,
            "the execution ref outlives the last physical buffer access");
    t.check(!c.ready() && ctx.poll() == 0 && raw->publication_pending_size_for_test() == 0,
            "no publication is queued while the borrow-touching ref is live");

    resume_threadpool_gate(gate);
    wait_threadpool_gate_exited(gate);
    guard.rearmed = true;
    rearm_threadpool_gate(gate);
    raw->set_worker_outcome_pre_terminal_pause_gate(nullptr);

    while (raw->publication_pending_size_for_test() == 0)
        std::this_thread::yield();
    t.check(raw->publication_pending_size_for_test() == 1,
            "terminal selection and ref retirement queue the publication");
    while (!c.ready())
        (void)ctx.poll();
    t.check(c.result().has_value() && c.result().value() == 4, "the outcome publishes");
    c.reset();
    t.check(core.snapshot().accepted_live == 0, "reclaim follows release");
    return true;
}

bool release_racing_the_publication_epilogue_reclaims_without_new_io(Tracker& t) {
    auto file = open_temp_file(t, "sluice b1b epilogue race\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{2, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    ThreadPoolBackend::PublicationEpiloguePauseGate gate;
    raw->set_publication_epilogue_pause_gate(&gate);
    GateGuard guard{gate};

    std::vector<std::byte> buffer(4, std::byte{0});
    Completion<std::size_t> c;
    auto submitted = ctx.submit_read_request(
        ReadOp{NativeFileRef(*file), buffer.data(), 4, 0}, c);
    t.check(submitted.has_value(), "the request is accepted");
    const RequestHandle handle = submitted.value();
    const auto key = raw->request_key_for_test(c);
    t.check(key.has_value(), "the completion binding carries the request key");

    while (raw->publication_pending_size_for_test() == 0)
        std::this_thread::yield();
    std::thread publisher([&] { (void)ctx.poll(); });
    wait_threadpool_gate_paused(gate);

    t.check(c.ready(), "the compatibility result is visible once the ready edge fires");
    const auto inflight = core.observe_slot(SlotIndex{0});
    t.check(inflight.has_value() && inflight->publication_inflight && !inflight->published &&
                inflight->binding_live,
            "the publisher holds the publication pin between result write and completion");

    c.reset();
    const auto released = core.observe_slot(SlotIndex{0});
    t.check(released.has_value() && !released->binding_live && released->publication_inflight &&
                released->phase == RequestCore::SlotPhase::accepted,
            "the public release invalidates the binding immediately while the pin holds the slot");
    t.check(core.lookup(*key) == PublicLookup::not_found,
            "the public identity is already unresolvable");
    t.check(core.occupancy().accepted_live == 1, "the slot is pinned, not yet reclaimed");

    resume_threadpool_gate(gate);
    publisher.join();
    guard.rearmed = true;
    rearm_threadpool_gate(gate);
    raw->set_publication_epilogue_pause_gate(nullptr);

    t.check(core.occupancy().accepted_live == 0,
            "the final pin retirement reclaims synchronously without unrelated new I/O");
    t.check(resolves_as(ctx.request_state(handle), RequestHandleState::not_found),
            "the released identity stays unresolvable after the epilogue completes");

    Completion<std::size_t> reused;
    auto resubmitted = ctx.submit_read_request(
        ReadOp{NativeFileRef(*file), buffer.data(), 4, 0}, reused);
    t.check(resubmitted.has_value(), "the reclaimed slot is reusable");
    while (!reused.ready())
        (void)ctx.poll();
    reused.reset();
    return true;
}

bool zero_op_publishes_at_acceptance_without_dispatch(Tracker& t) {
    auto file = open_temp_file(t, "sluice b1b zero op\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{2, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    const std::uint64_t syscalls_before = raw->syscall_count_for_test();
    Completion<std::size_t> c;
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef(*file), nullptr, 0, kUnrepresentableOffset}, c);
    t.check(submitted.has_value(), "the zero-op occupies a slot and is accepted");
    t.check(c.ready(), "the zero-op is published at acceptance");
    t.check(raw->syscall_count_for_test() == syscalls_before, "no data syscall is dispatched");
    t.check(c.result().has_value() && c.result().value() == 0, "the zero-op result is zero bytes");
    const auto slot0 = core.observe_slot(SlotIndex{0});
    t.check(slot0.has_value() && slot0->published && slot0->binding_live,
            "the core holds the published zero-op");
    t.check(raw->event_owed_for_test(0), "the zero-op still owes its delivery event");

    (void)ctx.poll();
    t.check(raw->sink_deliveries() == 1, "the owed delivery event fires on the next progress");
    t.check(!raw->event_owed_for_test(0), "the owed event is discharged");

    c.reset();
    t.check(core.snapshot().accepted_live == 0, "the zero-op slot reclaims after release");
    return true;
}

bool zero_op_delivery_pins_the_slot_until_the_event_is_delivered(Tracker& t) {
    auto file = open_temp_file(t, "sluice b1b deferred pin\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{1, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    const std::uint64_t syscalls_before = raw->syscall_count_for_test();

    Completion<std::size_t> c;
    auto submitted =
        ctx.submit_read(ReadOp{NativeFileRef(*file), nullptr, 0, kUnrepresentableOffset}, c);
    t.check(submitted.has_value(), "the zero-op occupies the only slot and is accepted");
    t.check(c.ready() && c.result().has_value() && c.result().value() == 0,
            "the zero-op is published at acceptance");
    const auto key = raw->request_key_for_test(c);
    t.check(key.has_value(), "the completion carries the zero-op request key");
    const auto published_live = core.observe_slot(SlotIndex{0});
    t.check(published_live.has_value() && published_live->published &&
                published_live->binding_live && published_live->control_refs == 1,
            "the deferred delivery obligation pins the core slot");
    t.check(raw->event_owed_for_test(0), "the ready event is owed");

    c.reset();
    const auto pinned = core.observe_slot(SlotIndex{0});
    t.check(pinned.has_value() && pinned->phase == RequestCore::SlotPhase::accepted &&
                pinned->published && !pinned->binding_live && pinned->control_refs == 1,
            "resetting before the poll releases the binding but cannot reclaim the slot");
    t.check(raw->event_owed_for_test(0), "the delivery obligation survives the release");
    t.check(core.lookup(*key) == PublicLookup::not_found,
            "the released identity stops resolving while the event is still owed");

    std::vector<std::byte> buffer(4, std::byte{0});
    Completion<std::size_t> next;
    auto refused = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0}, next);
    t.check(!refused.has_value() && refused.error().code == IoError::Code::would_block,
            "the pinned slot cannot be reused before the owed event is delivered");
    t.check(raw->event_owed_for_test(0) && next.idle(),
            "the refused submission leaves the owed record untouched");

    const std::size_t events = ctx.poll();
    t.check(events == 1, "the poll delivers exactly the owed event");
    t.check(!raw->event_owed_for_test(0), "the owed event is discharged");
    t.check(raw->sink_deliveries() == 1, "the ready event is delivered exactly once");
    t.check(raw->sink_last_key() == *key, "the delivered event carries the old request key");

    const auto after = core.observe_slot(SlotIndex{0});
    t.check(after.has_value() && after->phase == RequestCore::SlotPhase::free &&
                after->generation.value == pinned->generation.value + 1 &&
                core.snapshot().accepted_live == 0,
            "the final pin retirement reclaims synchronously");
    t.check(raw->syscall_count_for_test() == syscalls_before,
            "the reclaim needed no unrelated new I/O");

    auto reused = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0}, next);
    t.check(reused.has_value(), "the reclaimed slot serves a new request");
    while (!next.ready())
        (void)ctx.poll();
    t.check(next.result().has_value() && next.result().value() == 4,
            "the new request completes normally");
    next.reset();
    t.check(core.snapshot().accepted_live == 0 && core.snapshot().free_slots == core.capacity(),
            "the context drains fully afterwards");
    return true;
}

bool admission_close_between_reserve_and_accept_refuses_and_rolls_back(Tracker& t) {
    auto file = open_temp_file(t, "sluice b1b close race\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{2, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    ThreadPoolBackend::PreAcceptCommitPauseGate gate;
    raw->set_pre_accept_commit_pause_gate(&gate);
    GateGuard guard{gate};

    std::vector<std::byte> buffer(4, std::byte{0});
    Completion<std::size_t> c;
    std::optional<Result<void>> submit_result;
    std::thread submitter([&] {
        submit_result = ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0}, c);
    });
    wait_threadpool_gate_paused(gate);

    t.check(core.snapshot().reserved == 1, "the reservation is not yet an acceptance");
    raw->close_admission();

    resume_threadpool_gate(gate);
    submitter.join();
    guard.rearmed = true;
    rearm_threadpool_gate(gate);
    raw->set_pre_accept_commit_pause_gate(nullptr);

    t.check(submit_result.has_value() && !submit_result->has_value() &&
                submit_result->error().code == IoError::Code::invalid_state,
            "the close race converts to an admission rejection");
    t.check(c.idle(), "the rolled-back completion is idle");
    const CoreSnapshot snap = core.snapshot();
    t.check(snap.reserved == 0 && snap.accepted_live == 0 && snap.free_slots == core.capacity(),
            "the reservation rolls back with no residue");
    return true;
}

bool waiter_registration_and_delivery_ride_the_publication(Tracker& t) {
    auto file = open_temp_file(t, "sluice b1b waiter\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{2, 1});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));

    ThreadPoolBackend::WorkerClaimedPauseGate gate;
    raw->set_worker_claimed_pause_gate(&gate);
    GateGuard guard{gate};

    std::vector<std::byte> buffer(4, std::byte{0});
    Completion<std::size_t> c;
    (void)ctx.submit_read(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0}, c);
    wait_threadpool_gate_paused(gate);

    const auto key = raw->request_key_for_test(c);
    t.check(key.has_value(), "the request key resolves for waiter registration");
    const sluice::async::detail::WaiterToken token{7, 1, 1};
    auto registered = raw->register_waiter_key_for_test(
        *key, token, sluice::async::detail::RoutingLease::pinning(11, 0, 0));
    t.check(registered.has_value(), "the waiter registers on the outstanding request");
    t.check(!raw->register_waiter_key_for_test(
                *key, token, sluice::async::detail::RoutingLease::pinning(12, 0, 0))
                 .has_value(),
            "a second registration on the same request is refused");

    resume_threadpool_gate(gate);
    wait_threadpool_gate_exited(gate);
    guard.rearmed = true;
    rearm_threadpool_gate(gate);
    raw->set_worker_claimed_pause_gate(nullptr);

    while (!c.ready())
        (void)ctx.poll();
    t.check(raw->sink_deliveries() >= 1 && raw->sink_last_has_waiter(),
            "the publication delivers a ready event carrying the waiter");
    t.check(raw->sink_last_token() == token, "the delivered event carries the registered token");
    t.check(raw->sink_last_lease_id() == 11, "the delivered event carries the routing lease");
    const auto waiter = raw->waiter_of_slot_for_test(0);
    t.check(waiter.has_value() &&
                waiter->registration == sluice::async::detail::WaiterRegistration::closed &&
                !waiter->delivery_present,
            "the registration closes at delivery");
    c.reset();
    return true;
}

bool stale_identity_does_not_resolve_after_slot_reuse(Tracker& t) {
    auto file = open_temp_file(t, "sluice b1b stale identity\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{2, 1});
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    std::vector<std::byte> buffer(4, std::byte{0});
    Completion<std::size_t> first;
    auto first_submitted =
        ctx.submit_read_request(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0}, first);
    t.check(first_submitted.has_value(), "the first request is accepted");
    const RequestHandle stale = first_submitted.value();
    while (!first.ready())
        (void)ctx.poll();
    first.reset();
    t.check(resolves_as(ctx.request_state(stale), RequestHandleState::not_found),
            "the released identity stops resolving");

    Completion<std::size_t> second;
    auto second_submitted =
        ctx.submit_read_request(ReadOp{NativeFileRef(*file), buffer.data(), 4, 0}, second);
    t.check(second_submitted.has_value(), "the reclaimed slot serves a new request");
    t.check(resolves_as(ctx.request_state(stale), RequestHandleState::not_found),
            "the stale identity does not resolve against the reused slot");
    t.check(resolves_as(ctx.request_state(second_submitted.value()),
                        RequestHandleState::outstanding),
            "the new identity resolves while outstanding");
    while (!second.ready())
        (void)ctx.poll();
    second.reset();
    t.check(core.snapshot().accepted_live == 0, "both occupancies reclaimed");
    return true;
}

bool bounded_workers_execute_the_syscalls_not_the_submitter(Tracker& t) {
    auto file = open_temp_file(t, "sluice b1b workers\n");
    if (!file.has_value())
        return false;

    auto backend = std::make_unique<ThreadPoolBackend>(ThreadPoolConfig{4, 3});
    ThreadPoolBackend* raw = backend.get();
    AsyncIoContext ctx(std::move(backend));
    RequestCore& core = *ctx.context_core_for_test();

    std::vector<std::vector<std::byte>> buffers;
    std::vector<Completion<std::size_t>> completions(4);
    for (std::size_t i = 0; i < 4; ++i) {
        buffers.emplace_back(4, std::byte{0});
    }
    for (std::size_t i = 0; i < 4; ++i) {
        auto submitted = ctx.submit_read(
            ReadOp{NativeFileRef(*file), buffers[i].data(), 4, 0}, completions[i]);
        t.check(submitted.has_value(), "each submission is accepted without waiting");
    }
    for (std::size_t i = 0; i < 4; ++i) {
        while (!completions[i].ready())
            (void)ctx.poll();
        t.check(completions[i].result().has_value() && completions[i].result().value() == 4,
                "each request publishes its own outcome");
        completions[i].reset();
    }
    t.check(raw->syscall_count_for_test() == 4, "every syscall ran on a worker");
    t.check(raw->workers_spawned_for_test() == 3, "the configured worker bound held");
    t.check(core.snapshot().accepted_live == 0 && core.snapshot().free_slots == core.capacity(),
            "the bounded table is fully reclaimed");
    return true;
}

struct NamedTest {
    const char* name;
    bool (*fn)(Tracker&);
};

}

int main() {
    const NamedTest tests[] = {
        {"pre_accept_failure_leaves_no_core_residue", pre_accept_failure_leaves_no_core_residue},
        {"range_precedes_capacity_under_saturation", range_precedes_capacity_under_saturation},
        {"accepted_pre_dispatch_work_has_a_core_owner", accepted_pre_dispatch_work_has_a_core_owner},
        {"cancel_during_accept_dispatch_window_converges_through_terminal",
         cancel_during_accept_dispatch_window_converges_through_terminal},
        {"post_accept_dispatch_failure_is_a_terminal_not_a_rejection",
         post_accept_dispatch_failure_is_a_terminal_not_a_rejection},
        {"claim_chain_and_running_cancel_do_not_release_the_borrow",
         claim_chain_and_running_cancel_do_not_release_the_borrow},
        {"last_borrow_access_precedes_final_execution_retirement",
         last_borrow_access_precedes_final_execution_retirement},
        {"release_racing_the_publication_epilogue_reclaims_without_new_io",
         release_racing_the_publication_epilogue_reclaims_without_new_io},
        {"zero_op_publishes_at_acceptance_without_dispatch",
         zero_op_publishes_at_acceptance_without_dispatch},
        {"zero_op_delivery_pins_the_slot_until_the_event_is_delivered",
         zero_op_delivery_pins_the_slot_until_the_event_is_delivered},
        {"admission_close_between_reserve_and_accept_refuses_and_rolls_back",
         admission_close_between_reserve_and_accept_refuses_and_rolls_back},
        {"waiter_registration_and_delivery_ride_the_publication",
         waiter_registration_and_delivery_ride_the_publication},
        {"stale_identity_does_not_resolve_after_slot_reuse",
         stale_identity_does_not_resolve_after_slot_reuse},
        {"bounded_workers_execute_the_syscalls_not_the_submitter",
         bounded_workers_execute_the_syscalls_not_the_submitter},
    };
    int failed = 0;
    for (const NamedTest& test : tests) {
        Tracker tracker{test.name};
        const bool reached_end = test.fn(tracker);
        if (!reached_end || tracker.failures != 0) {
            ++failed;
        }
    }
    if (failed != 0) {
        std::fprintf(stderr, "%d of %zu threadpool core cutover tests failed\n", failed,
                     sizeof(tests) / sizeof(tests[0]));
        return 1;
    }
    std::printf("all %zu threadpool core cutover tests passed\n",
                sizeof(tests) / sizeof(tests[0]));
    return 0;
}
