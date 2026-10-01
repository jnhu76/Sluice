#pragma once

#include <sluice/async/completion.hpp>
#include <sluice/async/detail/observer_protocol.hpp>
#include <sluice/async/detail/progress_source.hpp>
#include <sluice/async/detail/ready_sink.hpp>
#include <sluice/async/detail/request_key.hpp>
#include <sluice/async/request.hpp>
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
#include <optional>
#include <stdexcept>
#include <thread>

namespace sluice::async {

namespace detail {
class RequestCore;
}
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

class AsyncBackend {
  public:
    virtual ~AsyncBackend() = default;
    AsyncBackend(const AsyncBackend&) = delete;
    AsyncBackend& operator=(const AsyncBackend&) = delete;

    void attach_stats(AsyncStats* s) { stats_ = s; }

    void attach_ready_sink(detail::SynchronousReadySink* sink) noexcept { routing_sink_ = sink; }

    // Installed exactly once by the AsyncIoContext constructor before the
    // backend can produce any work. There is no rebind path. A backend that
    // cannot install its required progress-notification binding fails the
    // context construction.
    void attach_progress_port(detail::BackendProgressPort port) {
        progress_port_ = port;
        if (!progress_port_attached()) {
            throw std::runtime_error(
                "sluice::async::AsyncBackend: progress notification binding install failed");
        }
    }

    // Authoritative result of one bounded nonblocking progress pass. The
    // booleans are post-pass facts about backend state, not guesses derived
    // from the completion count.
    struct ProgressPass {
        std::size_t completed = 0;
        bool immediate_work_remains = false;
        bool accepted_work_remains = false;
        // Accepted work whose transport has not reached the kernel; the owner
        // owes a further bounded pass and must not idle-return or park past it
        // without scheduling one.
        bool dispatch_retry_remains = false;
        // The context's sticky wait-health verdict at this pass.
        bool health_failed = false;
    };

    virtual std::size_t poll() = 0;

    virtual ProgressPass poll_progress() {
        ProgressPass pass;
        pass.completed = poll();
        pass.accepted_work_remains = outstanding() != 0;
        return pass;
    }

    virtual void cancel(Completion<std::size_t>& c) { (void)c; }
    virtual void cancel(Completion<void>& c) { (void)c; }

    virtual std::size_t outstanding() const noexcept = 0;

    // Whether the backend produces physical progress signals, so a driver can
    // park productively on the context progress source. Backends that answer
    // false are not v1 request backends: the context refuses to wait on them,
    // and ApplicationRuntime rejects them at build time.
    virtual bool signals_physical_progress() const noexcept { return false; }

    virtual bool supports_request_identity() const noexcept { return false; }

  private:
    friend class AsyncIoContext;
    template <class T> friend class Request;

    // Nonblocking probe of immediately actionable backend-physical work
    // (kernel CQ state for io_uring). Kernel producers bypass the userspace
    // progress epochs, so the park handshake needs this physical answer as
    // its final actionable-work check. Producers that signal through the
    // context source are covered by token revalidation and answer false.
    virtual bool has_immediate_physical_work() const noexcept { return false; }

    // A null completion destination publishes through the core only; the
    // returned key is the accepted request identity in every accepting case.
    virtual Result<detail::RequestKey> submit_read(ReadOp op, Completion<std::size_t>* c) = 0;
    virtual Result<detail::RequestKey> submit_write(WriteOp op, Completion<std::size_t>* c) = 0;
    virtual Result<detail::RequestKey> submit_sync_data(SyncDataOp op, Completion<void>* c) = 0;
    virtual Result<detail::RequestKey> submit_sync_all(SyncAllOp op, Completion<void>* c) = 0;

    virtual Result<RequestHandleState> resolve_identity_state(std::uint64_t context,
                                                              std::uint32_t slot,
                                                              std::uint64_t generation) const {
        (void)context;
        (void)slot;
        (void)generation;
        return make_unexpected<RequestHandleState>(IoError{IoError::Code::not_supported});
    }

    // The capacity of the request slot table this backend serves; the
    // context-owned core is sized by it.
    virtual std::size_t slot_capacity() const noexcept = 0;

    virtual detail::PublicCancel cancel_identity(detail::RequestKey key) = 0;

    RequestHandle identity_of(Completion<std::size_t>& c) const noexcept;
    RequestHandle identity_of(Completion<void>& c) const noexcept;

    Result<RequestHandleState> request_handle_state(const RequestHandle& h) const noexcept;

  protected:
    AsyncBackend() = default;
    AsyncStats* stats_ = nullptr;

    detail::SynchronousReadySink* routing_sink_ = nullptr;

    detail::BackendProgressPort progress_port_{};

    // Runs once after the context installs the progress port, before any work
    // can exist. Backends install their kernel progress-notification binding
    // here; false fails the context construction. They cannot replace, query,
    // or unbind the port.
    virtual bool progress_port_attached() noexcept { return true; }

    // Installed only by the AsyncIoContext constructor; backends never set
    // or clear it, and the backend retires before the core storage does.
    detail::RequestCore* core_ = nullptr;

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
    template <class T>
    static void install_core_binding(Completion<T>& c, detail::RequestCore* core,
                                     detail::RequestKey key) noexcept {
        c.install_core_binding_for_backend(core, key);
    }
    template <class T>
    static std::optional<detail::RequestKey> core_binding(const Completion<T>& c) noexcept {
        return c.core_binding_for_backend();
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

class AsyncIoContext;

// Release handle for the context's fixed driving authority: claiming records
// this thread as the context's progress owner; the handle's destruction or
// move-assignment releases the claim and detaches the authority. At most one
// live handle exists per context, and it must not outlive its context.
class ProgressOwner {
  public:
    ProgressOwner(ProgressOwner&& other) noexcept;
    ProgressOwner& operator=(ProgressOwner&& other) noexcept;
    ~ProgressOwner();

    ProgressOwner(const ProgressOwner&) = delete;
    ProgressOwner& operator=(const ProgressOwner&) = delete;

  private:
    friend class AsyncIoContext;
    ProgressOwner() noexcept = default;

    AsyncIoContext* context_ = nullptr;
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

    Result<Request<std::size_t>> submit_read(ReadOp op);
    Result<Request<std::size_t>> submit_write(WriteOp op);
    Result<Request<void>> submit_sync_data(SyncDataOp op);
    Result<Request<void>> submit_sync_all(SyncAllOp op);

    Result<RequestHandle> submit_read_request(ReadOp op, Completion<std::size_t>& c);
    Result<RequestHandle> submit_write_request(WriteOp op, Completion<std::size_t>& c);
    Result<RequestHandle> submit_sync_data_request(SyncDataOp op, Completion<void>& c);
    Result<RequestHandle> submit_sync_all_request(SyncAllOp op, Completion<void>& c);

    Result<CancelDisposition> cancel(const RequestId& id);

    RequestReadiness lookup(const RequestId& id) const;

    Result<RequestHandleState> request_state(const RequestHandle& h) const;

    Result<std::size_t> poll();

    // One bounded nonblocking progress pass with the post-pass state report;
    // the drive operation for external event-loop hosts: acknowledge
    // notification, then bounded passes. Keep passing
    // while immediate_work_remains holds; when dispatch_retry_remains holds,
    // schedule a future progress pass — that accepted transport produces no
    // completion and no notification until a pass submits it, so an fd-only
    // wait strands it. accepted_work_remains alone parks normally: that work
    // waits for its kernel completion notification.
    //
    // Driving belongs to the context's fixed progress owner: the first drive
    // or claim_progress_owner() on this thread attaches it, and a call fails
    // with invalid_state from any other thread while attached, while another
    // drive is active on this context, or when the context is inert
    // (moved-from). A ProgressOwner release detaches so a later driver may
    // attach.
    using ProgressPass = AsyncBackend::ProgressPass;
    Result<ProgressPass> poll_progress();

    // Records this thread as the context's fixed progress owner and returns
    // the release handle. Fails with invalid_state when a handle is already
    // live, when another thread holds the attached driving authority, or when
    // the context is inert.
    Result<ProgressOwner> claim_progress_owner();

    // Owner-visible wait report. `progress` covers both a pass with
    // completions and the quiescent observation (completed == 0, no accepted
    // work, nothing immediate, no retry owed). `control_interrupted` reports
    // sticky owner control, which stays pending until
    // acknowledge_progress_control() retires it. `deadline_expired` is only
    // the elapsed wait bound — accepted I/O is untouched and never cancelled
    // or settled by it. `health_failure` means the progress/wait machinery
    // cannot continue safely and is never reported as a zero-completion
    // result.
    struct ProgressWaitOutcome {
        enum class Kind : std::uint8_t {
            progress,
            control_interrupted,
            deadline_expired,
            health_failure,
        };
        Kind kind = Kind::progress;
        std::size_t completed = 0;
    };

    Result<ProgressWaitOutcome> wait_one();

    Result<ProgressWaitOutcome> wait_one(std::chrono::nanoseconds max_park);

    bool has_split_wait_capability() const noexcept;

    bool has_bounded_split_wait_capability() const noexcept;

    void interrupt_progress_waiters() noexcept;

    // Retires the control the owner last observed; control that arrived after
    // that observation stays pending and keeps interrupting later waits.
    void acknowledge_progress_control();

    // Borrows the context notification fd for external event-loop
    // registration. The borrow starts the context's external-notification
    // interest: the host must not close, independently drain, or repurpose
    // the fd; acknowledgement runs through acknowledge_progress_notification();
    // and the interest must be retired through detach_progress_host() before
    // the notification source is torn down. A retired borrow may be followed
    // by a new one.
    int progress_notification_fd() noexcept;

    void acknowledge_progress_notification() noexcept;

    // Retires the external-notification interest and returns the borrowed fd
    // the host must drop from its event loop, or -1 when no interest was
    // live. The retired registration grants no authority over any later
    // context, including one reusing the numeric descriptor.
    int detach_progress_host() noexcept;

    void cancel(Completion<std::size_t>& c);
    void cancel(Completion<void>& c);

    void set_ready_sink(detail::SynchronousReadySink* sink);

    struct ObserverAttachment {
        detail::ObserverRegistration status = detail::ObserverRegistration::not_found;
        detail::RequestKey key{};
        bool armed() const noexcept { return status == detail::ObserverRegistration::armed; }
    };

    struct ObserverCancelResult {
        detail::ObserverCancellation status = detail::ObserverCancellation::not_found;
        detail::RequestKey key{};
        bool retired() const noexcept { return status == detail::ObserverCancellation::retired; }
        bool in_progress() const noexcept {
            return status == detail::ObserverCancellation::delivery_in_progress;
        }
    };

    ObserverAttachment attach_observer(Completion<std::size_t>& c);
    ObserverAttachment attach_observer(Completion<void>& c);
    ObserverCancelResult cancel_observer(Completion<std::size_t>& c);
    ObserverCancelResult cancel_observer(Completion<void>& c);
    bool retire_delivery(detail::RequestKey key);

    std::size_t outstanding() const noexcept;
    const AsyncStats* stats() const noexcept { return stats_; }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    detail::RequestCore* context_core_for_test() noexcept { return core_.get(); }

    detail::ProgressSource::Token progress_token_for_test() const noexcept;

    void set_progress_prepark_counter_for_test(std::atomic<int>* counter) noexcept;

    void set_progress_prerevalidate_pause_gate_for_test(
        detail::ProgressSource::PauseGate* gate) noexcept;

    void set_progress_prepark_pause_gate_for_test(
        detail::ProgressSource::PauseGate* gate) noexcept;

    void set_progress_epoch_for_test(std::uint64_t epoch) noexcept;

    void set_control_epoch_for_test(std::uint64_t epoch) noexcept;

    void set_control_exhaustion_for_test(std::uint64_t exhaustion) noexcept;

    void set_wait_health_failed_for_test() noexcept;

    void saturate_progress_notification_for_test() noexcept;

    bool progress_exhausted_for_test() const noexcept;
#endif

  private:
    // Declaration order is teardown order on every exit path, constructor
    // unwind included: the backend retires before the progress source its
    // port borrows and before the core its workers call into.
    std::unique_ptr<detail::RequestCore> core_;
    std::unique_ptr<detail::ProgressSource> progress_;
    std::unique_ptr<AsyncBackend> backend_;
    AsyncStats* stats_;

    // Exhaustion closes admission before the epoch domain can wrap; the
    // source is a leaf and cannot reach the core itself.
    void close_admission_on_progress_exhaustion_() noexcept;

    bool backend_has_immediate_physical_work_() noexcept;

    friend class ProgressOwner;

    // Terminates if this context still carries a live progress binding that
    // teardown must not discard: an active drive, a claimed owner, or a live
    // external notification interest.
    void fail_fast_if_progress_binding_live_() noexcept;

    // Marks the drive domain active for the attached owner thread and reports
    // whether the caller may drive now: rejected while another drive is
    // active, while another thread holds the attached authority, or on an
    // inert context. The first drive from an unattached context attaches the
    // calling thread.
    bool drive_entry_admitted_() noexcept;
    void drive_exit_() noexcept;

    // Detaches the driving authority; called only from ProgressOwner release.
    void release_progress_owner_() noexcept;

    // One backend progress pass for a caller already holding the drive
    // domain; the public drive entry points gate before using this.
    AsyncBackend::ProgressPass run_progress_pass_();

    struct DriveGuard {
        AsyncIoContext* ctx;
        explicit DriveGuard(AsyncIoContext* c) noexcept : ctx(c) {}
        ~DriveGuard() noexcept { ctx->drive_exit_(); }
        DriveGuard(const DriveGuard&) = delete;
        DriveGuard& operator=(const DriveGuard&) = delete;
    };

    mutable std::mutex access_mtx_;
    bool drive_active_ = false;
    bool owner_claimed_ = false;
    std::thread::id owner_thread_{};
    bool notification_interest_live_ = false;
};

template <class T> Result<CancelDisposition> Request<T>::cancel() {
    if (backend_ == nullptr) {
        return make_unexpected<CancelDisposition>(IoError{IoError::Code::invalid_state});
    }
    switch (backend_->cancel_identity(key_)) {
    case detail::PublicCancel::won_before_execution:
        return CancelDisposition::won_before_execution;
    case detail::PublicCancel::requested:
        return CancelDisposition::requested;
    case detail::PublicCancel::already_terminal:
        return CancelDisposition::already_terminal;
    case detail::PublicCancel::not_found:
        return CancelDisposition::not_found;
    }
    return CancelDisposition::not_found;
}

}
