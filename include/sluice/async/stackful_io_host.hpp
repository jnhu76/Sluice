#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/cancel.hpp>
#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/blocking/file.hpp>
#include <sluice/error.hpp>
#include <sluice/result.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace sluice::async {

struct StackfulHostConfig {
    std::size_t task_capacity = 8;
    std::size_t stack_bytes = 64 * 1024;
};

class StackfulIoHost;

// Handle passed to a task body. The token is the host-wide stop token,
// shared by every task of the host: tasks observe it but cannot un-latch a
// host stop (the returned reference is read-only). The context is valid only
// inside the task body invocation that received it; tasks must not retain it
// past return.
class IoTaskContext {
  public:
    const CancelToken& token() const noexcept { return *token_; }

    Result<std::size_t> read(NativeFileRef file, std::span<std::byte> dst, std::uint64_t offset);
    Result<std::size_t> write(NativeFileRef file, std::span<const std::byte> src,
                              std::uint64_t offset);
    Result<void> sync_data(NativeFileRef file);
    Result<void> sync_all(NativeFileRef file);

    Result<std::size_t> read_for(NativeFileRef file, std::span<std::byte> dst,
                                 std::uint64_t offset, std::chrono::nanoseconds wait);
    Result<std::size_t> write_for(NativeFileRef file, std::span<const std::byte> src,
                                  std::uint64_t offset, std::chrono::nanoseconds wait);
    Result<void> sync_data_for(NativeFileRef file, std::chrono::nanoseconds wait);
    Result<void> sync_all_for(NativeFileRef file, std::chrono::nanoseconds wait);

    // Exact/all composition over the primitive helpers above: repeated
    // primitive calls advanced by confirmed bytes (never replayed). The
    // invocation is validated first, like the direct forms: a closed file,
    // illegal access, an invalid range or an already-stopped host rejects
    // the whole call through the outer result, before anything is accepted.
    // A zero-length invocation is a logical no-op that still crosses
    // admission as one no-op request — inheriting the context's admission
    // and slot rules — and completes without a data operation. Once
    // composing, a primitive failure stops the loop and is reported in the
    // outcome with the confirmed prefix retained, next to a structured stop
    // reason (complete, EOF before full, write made no progress, primitive
    // error); a host stop observed mid-composition rejects the next
    // primitive boundary with `canceled` before acceptance, reported like
    // any primitive error.
    Result<blocking::CompositionOutcome> read_exact(NativeFileRef file, std::span<std::byte> dst,
                                                    std::uint64_t offset);
    Result<blocking::CompositionOutcome> write_all(NativeFileRef file,
                                                   std::span<const std::byte> src,
                                                   std::uint64_t offset);

  private:
    friend class StackfulIoHost;

    IoTaskContext(StackfulIoHost& host, CancelToken& token) noexcept
        : host_(&host), token_(&token) {}

    StackfulIoHost* host_;
    CancelToken* token_;
};

// Optional stackful File-I/O host adapter over an application-owned
// AsyncIoContext. `create` fails with `not_supported` unless the context's
// backend can drive the split wait protocol.
//
// `spawn` and `run` must be called from the host thread (the thread that
// calls `run`); only `request_stop` is safe from any thread. This is a
// caller obligation, not a policed precondition.
//
// `run` claims the context's progress owner for the whole call, drives every
// admitted task to retirement, and releases the owner on every exit; it
// returns the first task error of that call, if any. Blocking file management
// (open, explicit close, resize) is outside the task region: perform it on
// the host thread outside `run`.
//
// `request_stop()` publishes the stop flag, requests the task stop token and
// closes spawn admission. It neither cancels nor settles accepted requests
// and it touches nothing in the context's control plane: an accepted request
// keeps its responsibility until its natural publication, and stop
// convergence waits on that settlement.
//
// Admission linearizes at `spawn`'s acquire-load of the stop flag: a spawn
// that observes stop=false may complete its slot commitment even if
// `request_stop()` stores stop=true immediately afterwards — that admitted
// task is part of the finite host set and is driven to retirement normally.
// A spawn that observes stop=true admits no task.
//
// The `_for` forms bound only the driver's initial park window for that
// await: on expiry the request keeps its responsibility — the host neither
// cancels nor settles it — and the helper returns the operation's real
// terminal result whenever it reaches one. A non-positive wait provides no
// effective bound. Destroying the host while tasks are spawned but not
// retired fails fast.
class StackfulIoHost {
  public:
    static Result<std::unique_ptr<StackfulIoHost>> create(AsyncIoContext& ctx,
                                                          StackfulHostConfig config);

    ~StackfulIoHost();

    StackfulIoHost(const StackfulIoHost&) = delete;
    StackfulIoHost& operator=(const StackfulIoHost&) = delete;
    StackfulIoHost(StackfulIoHost&&) = delete;
    StackfulIoHost& operator=(StackfulIoHost&&) = delete;

    std::size_t task_capacity() const noexcept { return task_capacity_; }

    bool stop_requested() const noexcept { return stop_requested_.load(std::memory_order_acquire); }

    Result<void> spawn(std::function<void(IoTaskContext&)> task);

    void request_stop() noexcept;

    Result<void> run();

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    std::size_t test_live_task_count() const noexcept { return live_tasks_; }

    std::size_t test_ready_ring_size() const noexcept { return ring_size_; }

    std::size_t test_slots_with_await_link() const noexcept {
        std::size_t with_link = 0;
        for (std::size_t i = 0; i < task_capacity_; ++i) {
            if (slots_[i].await_link != nullptr) {
                ++with_link;
            }
        }
        return with_link;
    }
#endif

  private:
    friend class IoTaskContext;

    struct AwaitLink {
        bool (*ready_fn)(void* request) noexcept = nullptr;
#if defined(SLUICE_STACKFUL_HOST_MUTANT_DEADLINE_CANCELS) ||                                  \
    defined(SLUICE_STACKFUL_HOST_MUTANT_STOP_IMPLICIT_CANCEL)
        void (*cancel_fn)(void* request) noexcept = nullptr;
#endif
        void* request = nullptr;
        std::chrono::steady_clock::time_point deadline{};
        bool has_deadline = false;
    };

    struct TaskSlot {
        StackfulIoHost* host = nullptr;
        Fiber fiber;
        std::unique_ptr<std::byte[]> stack;
        std::function<void(IoTaskContext&)> entry;
        AwaitLink* await_link = nullptr;
        bool live = false;
    };

    static constexpr std::size_t kMinStackBytes = 64 * 1024;

    static Result<std::unique_ptr<StackfulIoHost>> create_validated_(AsyncIoContext& ctx,
                                                                     StackfulHostConfig config);

    explicit StackfulIoHost(AsyncIoContext& ctx, std::size_t task_capacity);

    bool prepare_slot_fiber_(TaskSlot& slot) noexcept;

    static void task_entry_bridge_(fiber_ctx::Switch* resumed_by, void* userdata);

    void run_task_(TaskSlot& slot);
    void suspend_current_(AwaitLink& link);
    void wake_suspended_ready_();
    std::chrono::nanoseconds next_park_() const noexcept;
    void record_task_error_(const IoError& error) noexcept;

    template <class T, class Submit>
    static Result<T> await_request_(StackfulIoHost& host, Submit&& submit, bool bounded,
                                    std::chrono::nanoseconds wait);

    AsyncIoContext& ctx_;
    std::size_t task_capacity_ = 0;
    std::size_t stack_bytes_ = 0;
    std::unique_ptr<TaskSlot[]> slots_;
    std::size_t live_tasks_ = 0;

    std::unique_ptr<std::size_t[]> ready_ring_;
    std::size_t ring_head_ = 0;
    std::size_t ring_size_ = 0;

    fiber_ctx::Context driver_ctx_;

    CancelToken stop_token_;
    std::atomic<bool> stop_requested_{false};
    bool first_task_error_set_ = false;
    IoError first_task_error_{};

    static thread_local StackfulIoHost* running_host_;
    static thread_local TaskSlot* running_slot_;
};

template <class T, class Submit>
Result<T> StackfulIoHost::await_request_(StackfulIoHost& host, Submit&& submit, bool bounded,
                                         std::chrono::nanoseconds wait) {
    static_assert(std::is_nothrow_move_constructible_v<Request<T>>);
#if !defined(SLUICE_STACKFUL_HOST_MUTANT_AWAIT_STOP_RETURNS_UNSETTLED)
    if (host.stop_requested()) {
        return make_unexpected<T>(IoError{IoError::Code::canceled});
    }
#endif
    auto submitted = submit();
    if (!submitted.has_value()) {
        return make_unexpected<T>(submitted.error());
    }
    Request<T> request = std::move(submitted.value());
#if defined(SLUICE_STACKFUL_HOST_MUTANT_AWAIT_STOP_RETURNS_UNSETTLED)
    if (host.stop_requested()) {
        return make_unexpected<T>(IoError{IoError::Code::canceled});
    }
#endif
    AwaitLink link;
    link.request = &request;
    link.ready_fn = [](void* p) noexcept { return static_cast<Request<T>*>(p)->ready(); };
#if defined(SLUICE_STACKFUL_HOST_MUTANT_DEADLINE_CANCELS) ||                                  \
    defined(SLUICE_STACKFUL_HOST_MUTANT_STOP_IMPLICIT_CANCEL)
    link.cancel_fn = [](void* p) noexcept { (void)static_cast<Request<T>*>(p)->cancel(); };
#endif
    if (bounded) {
        const auto now = std::chrono::steady_clock::now();
        link.deadline = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::min(wait, std::chrono::steady_clock::time_point::max() -
                                                     now));
        link.has_deadline = true;
    }
    host.suspend_current_(link);
    const RequestObservation<T> observed = request.take_result();
    if (observed.readiness != RequestReadiness::ready) {
        detail::stackful_host_suspend_invariant_fail_fast();
    }
    return observed.result;
}

} // namespace sluice::async
