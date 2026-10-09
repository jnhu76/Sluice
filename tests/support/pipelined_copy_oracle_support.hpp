#pragma once

// e1 (#474): COPY-B pipelined-copy contract oracles over the current
// run_pipelined_copy_with_backend API, driven through a scripted backend.
//
// Backend model: submit accepts through RequestCore exactly like a v1 request
// backend (reserve -> begin_binding -> accept(borrow facts) -> core binding ->
// commit -> claim_execution). Accepted ops stay completable until the test
// stages an outcome: the fill/commit of the read/write borrow happens BEFORE
// offer_terminal + release_execution (the borrow is legal to touch exactly in
// that window), then the publication is queued; the runtime driver's poll()
// publishes (begin_publication -> publish -> complete_publication -> observer
// delivery), mirroring ThreadPoolBackend's worker/publication split. The
// accept-time overlap check against every borrow still live (accepted through
// release_execution) turns premature task-side buffer reuse into a
// deterministic violation record instead of a data race; overlap is decided
// on integer address intervals because the slot buffers come from distinct
// allocations, where relational pointer comparison has no defined order.
// peak_borrow_live(kind) is the high-water count of ops inside that same
// backend-physical borrow window (accept .. release_execution) and is the
// metric backing the at-most-one-in-flight-write oracle; public-binding
// outstanding (accept .. publication delivery) is a longer, distinct interval
// and is deliberately not what that oracle measures. Execution is claimed
// at submit (as the uring transport does), so a core cancel of an accepted op
// resolves physical_interruption_unsupported; the won_before_execution
// publication branch is defensive and unexercised by these scenarios.
//
// CONTRACT_DECISION_REQUIRED (error precedence, waiter level): copy_task's
// phase-3 cleanup returns a drain WAITER failure (await_completion error)
// before the primary error. No assertion freezes that precedence as a
// contract: with a contract-conforming backend the waiter-failure arms are
// unreachable through the public single-task API (one task fiber holds at most
// one live wait, so wait-record exhaustion cannot happen; observer-attach
// failure on a non-ready completion requires losing a live accepted request;
// waiter cancellation requires a runtime control action, and
// run_task_to_result only requests stop after the task published). The
// reachable arm IS asserted: op RESULTS of completions drained after a failure
// never override the primary error (await_drain discards c.result()).
//
// Every scenario ends drained (backend pending empty) or the harness watchdog
// dumps diagnostics and aborts the process: a broken pipeline fails bounded,
// never hangs the suite.

#include "copy_task.hpp"

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/reference_ready_sink.hpp>
#include <sluice/async/detail/request_core.hpp>
#include <sluice/detail/file_semantics.hpp>
#include <sluice/file_resource.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sluice_copy_oracle {

using namespace sluice::async;
using sluice::File;
using sluice::FileAccess;
using sluice::FileInfo;
using sluice::FileOpen;
using sluice::FileSize;
using sluice::IoError;
using sluice::Result;
using namespace sluice_copy;

constexpr std::size_t kDefaultSlots = 64;
constexpr auto kDriveBound = std::chrono::seconds(10);
constexpr auto kWaitFor = std::chrono::milliseconds(5000);

inline int g_checks_failed = 0;
inline std::vector<std::string> g_failures;

inline void check(bool ok, const char* what) {
    if (!ok) {
        ++g_checks_failed;
        g_failures.push_back(what);
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

inline void check_msg(bool ok, const std::string& what) {
    check(ok, what.c_str());
}

inline std::byte seed_byte(std::uint64_t offset) {
    return std::byte{static_cast<unsigned char>((offset * 131 + 7) & 0xFF)};
}

// Callers skip null/empty borrows, so both intervals are non-empty. uintptr_t
// holds the object's address on the supported targets (Linux x86_64).
inline bool address_intervals_overlap(const std::byte* a, std::size_t a_len,
                                      const std::byte* b, std::size_t b_len) {
    const std::uintptr_t a_lo = reinterpret_cast<std::uintptr_t>(a);
    const std::uintptr_t b_lo = reinterpret_cast<std::uintptr_t>(b);
    return a_lo < b_lo + b_len && b_lo < a_lo + a_len;
}

inline detail::OperationKind kind_of(detail::RequestOp op) {
    switch (op) {
    case detail::RequestOp::read:
        return detail::OperationKind::read;
    case detail::RequestOp::write:
        return detail::OperationKind::write;
    case detail::RequestOp::sync_data:
        return detail::OperationKind::sync_data;
    case detail::RequestOp::sync_all:
        return detail::OperationKind::sync_all;
    default:
        return detail::OperationKind::read;
    }
}

struct ScriptedOpView {
    std::uint64_t submit_id = 0;
    detail::OperationKind kind = detail::OperationKind::read;
    std::uint64_t offset = 0;
    std::size_t len = 0;
    void* completion = nullptr;
    std::byte* buffer = nullptr;
};

class ScriptedBackend;

struct ScriptedShared {
    struct Record {
        std::uint64_t submit_id = 0;
        detail::RequestKey key{};
        detail::OperationKind kind = detail::OperationKind::read;
        std::byte* buffer = nullptr;
        std::uint64_t offset = 0;
        std::size_t len = 0;
        void* completion = nullptr;
        void (*publish)(void*, const detail::PublicationPayload&) noexcept = nullptr;
        enum class Stage { accepted, staged, published } stage = Stage::accepted;
        bool borrow_released = false;
    };

    std::mutex mtx;
    std::condition_variable cv;
    std::map<std::uint32_t, Record> records;
    std::deque<detail::RequestKey> publication_queue;
    std::map<std::uint64_t, std::byte> shadow;

    // backend_gate_ serializes controller access to the backend object for as
    // long as the call runs; the backend destructor empties backend_raw under
    // the same gate, so a staged outcome never dereferences a dead backend.
    std::mutex backend_gate;
    ScriptedBackend* backend_raw = nullptr;

    std::uint64_t next_submit_id = 0;
    std::size_t accepted_count[6] = {};
    std::size_t peak_live[6] = {};
    std::size_t peak_borrow_live[6] = {};

    bool borrow_overlap_violation = false;
    std::string protocol_violation;
    bool closed = false;

    std::optional<IoError> fail_next[6];

    static int kind_index(detail::OperationKind k) { return static_cast<int>(k); }

    std::size_t live_count_locked(detail::OperationKind k) const {
        std::size_t n = 0;
        for (const auto& [slot, r] : records)
            if (r.kind == k && r.stage != Record::Stage::published)
                ++n;
        return n;
    }

    std::size_t completable_count_locked(detail::OperationKind k) const {
        std::size_t n = 0;
        for (const auto& [slot, r] : records)
            if (r.kind == k && r.stage == Record::Stage::accepted)
                ++n;
        return n;
    }

    static bool borrow_is_live(const Record& r) {
        return r.stage == Record::Stage::accepted ||
               (r.stage == Record::Stage::staged && !r.borrow_released);
    }

    std::size_t borrow_live_count_locked(detail::OperationKind k) const {
        std::size_t n = 0;
        for (const auto& [slot, r] : records)
            if (r.kind == k && borrow_is_live(r))
                ++n;
        return n;
    }

    void recompute_peak_locked(detail::OperationKind k) {
        const std::size_t live = live_count_locked(k);
        if (live > peak_live[kind_index(k)])
            peak_live[kind_index(k)] = live;
    }

    void recompute_borrow_peak_locked(detail::OperationKind k) {
        const std::size_t live = borrow_live_count_locked(k);
        if (live > peak_borrow_live[kind_index(k)])
            peak_borrow_live[kind_index(k)] = live;
    }
};

class ScriptedBackend final : public AsyncBackend {
  public:
    explicit ScriptedBackend(std::shared_ptr<ScriptedShared> shared, std::size_t slots)
        : shared_(std::move(shared)), slots_(slots) {
        std::lock_guard<std::mutex> lk(shared_->backend_gate);
        shared_->backend_raw = this;
    }

    ~ScriptedBackend() override {
        {
            std::lock_guard<std::mutex> lk(shared_->backend_gate);
            shared_->backend_raw = nullptr;
        }
        {
            std::lock_guard<std::mutex> lk(shared_->mtx);
            shared_->closed = true;
        }
        shared_->cv.notify_all();
    }

    bool supports_request_identity() const noexcept override { return true; }
    bool signals_physical_progress() const noexcept override { return true; }

    std::size_t poll() override { return poll_progress().completed; }

    ProgressPass poll_progress() override {
        std::size_t published = 0;
        for (;;) {
            detail::RequestKey key{};
            {
                std::lock_guard<std::mutex> lk(shared_->mtx);
                if (shared_->publication_queue.empty())
                    break;
                key = shared_->publication_queue.front();
                shared_->publication_queue.pop_front();
            }
            publish_one(key);
            ++published;
        }
        ProgressPass pass;
        pass.completed = published;
        pass.accepted_work_remains = core_ != nullptr && core_->occupancy().outstanding != 0;
        return pass;
    }

    std::size_t outstanding() const noexcept override {
        return core_ != nullptr ? core_->occupancy().outstanding : 0;
    }

    bool internal_work_retired() const noexcept override {
        std::lock_guard<std::mutex> lk(shared_->mtx);
        return shared_->publication_queue.empty();
    }

    void stop_execution() noexcept override {}

    void retire_execution_resources() noexcept override {
        bool queue_empty = true;
        {
            std::lock_guard<std::mutex> lk(shared_->mtx);
            queue_empty = shared_->publication_queue.empty();
        }
        const detail::CoreOccupancy occupancy =
            core_ != nullptr ? core_->occupancy() : detail::CoreOccupancy{};
        if (!queue_empty || occupancy.outstanding != 0 || occupancy.execution_refs != 0 ||
            occupancy.control_refs != 0 || occupancy.publication_inflight != 0 ||
            occupancy.observer_registrations != 0) {
            std::fprintf(stderr,
                         "ScriptedBackend: non-quiescent destruction (queue_empty=%d "
                         "outstanding=%zu exec=%zu ctrl=%zu pub=%zu obs=%zu)\n",
                         queue_empty ? 1 : 0, occupancy.outstanding, occupancy.execution_refs,
                         occupancy.control_refs, occupancy.publication_inflight,
                         occupancy.observer_registrations);
            std::fflush(stderr);
            std::abort();
        }
    }

    std::size_t slot_capacity() const noexcept override { return slots_; }

    detail::PublicCancel cancel_identity(detail::RequestKey key) override {
        {
            std::lock_guard<std::mutex> lk(shared_->mtx);
            auto it = shared_->records.find(key.slot.value);
            if (it != shared_->records.end() &&
                it->second.key.generation.value == key.generation.value &&
                it->second.stage == ScriptedShared::Record::Stage::accepted) {
                it->second.stage = ScriptedShared::Record::Stage::staged;
                shared_->cv.notify_all();
            }
        }
        detail::PublicCancel disposition = core_->cancel(key);
        if (disposition == detail::PublicCancel::won_before_execution) {
            (void)core_->release_execution(key);
            {
                std::lock_guard<std::mutex> lk(shared_->mtx);
                auto it = shared_->records.find(key.slot.value);
                if (it != shared_->records.end() &&
                    it->second.key.generation.value == key.generation.value) {
                    it->second.borrow_released = true;
                    shared_->recompute_borrow_peak_locked(it->second.kind);
                }
                shared_->publication_queue.push_back(key);
            }
            progress_port_.signal();
        } else if (disposition == detail::PublicCancel::requested) {
            disposition = detail::PublicCancel::physical_interruption_unsupported;
        }
        return disposition;
    }

    // Controller entry (test thread): terminal handoff + borrow retirement +
    // publication queueing + progress signal. Called while the controller
    // holds shared_->backend_gate.
    void stage_outcome(detail::RequestKey key, sluice::detail::IoOutcome outcome) {
        if (core_->offer_terminal(key, detail::TerminalCandidate{
                                           detail::TerminalCandidateKind::physical_outcome,
                                           outcome}) != detail::TerminalVerdict::chosen) {
            note_protocol_violation("offer_terminal rejected");
            return;
        }
        if (core_->release_execution(key) !=
            detail::ExecutionRelease::borrow_touch_fully_retired) {
            note_protocol_violation("release_execution rejected");
            return;
        }
        {
            std::lock_guard<std::mutex> lk(shared_->mtx);
            auto it = shared_->records.find(key.slot.value);
            if (it != shared_->records.end() &&
                it->second.key.generation.value == key.generation.value) {
                it->second.borrow_released = true;
                shared_->recompute_borrow_peak_locked(it->second.kind);
            }
        }
        {
            std::lock_guard<std::mutex> lk(shared_->mtx);
            shared_->publication_queue.push_back(key);
        }
        progress_port_.signal();
    }

  private:
    static void publish_size_completion(void* completion,
                                        const detail::PublicationPayload& payload) noexcept {
        const sluice::detail::IoOutcome& o = payload.outcome;
        AsyncBackend::publish(*static_cast<Completion<std::size_t>*>(completion),
                              o.succeeded
                                  ? Result<std::size_t>{static_cast<std::size_t>(
                                        o.effect.confirmed_bytes)}
                                  : make_unexpected<std::size_t>(o.error));
    }

    static void publish_void_completion(void* completion,
                                        const detail::PublicationPayload& payload) noexcept {
        const sluice::detail::IoOutcome& o = payload.outcome;
        AsyncBackend::publish(*static_cast<Completion<void>*>(completion),
                              o.succeeded ? Result<void>{} : make_unexpected<void>(o.error));
    }

    Result<detail::RequestKey> submit_read(ReadOp op, Completion<std::size_t>* c) override {
        if (c == nullptr)
            return make_unexpected<detail::RequestKey>(IoError{IoError::Code::invalid_argument});
        const auto rejection = sluice::detail::accept_or_reject(sluice::detail::precheck_data_op(
            {op.file.fd < 0, op.file.access, sluice::detail::FileOperation::read, op.offset,
             op.len}));
        if (!rejection.has_value())
            return make_unexpected<detail::RequestKey>(rejection.error());
        if (auto injected = consume_injected_failure(detail::OperationKind::read))
            return make_unexpected<detail::RequestKey>(*injected);
        return accept_size_op(op.file.fd, static_cast<std::byte*>(op.dst), op.offset, op.len,
                              detail::RequestOp::read, detail::OperationKind::read, c,
                              &publish_size_completion);
    }

    Result<detail::RequestKey> submit_write(WriteOp op, Completion<std::size_t>* c) override {
        if (c == nullptr)
            return make_unexpected<detail::RequestKey>(IoError{IoError::Code::invalid_argument});
        const auto rejection = sluice::detail::accept_or_reject(sluice::detail::precheck_data_op(
            {op.file.fd < 0, op.file.access, sluice::detail::FileOperation::write, op.offset,
             op.len}));
        if (!rejection.has_value())
            return make_unexpected<detail::RequestKey>(rejection.error());
        if (auto injected = consume_injected_failure(detail::OperationKind::write))
            return make_unexpected<detail::RequestKey>(*injected);
        return accept_size_op(op.file.fd, const_cast<std::byte*>(op.src), op.offset, op.len,
                              detail::RequestOp::write, detail::OperationKind::write, c,
                              &publish_size_completion);
    }

    Result<detail::RequestKey> submit_sync_data(SyncDataOp op, Completion<void>* c) override {
        return submit_sync(op, c, detail::OperationKind::sync_data,
                           detail::RequestOp::sync_data);
    }

    Result<detail::RequestKey> submit_sync_all(SyncAllOp op, Completion<void>* c) override {
        return submit_sync(op, c, detail::OperationKind::sync_all,
                           detail::RequestOp::sync_all);
    }

    Result<detail::RequestKey> submit_file_info(FileInfoOp, Completion<FileInfo>*) override {
        return make_unexpected<detail::RequestKey>(IoError{IoError::Code::not_supported});
    }

    Result<detail::RequestKey> submit_size(SizeOp, Completion<FileSize>*) override {
        return make_unexpected<detail::RequestKey>(IoError{IoError::Code::not_supported});
    }

    template <class Op>
    Result<detail::RequestKey> submit_sync(Op op, Completion<void>* c, detail::OperationKind kind,
                                           detail::RequestOp core_op) {
        if (c == nullptr)
            return make_unexpected<detail::RequestKey>(IoError{IoError::Code::invalid_argument});
        const auto rejection = sluice::detail::accept_or_reject(sluice::detail::precheck_state_op(
            op.file.fd < 0, op.file.access, sluice::detail::FileOperation::sync_data));
        if (!rejection.has_value())
            return make_unexpected<detail::RequestKey>(rejection.error());
        if (auto injected = consume_injected_failure(kind))
            return make_unexpected<detail::RequestKey>(*injected);
        return accept_void_op(op.file.fd, core_op, kind, c, &publish_void_completion);
    }

    std::optional<IoError> consume_injected_failure(detail::OperationKind kind) {
        std::lock_guard<std::mutex> lk(shared_->mtx);
        auto& armed = shared_->fail_next[ScriptedShared::kind_index(kind)];
        if (!armed.has_value())
            return std::nullopt;
        IoError e = *armed;
        armed.reset();
        return e;
    }

    Result<detail::RequestKey> accept_size_op(int fd, std::byte* buffer, std::uint64_t offset,
                                              std::uint64_t length, detail::RequestOp core_op,
                                              detail::OperationKind kind,
                                              Completion<std::size_t>* c,
                                              void (*publish_fn)(void*,
                                                                 const detail::PublicationPayload&)
                                                  noexcept) {
        const auto reservation = core_->reserve();
        if (!reservation.ok()) {
            const IoError::Code code =
                reservation.status == detail::ReserveStatus::admission_closed
                    ? IoError::Code::invalid_state
                    : IoError::Code::would_block;
            return make_unexpected<detail::RequestKey>(IoError{code});
        }
        const detail::SlotHandle h{reservation.reservation.slot, reservation.reservation.generation};
        if (!begin_binding(*c)) {
            (void)core_->rollback(reservation.reservation);
            return make_unexpected<detail::RequestKey>(IoError{IoError::Code::invalid_state});
        }
        const detail::RequestDescriptor descriptor{core_op, length == 0 ? 0 : offset, length,
                                                   length == 0};
        const detail::BorrowFacts borrow{fd, buffer, length};
        const auto accepted = core_->accept(reservation.reservation, descriptor, borrow);
        if (!accepted.ok()) {
            (void)core_->rollback(reservation.reservation);
            return make_unexpected<detail::RequestKey>(IoError{IoError::Code::invalid_state});
        }
        const detail::RequestKey key = accepted.id;
        install_core_binding(*c, core_, key);
        commit_binding(*c);
        if (core_->claim_execution(key) != detail::ExecutionClaim::claimed) {
            note_protocol_violation("claim_execution denied for accepted submit");
            return make_unexpected<detail::RequestKey>(IoError{IoError::Code::backend_error});
        }
        record_submit(key, h, kind, fd, buffer, offset, static_cast<std::size_t>(length), c,
                      publish_fn);
        return key;
    }

    Result<detail::RequestKey> accept_void_op(int fd, detail::RequestOp core_op,
                                              detail::OperationKind kind, Completion<void>* c,
                                              void (*publish_fn)(void*,
                                                                 const detail::PublicationPayload&)
                                                  noexcept) {
        const auto reservation = core_->reserve();
        if (!reservation.ok()) {
            const IoError::Code code =
                reservation.status == detail::ReserveStatus::admission_closed
                    ? IoError::Code::invalid_state
                    : IoError::Code::would_block;
            return make_unexpected<detail::RequestKey>(IoError{code});
        }
        const detail::SlotHandle h{reservation.reservation.slot, reservation.reservation.generation};
        if (!begin_binding(*c)) {
            (void)core_->rollback(reservation.reservation);
            return make_unexpected<detail::RequestKey>(IoError{IoError::Code::invalid_state});
        }
        const detail::RequestDescriptor descriptor{core_op, 0, 0, false};
        const detail::BorrowFacts borrow{fd, nullptr, 0};
        const auto accepted = core_->accept(reservation.reservation, descriptor, borrow);
        if (!accepted.ok()) {
            (void)core_->rollback(reservation.reservation);
            return make_unexpected<detail::RequestKey>(IoError{IoError::Code::invalid_state});
        }
        const detail::RequestKey key = accepted.id;
        install_core_binding(*c, core_, key);
        commit_binding(*c);
        if (core_->claim_execution(key) != detail::ExecutionClaim::claimed) {
            note_protocol_violation("claim_execution denied for accepted submit");
            return make_unexpected<detail::RequestKey>(IoError{IoError::Code::backend_error});
        }
        record_submit(key, h, kind, fd, nullptr, 0, 0, c, publish_fn);
        return key;
    }

    void record_submit(detail::RequestKey key, detail::SlotHandle h, detail::OperationKind kind,
                       int fd, std::byte* buffer, std::uint64_t offset, std::size_t length,
                       void* completion,
                       void (*publish_fn)(void*, const detail::PublicationPayload&) noexcept) {
        (void)fd;
        {
            std::lock_guard<std::mutex> lk(shared_->mtx);
            for (const auto& [slot, r] : shared_->records) {
                if (!ScriptedShared::borrow_is_live(r) || r.len == 0 || length == 0 ||
                    r.buffer == nullptr || buffer == nullptr)
                    continue;
                if (address_intervals_overlap(r.buffer, r.len, buffer, length))
                    shared_->borrow_overlap_violation = true;
            }
            ScriptedShared::Record& rec = shared_->records[h.slot.value];
            rec.submit_id = ++shared_->next_submit_id;
            rec.key = key;
            rec.kind = kind;
            rec.buffer = buffer;
            rec.offset = offset;
            rec.len = length;
            rec.completion = completion;
            rec.publish = publish_fn;
            rec.stage = ScriptedShared::Record::Stage::accepted;
            rec.borrow_released = false;
            ++shared_->accepted_count[ScriptedShared::kind_index(kind)];
            shared_->recompute_peak_locked(kind);
            shared_->recompute_borrow_peak_locked(kind);
        }
        shared_->cv.notify_all();
    }

    void publish_one(detail::RequestKey key) {
        detail::PublicationPayload payload;
        if (core_->begin_publication(key, &payload) != detail::PublicationGrant::granted) {
            note_protocol_violation("begin_publication denied");
            return;
        }
        detail::OperationKind kind = kind_of(payload.op);
        {
            std::lock_guard<std::mutex> lk(shared_->mtx);
            auto it = shared_->records.find(key.slot.value);
            if (it != shared_->records.end() &&
                it->second.key.generation.value == key.generation.value &&
                it->second.stage == ScriptedShared::Record::Stage::staged) {
                kind = it->second.kind;
                it->second.publish(it->second.completion, payload);
                it->second.stage = ScriptedShared::Record::Stage::published;
                shared_->recompute_peak_locked(kind);
            }
        }
        if (core_->complete_publication(key) != detail::PublicationCompletion::completed) {
            note_protocol_violation("complete_publication denied");
            return;
        }
        (void)core_->claim_observer_delivery(key);
        (routing_sink_ ? *routing_sink_ : sink_).on_ready(detail::ReadyEvent{key, kind});
    }

    void note_protocol_violation(const char* what) {
        std::lock_guard<std::mutex> lk(shared_->mtx);
        if (shared_->protocol_violation.empty())
            shared_->protocol_violation = what;
    }

    std::shared_ptr<ScriptedShared> shared_;
    detail::ReferenceReadySink sink_;
    std::size_t slots_;
};

class ScriptedController {
  public:
    ScriptedController() = default;
    explicit ScriptedController(std::shared_ptr<ScriptedShared> shared)
        : shared_(std::move(shared)) {}

    std::vector<ScriptedOpView> completable_ops() const {
        std::lock_guard<std::mutex> lk(shared_->mtx);
        std::vector<ScriptedOpView> out;
        for (const auto& [slot, r] : shared_->records) {
            if (r.stage != ScriptedShared::Record::Stage::accepted)
                continue;
            out.push_back(
                ScriptedOpView{r.submit_id, r.kind, r.offset, r.len, r.completion, r.buffer});
        }
        return out;
    }

    std::size_t completable_count(detail::OperationKind kind) const {
        std::lock_guard<std::mutex> lk(shared_->mtx);
        return shared_->completable_count_locked(kind);
    }

    // Snapshot of a submitted op regardless of stage; the record for a slot is
    // overwritten on reuse, so capture before completing the op in question.
    std::optional<ScriptedOpView> op_view(std::uint64_t submit_id) const {
        std::lock_guard<std::mutex> lk(shared_->mtx);
        for (const auto& [slot, r] : shared_->records)
            if (r.submit_id == submit_id)
                return ScriptedOpView{r.submit_id, r.kind, r.offset, r.len, r.completion,
                                      r.buffer};
        return std::nullopt;
    }

    bool wait_completable(detail::OperationKind kind, std::size_t min_count,
                          std::chrono::milliseconds timeout = kWaitFor) const {
        std::unique_lock<std::mutex> lk(shared_->mtx);
        return shared_->cv.wait_until(lk, std::chrono::steady_clock::now() + timeout, [&] {
            return shared_->closed || shared_->completable_count_locked(kind) >= min_count;
        }) && !shared_->closed;
    }

    std::uint64_t wait_completable_at(detail::OperationKind kind, std::uint64_t offset,
                                      std::chrono::milliseconds timeout = kWaitFor) const {
        std::unique_lock<std::mutex> lk(shared_->mtx);
        if (!shared_->cv.wait_until(lk, std::chrono::steady_clock::now() + timeout, [&] {
                if (shared_->closed)
                    return true;
                for (const auto& [slot, r] : shared_->records)
                    if (r.stage == ScriptedShared::Record::Stage::accepted && r.kind == kind &&
                        r.offset == offset)
                        return true;
                return false;
            }))
            return 0;
        if (shared_->closed)
            return 0;
        for (const auto& [slot, r] : shared_->records)
            if (r.stage == ScriptedShared::Record::Stage::accepted && r.kind == kind &&
                r.offset == offset)
                return r.submit_id;
        return 0;
    }

    bool complete_bytes(std::uint64_t submit_id, std::size_t n) {
        detail::RequestKey key{};
        detail::OperationKind kind = detail::OperationKind::read;
        std::byte* buffer = nullptr;
        std::uint64_t offset = 0;
        if (!mark_staged(submit_id, key, kind, buffer, offset))
            return false;
        apply_byte_effect(kind, buffer, offset, n);
        return stage_on_backend(key, sluice::detail::IoOutcome::success(n));
    }

    // complete_bytes split in two: hold_staged moves an accepted op into
    // staged without offering the terminal, leaving its borrow live; a held op
    // that is never released stalls its scenario until the watchdog fires.
    std::optional<ScriptedOpView> hold_staged(std::uint64_t submit_id) {
        detail::RequestKey key{};
        detail::OperationKind kind = detail::OperationKind::read;
        std::byte* buffer = nullptr;
        std::uint64_t offset = 0;
        if (!mark_staged(submit_id, key, kind, buffer, offset))
            return std::nullopt;
        held_[submit_id] = HeldStaged{key, kind, buffer, offset};
        return ScriptedOpView{submit_id, kind, offset, 0, nullptr, buffer};
    }

    bool release_held(std::uint64_t submit_id, std::size_t n) {
        const auto it = held_.find(submit_id);
        if (it == held_.end())
            return false;
        apply_byte_effect(it->second.kind, it->second.buffer, it->second.offset, n);
        const bool ok = stage_on_backend(it->second.key, sluice::detail::IoOutcome::success(n));
        held_.erase(it);
        return ok;
    }

    bool complete_eof(std::uint64_t submit_id) { return complete_bytes(submit_id, 0); }

    bool complete_sync(std::uint64_t submit_id) {
        detail::RequestKey key{};
        detail::OperationKind kind = detail::OperationKind::sync_data;
        std::byte* buffer = nullptr;
        std::uint64_t offset = 0;
        if (!mark_staged(submit_id, key, kind, buffer, offset))
            return false;
        return stage_on_backend(key, sluice::detail::IoOutcome::success(0));
    }

    bool complete_error(std::uint64_t submit_id, IoError error) {
        detail::RequestKey key{};
        detail::OperationKind kind = detail::OperationKind::read;
        std::byte* buffer = nullptr;
        std::uint64_t offset = 0;
        if (!mark_staged(submit_id, key, kind, buffer, offset))
            return false;
        return stage_on_backend(key, sluice::detail::IoOutcome::failure(error));
    }

    void fail_next_submit(detail::OperationKind kind, IoError error) {
        std::lock_guard<std::mutex> lk(shared_->mtx);
        shared_->fail_next[ScriptedShared::kind_index(kind)] = error;
    }

    // True when no accepted op remains. Bounded: a pipeline defect that keeps
    // accepting new work forever cannot spin here past the budget; the caller's
    // watchdog then reports the stall.
    bool settle_all_remaining(std::chrono::milliseconds budget = kDriveBound) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        for (;;) {
            const std::vector<ScriptedOpView> ops = completable_ops();
            if (ops.empty())
                return true;
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            for (const ScriptedOpView& op : ops) {
                switch (op.kind) {
                case detail::OperationKind::read:
                    (void)complete_eof(op.submit_id);
                    break;
                case detail::OperationKind::write:
                    (void)complete_bytes(op.submit_id, op.len);
                    break;
                default:
                    (void)complete_sync(op.submit_id);
                    break;
                }
            }
        }
    }

    // Drives the copy to completion: full reads below total, EOF reads at or
    // past total, full writes, successful syncs. Bounded by kDriveBound; a
    // copy that stops making progress returns and the caller's watchdog
    // reports it.
    void drive_to_end(std::uint64_t total, const std::atomic<bool>& published) {
        const auto deadline = std::chrono::steady_clock::now() + kDriveBound;
        while (!published.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            bool staged_any = false;
            for (const ScriptedOpView& op : completable_ops()) {
                switch (op.kind) {
                case detail::OperationKind::read:
                    staged_any = (op.offset >= total ? complete_eof(op.submit_id)
                                                     : complete_bytes(op.submit_id, op.len)) ||
                                 staged_any;
                    break;
                case detail::OperationKind::write:
                    staged_any = complete_bytes(op.submit_id, op.len) || staged_any;
                    break;
                default:
                    staged_any = complete_sync(op.submit_id) || staged_any;
                    break;
                }
            }
            if (!staged_any)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    bool pending_empty() const {
        std::lock_guard<std::mutex> lk(shared_->mtx);
        if (!shared_->publication_queue.empty())
            return false;
        for (const auto& [slot, r] : shared_->records)
            if (r.stage != ScriptedShared::Record::Stage::published)
                return false;
        return true;
    }

    bool borrow_overlap_violation() const {
        std::lock_guard<std::mutex> lk(shared_->mtx);
        return shared_->borrow_overlap_violation;
    }

    std::string protocol_violation() const {
        std::lock_guard<std::mutex> lk(shared_->mtx);
        return shared_->protocol_violation;
    }

    std::size_t accepted(detail::OperationKind kind) const {
        std::lock_guard<std::mutex> lk(shared_->mtx);
        return shared_->accepted_count[ScriptedShared::kind_index(kind)];
    }

    std::size_t peak_live(detail::OperationKind kind) const {
        std::lock_guard<std::mutex> lk(shared_->mtx);
        return shared_->peak_live[kind_index(kind)];
    }

    // High-water count of ops of `kind` simultaneously inside the
    // backend-physical borrow window (accept .. release_execution). Distinct
    // from peak_live (accept .. publication) and from completable_count
    // (accepted only): a staged-but-unreleased op still holds its borrow.
    std::size_t peak_borrow_live(detail::OperationKind kind) const {
        std::lock_guard<std::mutex> lk(shared_->mtx);
        return shared_->peak_borrow_live[kind_index(kind)];
    }

    std::size_t borrow_live_count(detail::OperationKind kind) const {
        std::lock_guard<std::mutex> lk(shared_->mtx);
        return shared_->borrow_live_count_locked(kind);
    }

    bool shadow_matches(std::uint64_t total) const {
        std::lock_guard<std::mutex> lk(shared_->mtx);
        if (shared_->shadow.size() != total)
            return false;
        for (const auto& [off, b] : shared_->shadow)
            if (b != seed_byte(off))
                return false;
        return true;
    }

    std::string diagnostics() const {
        std::lock_guard<std::mutex> lk(shared_->mtx);
        std::string out;
        for (const auto& [slot, r] : shared_->records) {
            out += "  op id=" + std::to_string(r.submit_id) + " slot=" + std::to_string(slot) +
                   " kind=" + std::to_string(static_cast<int>(r.kind)) +
                   " stage=" + std::to_string(static_cast<int>(r.stage)) +
                   " off=" + std::to_string(r.offset) + " len=" + std::to_string(r.len) + "\n";
        }
        out += "  publication_queue=" + std::to_string(shared_->publication_queue.size()) +
               " closed=" + (shared_->closed ? "1" : "0") +
               " borrow_overlap=" + (shared_->borrow_overlap_violation ? "1" : "0") +
               " protocol_violation=" + shared_->protocol_violation + "\n";
        return out;
    }

  private:
    static std::size_t kind_index(detail::OperationKind k) {
        return static_cast<std::size_t>(k);
    }

    struct HeldStaged {
        detail::RequestKey key{};
        detail::OperationKind kind = detail::OperationKind::read;
        std::byte* buffer = nullptr;
        std::uint64_t offset = 0;
    };
    std::map<std::uint64_t, HeldStaged> held_;

    bool mark_staged(std::uint64_t submit_id, detail::RequestKey& key,
                     detail::OperationKind& kind, std::byte*& buffer, std::uint64_t& offset) {
        std::lock_guard<std::mutex> lk(shared_->mtx);
        for (auto& [slot, r] : shared_->records) {
            if (r.submit_id == submit_id && r.stage == ScriptedShared::Record::Stage::accepted) {
                key = r.key;
                kind = r.kind;
                buffer = r.buffer;
                offset = r.offset;
                r.stage = ScriptedShared::Record::Stage::staged;
                shared_->recompute_peak_locked(r.kind);
                shared_->cv.notify_all();
                return true;
            }
        }
        return false;
    }

    void apply_byte_effect(detail::OperationKind kind, std::byte* buffer, std::uint64_t offset,
                           std::size_t n) {
        if (kind == detail::OperationKind::read) {
            for (std::size_t i = 0; i < n; ++i)
                buffer[i] = seed_byte(offset + i);
        } else if (kind == detail::OperationKind::write) {
            std::lock_guard<std::mutex> lk(shared_->mtx);
            for (std::size_t i = 0; i < n; ++i)
                shared_->shadow[offset + i] = buffer[i];
        }
    }

    bool stage_on_backend(detail::RequestKey key, sluice::detail::IoOutcome outcome) {
        std::lock_guard<std::mutex> lk(shared_->backend_gate);
        if (shared_->backend_raw == nullptr)
            return false;
        shared_->backend_raw->stage_outcome(key, outcome);
        return true;
    }

    std::shared_ptr<ScriptedShared> shared_;
};

class ThrowingBackend final : public AsyncBackend {
  public:
    bool supports_request_identity() const noexcept override { return true; }
    bool signals_physical_progress() const noexcept override { return true; }
    std::size_t poll() override { return 0; }
    std::size_t outstanding() const noexcept override { return 0; }
    bool internal_work_retired() const noexcept override { return true; }
    std::size_t slot_capacity() const noexcept override { return 1; }
    detail::PublicCancel cancel_identity(detail::RequestKey) override {
        return detail::PublicCancel::not_found;
    }

  private:
    Result<detail::RequestKey> submit_read(ReadOp, Completion<std::size_t>*) override {
        throw std::runtime_error("injected submit failure");
    }
    Result<detail::RequestKey> submit_write(WriteOp, Completion<std::size_t>*) override {
        throw std::runtime_error("injected submit failure");
    }
    Result<detail::RequestKey> submit_sync_data(SyncDataOp, Completion<void>*) override {
        throw std::runtime_error("injected submit failure");
    }
    Result<detail::RequestKey> submit_sync_all(SyncAllOp, Completion<void>*) override {
        throw std::runtime_error("injected submit failure");
    }
    Result<detail::RequestKey> submit_file_info(FileInfoOp, Completion<FileInfo>*) override {
        throw std::runtime_error("injected submit failure");
    }
    Result<detail::RequestKey> submit_size(SizeOp, Completion<FileSize>*) override {
        throw std::runtime_error("injected submit failure");
    }
};

struct TempFile {
    std::string path;
    explicit TempFile(const char* tmpl) {
        std::vector<char> buf(tmpl, tmpl + std::strlen(tmpl) + 1);
        const int fd = ::mkstemp(buf.data());
        if (fd >= 0) {
            ::close(fd);
            path = buf.data();
        }
    }
};

struct CopyScenario {
    std::optional<ScriptedController> ctrl;
    std::thread copy_thread;
    std::optional<Result<CopyStats>> copy_result;
    std::atomic<bool> copy_published{false};
    std::optional<File> src;
    std::optional<File> dst;
    std::string src_path;
    std::string dst_path;
    bool joined = false;

    CopyScenario() : ctrl(std::nullopt) {}

    ~CopyScenario() { drain_and_join(); }

    bool open_files() {
        src_path = TempFile("/tmp/sluice_copy_oracle_src_XXXXXX").path;
        dst_path = TempFile("/tmp/sluice_copy_oracle_dst_XXXXXX").path;
        if (src_path.empty() || dst_path.empty())
            return false;
        auto src_open = File::open(src_path);
        FileOpen write_mode;
        write_mode.access = FileAccess::write_only;
        auto dst_open = File::open(dst_path, write_mode);
        if (!src_open.has_value() || !dst_open.has_value())
            return false;
        src = std::move(src_open).value();
        dst = std::move(dst_open).value();
        return true;
    }

    void start(std::size_t buffer_size, std::size_t depth, SyncPolicy sync, unsigned workers = 1,
               std::size_t slots = kDefaultSlots) {
        if (!src.has_value() || !dst.has_value()) {
            std::fprintf(stderr, "oracle: start() before successful open_files()\n");
            std::fflush(stderr);
            std::abort();
        }
        auto shared = std::make_shared<ScriptedShared>();
        ctrl = ScriptedController(shared);
        copy_thread = std::thread([this, buffer_size, depth, sync, workers, slots, shared]() {
            auto backend = std::make_unique<ScriptedBackend>(shared, slots);
            auto r = run_pipelined_copy_with_backend(
                *src, NativeFileRef{*dst}, buffer_size, depth, workers, sync,
                std::move(backend));
            copy_result = std::move(r);
            copy_published.store(true, std::memory_order_release);
        });
    }

    bool published() const { return copy_published.load(std::memory_order_acquire); }

    void drain_and_join() { drain_and_join_for(std::chrono::seconds(15)); }

    void drain_and_join_for(std::chrono::milliseconds deadline) {
        if (joined)
            return;
        const auto dl = std::chrono::steady_clock::now() + deadline;
        while (!published() && std::chrono::steady_clock::now() < dl) {
            if (ctrl.has_value()) {
                const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                    dl - std::chrono::steady_clock::now());
                (void)ctrl->settle_all_remaining(remain);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (ctrl.has_value()) {
            const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                dl - std::chrono::steady_clock::now());
            (void)ctrl->settle_all_remaining(std::max(remain, std::chrono::milliseconds(0)));
        }
        if (published()) {
            if (copy_thread.joinable())
                copy_thread.join();
            joined = true;
            return;
        }
        watchdog_abort(deadline);
    }

    [[noreturn]] void watchdog_abort(std::chrono::milliseconds deadline) {
        std::fprintf(stderr, "\n[watchdog] copy thread did not publish within %lld ms\n",
                     static_cast<long long>(deadline.count()));
        if (ctrl.has_value())
            std::fputs(ctrl->diagnostics().c_str(), stderr);
        for (const std::string& f : g_failures)
            std::fprintf(stderr, "[watchdog]   failure: %s\n", f.c_str());
        std::fflush(stderr);
        std::abort();
    }
};

}  // namespace sluice_copy_oracle
