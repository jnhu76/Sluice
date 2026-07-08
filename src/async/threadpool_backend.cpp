// Implementation of ThreadPoolBackend (sluice-CORE-020A).
//
// Each submitted op spawns a worker thread that performs the blocking syscall
// (pread/pwrite/fdatasync/fsync) and pushes a terminal Result into the ready
// queue. poll() drains the queue; wait_one() blocks on the cv. The backend owns
// its worker threads and joins them in the destructor, so it outlives any in-
// flight worker — the captured `this` in worker lambdas stays valid.
#include <sluice/async/threadpool_backend.hpp>

#include <sluice/detail/posix_retry.hpp>
#include <sluice/error.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <utility>

namespace sluice::async {

ThreadPoolBackend::~ThreadPoolBackend() {
    // Mark destroying so workers don't spawn new work; join all in-flight.
    {
        std::lock_guard<std::mutex> lk(mtx_);
        destroying_ = true;
    }
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
}

namespace {
// Run a blocking read (pread) and return the result. EINTR retried.
Result<std::size_t> do_read(int fd, std::byte* dst, std::size_t len, std::uint64_t off) {
    ssize_t n = sluice::detail::retry_on_eintr([&] {
        return ::pread(fd, dst, len, static_cast<off_t>(off));
    });
    if (n < 0) return make_unexpected<std::size_t>(sluice::from_errno_value(errno));
    return static_cast<std::size_t>(n);
}
Result<std::size_t> do_write(int fd, const std::byte* src, std::size_t len, std::uint64_t off) {
    ssize_t n = sluice::detail::retry_on_eintr([&] {
        return ::pwrite(fd, src, len, static_cast<off_t>(off));
    });
    if (n < 0) return make_unexpected<std::size_t>(sluice::from_errno_value(errno));
    return static_cast<std::size_t>(n);
}
Result<void> do_sync(int fd, bool data_only) {
    int rc = sluice::detail::retry_on_eintr([&] {
        return data_only ? ::fdatasync(fd) : ::fsync(fd);
    });
    if (rc < 0) return make_unexpected<void>(sluice::from_errno_value(errno));
    return {};
}
}  // namespace

// Returns true if the op was enqueued; false if the backend is shutting down
// (submit_* then reports invalid_state to the caller). Centralizes the gate so
// every submit path enforces it identically.
bool ThreadPoolBackend::accepting_new_work() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return !destroying_;
}

void ThreadPoolBackend::enqueue_size(Completion<std::size_t>& c,
                                     std::function<Result<std::size_t>()> work) {
    c.mark_outstanding();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ++outstanding_;
    }
    Completion<std::size_t>* cp = &c;
    workers_.emplace_back([this, cp, work = std::move(work)] {
        Result<std::size_t> r = work();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ready_size_.push_back(ReadySize{cp, std::move(r)});
        }
        cv_.notify_one();
    });
}

void ThreadPoolBackend::enqueue_void(Completion<void>& c,
                                     std::function<Result<void>()> work) {
    c.mark_outstanding();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ++outstanding_;
    }
    Completion<void>* cp = &c;
    workers_.emplace_back([this, cp, work = std::move(work)] {
        Result<void> r = work();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ready_void_.push_back(ReadyVoid{cp, std::move(r)});
        }
        cv_.notify_one();
    });
}

void ThreadPoolBackend::shutting_down_for_test() {
    std::lock_guard<std::mutex> lk(mtx_);
    destroying_ = true;
}

Result<void> ThreadPoolBackend::submit_read(ReadOp op, Completion<std::size_t>& c) {
    if (!c.idle() || !accepting_new_work())
        return make_unexpected<void>(sluice::IoError{sluice::IoError::Code::invalid_state});
    enqueue_size(c, [op] { return do_read(op.fd, op.dst, op.len, op.offset); });
    return {};
}
Result<void> ThreadPoolBackend::submit_write(WriteOp op, Completion<std::size_t>& c) {
    if (!c.idle() || !accepting_new_work())
        return make_unexpected<void>(sluice::IoError{sluice::IoError::Code::invalid_state});
    enqueue_size(c, [op] { return do_write(op.fd, op.src, op.len, op.offset); });
    return {};
}
Result<void> ThreadPoolBackend::submit_sync_data(SyncDataOp op, Completion<void>& c) {
    if (!c.idle() || !accepting_new_work())
        return make_unexpected<void>(sluice::IoError{sluice::IoError::Code::invalid_state});
    enqueue_void(c, [op] { return do_sync(op.fd, /*data_only=*/true); });
    return {};
}
Result<void> ThreadPoolBackend::submit_sync_all(SyncAllOp op, Completion<void>& c) {
    if (!c.idle() || !accepting_new_work())
        return make_unexpected<void>(sluice::IoError{sluice::IoError::Code::invalid_state});
    enqueue_void(c, [op] { return do_sync(op.fd, /*data_only=*/false); });
    return {};
}

std::size_t ThreadPoolBackend::poll() {
    std::size_t n = 0;
    std::deque<ReadySize> rs;
    std::deque<ReadyVoid> rv;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        rs.swap(ready_size_);
        rv.swap(ready_void_);
    }
    for (auto& e : rs) {
        e.c->complete_with(std::move(e.r));
        ++n;
    }
    for (auto& e : rv) {
        e.c->complete_with(std::move(e.r));
        ++n;
    }
    if (n) {
        std::lock_guard<std::mutex> lk(mtx_);
        outstanding_ -= n;
    }
    return n;
}

Result<std::size_t> ThreadPoolBackend::wait_one() {
    {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [&] { return !ready_size_.empty() || !ready_void_.empty(); });
    }
    return poll();
}

void ThreadPoolBackend::cancel(Completion<std::size_t>& c) {
    (void)c;
    // Portable cancel of an in-flight blocking syscall is deferred (ADR §7 X2).
    // Here cancel is best-effort/no-op: the op completes with its real result
    // when the syscall returns (exactly-once). Recorded as a known limitation.
}
void ThreadPoolBackend::cancel(Completion<void>& c) { (void)c; }

std::size_t ThreadPoolBackend::outstanding() const noexcept {
    std::lock_guard<std::mutex> lk(mtx_);
    return outstanding_;
}

}  // namespace sluice::async
