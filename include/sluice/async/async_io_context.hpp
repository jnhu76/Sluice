#pragma once

#include <sluice/async/request.hpp>
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
#include <stdexcept>
#include <thread>

namespace sluice::async {

class AsyncBackend;
class AsyncIoContext;

namespace detail {
class RequestCore;
class ProgressSource;
struct ContextFactory;
struct TestAccess;
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
struct FileInfoOp {
    NativeFileRef file;
};
struct SizeOp {
    NativeFileRef file;
};

// Authoritative result of one bounded nonblocking progress pass. The booleans
// are post-pass facts about backend state, not guesses derived from the
// completion count.
struct ProgressPass {
    std::size_t completed = 0;
    bool immediate_work_remains = false;
    bool accepted_work_remains = false;
    // Accepted work whose transport has not reached the kernel; the owner owes
    // a further bounded pass and must not idle-return or park past it without
    // scheduling one.
    bool dispatch_retry_remains = false;
    // The context's sticky wait-health verdict at this pass.
    bool health_failed = false;
};

enum class ShutdownPolicy : std::uint8_t {
    drain,
    cancel_then_drain,
};

enum class ShutdownOutcome : std::uint8_t {
    completed,
    health_unresolved,
    health_failed,
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
    AsyncIoContext(const AsyncIoContext&) = delete;
    AsyncIoContext& operator=(const AsyncIoContext&) = delete;

    AsyncIoContext(AsyncIoContext&&) noexcept;

    AsyncIoContext& operator=(AsyncIoContext&&) noexcept;

    ~AsyncIoContext();

    Result<Request<std::size_t>> submit_read(ReadOp op);
    Result<Request<std::size_t>> submit_write(WriteOp op);
    Result<Request<void>> submit_sync_data(SyncDataOp op);
    Result<Request<void>> submit_sync_all(SyncAllOp op);
    Result<Request<FileInfo>> submit_file_info(FileInfoOp op);
    Result<Request<FileSize>> submit_size(SizeOp op);

    Result<CancelDisposition> cancel(const RequestId& id);

    RequestReadiness lookup(const RequestId& id) const;

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
    // that observation stays pending and keeps interrupting later waits. The
    // caller must be the context's attached progress owner: control
    // acknowledgement is an owner action in the driver-wait family
    // (THREAD-01), retiring control that owner's own wait outcome observed.
    void acknowledge_progress_control();

    // Borrows the context notification fd for external event-loop
    // registration. The borrow starts the context's external-notification
    // interest: the host must not close, independently drain, or repurpose
    // the fd; acknowledgement runs through acknowledge_progress_notification();
    // and the interest must be retired through detach_progress_host() before
    // the notification source is torn down. A retired borrow may be followed
    // by a new one. Returns -1 without starting an interest while the owner
    // is inside a drive callback or once execution has closed; a host that
    // needs the fd registers it before the owner starts driving or after a
    // drive callback returns.
    int progress_notification_fd() noexcept;

    void acknowledge_progress_notification() noexcept;

    // Retires the external-notification interest. The host must call this
    // only after it has already unregistered the fd from its event loop and
    // retired every callback that might use the context: this operation
    // acknowledges that the external registration authority is already gone,
    // so the context never believes no binding exists while a loop still
    // owns one. Idempotent; a retired interest may be followed by a new
    // borrow.
    void detach_progress_host() noexcept;

    // shutdown is blocking; see ADR-0004 for the caller contract.
    void close_admission() noexcept;
    void request_stop(ShutdownPolicy policy) noexcept;
    Result<ShutdownOutcome> shutdown(ShutdownPolicy policy);

    bool admission_open() const noexcept;

    bool execution_closed() const noexcept;

    enum class ObserverStatus : std::uint8_t {
        armed,
        duplicate,
        already_terminal,
        admission_closed,
        not_found,
    };

    struct ObserverAttachment {
        ObserverStatus status = ObserverStatus::not_found;
        RequestId request{};
        bool armed() const noexcept { return status == ObserverStatus::armed; }
    };

    enum class ObserverCancelStatus : std::uint8_t {
        retired,
        delivery_in_progress,
        not_registered,
        not_found,
    };

    struct ObserverCancelResult {
        ObserverCancelStatus status = ObserverCancelStatus::not_found;
        RequestId request{};
        bool retired() const noexcept { return status == ObserverCancelStatus::retired; }
        bool in_progress() const noexcept {
            return status == ObserverCancelStatus::delivery_in_progress;
        }
    };

    ObserverAttachment attach_observer(const RequestId& id);

    ObserverCancelResult cancel_observer(const RequestId& id);

    // Retirement query: reports whether the registration's delivery
    // retirement has been acquired. The driver retires a delivered
    // registration once its terminal hook has returned; a cancellation
    // retires an armed or queued one. While a delivery is in progress this
    // reports false and the caller must retain its observer state. Query it
    // while the Request binding is retained; once the released slot
    // reclaims, the identity stops resolving and this reports false, so the
    // answer is only authoritative before the binding is released.
    bool delivery_retired(const RequestId& id) const;

    std::size_t outstanding() const noexcept;
    const AsyncStats* stats() const noexcept { return stats_; }

  private:
    friend struct detail::ContextFactory;
    friend struct detail::TestAccess;

    explicit AsyncIoContext(std::unique_ptr<AsyncBackend> backend, AsyncStats* stats = nullptr);

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
    ProgressPass run_progress_pass_();

    // Drives to the execution-closed transition. The caller holds the drive
    // domain (shutdown) or the destructor's exclusive position.
    ShutdownOutcome drive_settlement_();

    bool settlement_converged_() const noexcept;

    void cancel_outstanding_for_stop_() noexcept;

    void upgrade_stop_policy_(ShutdownPolicy policy) noexcept;

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

    // Strongest policy ever requested; monotone, never reopens. Upgrade is
    // recorded under access_mtx_; the settling driver may read it without
    // that lock (THREAD-01 lets request_stop race an in-flight drive).
    std::atomic<ShutdownPolicy> stop_policy_{ShutdownPolicy::drain};

    // Written once by the settling owner after the underlying facts held and
    // the resources they describe were retired, so it is not recomputable.
    bool execution_closed_ = false;
    ShutdownOutcome settlement_outcome_ = ShutdownOutcome::health_failed;
};

}
