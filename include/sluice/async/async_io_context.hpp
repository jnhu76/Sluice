#pragma once

#include <sluice/async/completion.hpp>
#include <sluice/async/detail/ready_sink.hpp>
#include <sluice/async/request_handle.hpp>
#include <sluice/error.hpp>
#include <sluice/file_resource.hpp>
#include <sluice/measurement.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace sluice::async {

// Mechanism-level reference to the resource an explicit operation targets.
// Implicit conversion from a canonical File carries the File's access
// contract, so every initiation surface can enforce the ADR-0002 §5.5
// access-legality matrix before admission. Construction from a raw native
// handle is the explicit interop boundary: the caller declares the access as
// a claim, and the interop path does not claim canonical File access
// validation. Carries no ownership and no lifetime authority: the handle
// value is copied at op construction and the caller keeps the resource alive
// until terminal completion is observed.
struct NativeFileRef {
    NativeFileRef() = default;
    NativeFileRef(const sluice::File& file) : fd(file.native_handle()), access(file.access()) {}
    NativeFileRef(int native_fd, FileAccess declared_access)
        : fd(native_fd), access(declared_access) {}

    int fd = -1;
    FileAccess access = FileAccess::read_write;
};

struct ReadOp {
    NativeFileRef file;
    std::byte* dst = nullptr;
    std::size_t len = 0;
    std::uint64_t offset = 0;
};
struct WriteOp {
    NativeFileRef file;
    const std::byte* src = nullptr;
    std::size_t len = 0;
    std::uint64_t offset = 0;
};
struct SyncDataOp {
    NativeFileRef file;
};
struct SyncAllOp {
    NativeFileRef file;
};

struct BackendWaitToken {
    std::uint64_t progress_generation = 0;
    std::uint64_t control_generation = 0;
};

enum class BackendWakeReason {
    progress,
    interrupted,
};

class BackendWaitSource {
  public:
    BackendWaitSource() = default;
    virtual ~BackendWaitSource() = default;
    BackendWaitSource(const BackendWaitSource&) = delete;
    BackendWaitSource& operator=(const BackendWaitSource&) = delete;

    virtual BackendWaitToken snapshot() const noexcept = 0;

    virtual BackendWakeReason wait_for_change(BackendWaitToken observed) noexcept = 0;

    virtual BackendWakeReason wait_for_change(BackendWaitToken observed,
                                              std::chrono::nanoseconds max_park) noexcept {
        (void)max_park;
        return wait_for_change(observed);
    }

    virtual bool supports_bounded_wait() const noexcept { return false; }

    virtual void interrupt_all() noexcept = 0;

    virtual BackendWaitToken arm_committed_wait() noexcept { return snapshot(); }

    virtual BackendWaitToken consume_committed_wait() noexcept { return snapshot(); }
};

class AsyncBackend {
  public:
    virtual ~AsyncBackend() = default;
    AsyncBackend(const AsyncBackend&) = delete;
    AsyncBackend& operator=(const AsyncBackend&) = delete;

    void attach_stats(AsyncStats* s) { stats_ = s; }

    void attach_ready_sink(detail::SynchronousReadySink* sink) noexcept { routing_sink_ = sink; }

    virtual Result<void> register_waiter(Completion<std::size_t>& c, detail::WaiterToken token,
                                         detail::RoutingLease lease) {
        (void)c;
        (void)token;
        (void)lease;
        return make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
    virtual Result<void> register_waiter(Completion<void>& c, detail::WaiterToken token,
                                         detail::RoutingLease lease) {
        (void)c;
        (void)token;
        (void)lease;
        return make_unexpected<void>(IoError{IoError::Code::not_supported});
    }
    virtual Result<detail::RoutingLease> cancel_waiter(Completion<std::size_t>& c) {
        (void)c;
        return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_supported});
    }
    virtual Result<detail::RoutingLease> cancel_waiter(Completion<void>& c) {
        (void)c;
        return make_unexpected<detail::RoutingLease>(IoError{IoError::Code::not_supported});
    }

    virtual std::size_t poll() = 0;

    virtual Result<std::size_t> wait_one() = 0;

    virtual void cancel(Completion<std::size_t>& c) { (void)c; }
    virtual void cancel(Completion<void>& c) { (void)c; }

    virtual std::size_t outstanding() const noexcept = 0;

    virtual BackendWaitSource* wait_source() noexcept { return nullptr; }

    virtual bool wait_one_is_nonblocking() const noexcept { return false; }

    virtual bool supports_request_identity() const noexcept { return false; }

  private:
    friend class AsyncIoContext;

    // The access-legality matrix (ADR-0002 §5.5) is enforced by
    // AsyncIoContext before these are reachable; the backend only lowers fd.
    virtual Result<void> submit_read(ReadOp op, Completion<std::size_t>& c) = 0;
    virtual Result<void> submit_write(WriteOp op, Completion<std::size_t>& c) = 0;
    virtual Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c) = 0;
    virtual Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c) = 0;

    virtual Result<RequestHandleState> resolve_identity_state(std::uint64_t context,
                                                              std::uint32_t slot,
                                                              std::uint64_t generation) const {
        (void)context;
        (void)slot;
        (void)generation;
        return make_unexpected<RequestHandleState>(IoError{IoError::Code::not_supported});
    }

    RequestHandle identity_of(Completion<std::size_t>& c) const noexcept;
    RequestHandle identity_of(Completion<void>& c) const noexcept;

    Result<RequestHandleState> request_handle_state(const RequestHandle& h) const noexcept;

  protected:
    AsyncBackend() = default;
    AsyncStats* stats_ = nullptr;

    detail::SynchronousReadySink* routing_sink_ = nullptr;

    template <class T> static bool try_claim(Completion<T>& c) noexcept {
        return c.try_claim_for_backend();
    }

    template <class T> static bool begin_binding(Completion<T>& c) noexcept {
        return c.begin_binding_for_backend();
    }
    template <class T> static void commit_binding(Completion<T>& c) noexcept {
        c.commit_binding_to_outstanding();
    }
    template <class T> static void rollback_binding_before_accept(Completion<T>& c) noexcept {
        c.rollback_binding_before_accept();
    }

    template <class T>
    static void install_binding(Completion<T>& c, detail::RequestArena* arena,
                                detail::SlotHandle h) noexcept {
        c.install_binding_for_backend(arena, h);
    }
    template <class T> static void clear_binding(Completion<T>& c) noexcept {
        c.clear_binding_for_backend();
    }

    template <class T> static void publish(Completion<T>& c, Result<T>&& result) noexcept {
        c.publish_from_reap(std::move(result));
    }

    template <class T> static void rollback_claim_before_accept(Completion<T>& c) noexcept {
        c.rollback_claim_before_accept();
    }
};

class AsyncIoContext {
  public:
    explicit AsyncIoContext(std::unique_ptr<AsyncBackend> backend, AsyncStats* stats = nullptr);
    ~AsyncIoContext();

    AsyncIoContext(const AsyncIoContext&) = delete;
    AsyncIoContext& operator=(const AsyncIoContext&) = delete;

    AsyncIoContext(AsyncIoContext&&) noexcept;

    AsyncIoContext& operator=(AsyncIoContext&&) noexcept;

    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c);
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c);
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c);
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c);

    Result<RequestHandle> submit_read_request(ReadOp op, Completion<std::size_t>& c);
    Result<RequestHandle> submit_write_request(WriteOp op, Completion<std::size_t>& c);
    Result<RequestHandle> submit_sync_data_request(SyncDataOp op, Completion<void>& c);
    Result<RequestHandle> submit_sync_all_request(SyncAllOp op, Completion<void>& c);

    Result<RequestHandleState> request_state(const RequestHandle& h) const;

    std::size_t poll();

    Result<std::size_t> wait_one();

    Result<std::size_t> wait_one(std::chrono::nanoseconds max_park);

    bool has_split_wait_capability() const noexcept;

    bool has_bounded_split_wait_capability() const noexcept;

    void interrupt_backend_waiters() noexcept;

    void arm_backend_wait_commit() noexcept;

    void cancel(Completion<std::size_t>& c);
    void cancel(Completion<void>& c);

    void set_ready_sink(detail::SynchronousReadySink* sink);

    Result<void> register_waiter(Completion<std::size_t>& c, detail::WaiterToken token,
                                 detail::RoutingLease lease);
    Result<void> register_waiter(Completion<void>& c, detail::WaiterToken token,
                                 detail::RoutingLease lease);
    Result<detail::RoutingLease> cancel_waiter(Completion<std::size_t>& c);
    Result<detail::RoutingLease> cancel_waiter(Completion<void>& c);

    std::size_t outstanding() const noexcept;
    const AsyncStats* stats() const noexcept { return stats_; }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    BackendWaitToken backend_wait_token_for_test() const noexcept;

    struct WaitSourceProgressPauseGate;
    void set_wait_source_progress_pause_gate_for_test(WaitSourceProgressPauseGate* gate) noexcept;
    static void
    resume_wait_source_progress_gate_for_test(WaitSourceProgressPauseGate& gate) noexcept;
#endif

  private:
    std::unique_ptr<AsyncBackend> backend_;
    AsyncStats* stats_;

    mutable std::mutex access_mtx_;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    std::atomic<WaitSourceProgressPauseGate*> wait_source_progress_gate_{nullptr};
    void pause_after_wait_source_progress_() noexcept;
#endif
};

} // namespace sluice::async

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include "async_io_context_test_seams.hpp"
#endif
