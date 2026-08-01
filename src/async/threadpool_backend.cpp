// Implementation of ThreadPoolBackend (sluice-CORE-020A).
//
// Each submitted op spawns a worker thread that performs the blocking syscall
// (pread/pwrite/fdatasync/fsync) and pushes a terminal Result into the ready
// queue. poll() drains the queue; wait_one() blocks on the cv. Each reaped
// entry's worker is joined immediately outside the lock, keeping unjoined
// pthread resources bounded by outstanding ops. The destructor joins any
// remaining in-flight workers, so it outlives any worker — the captured
// `this` in worker lambdas stays valid.
#include <sluice/async/threadpool_backend.hpp>

#include <sluice/detail/io_validation.hpp>
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
Result<std::size_t> do_read(int fd, std::byte* dst, std::size_t len, off_t off) {
    ssize_t n = sluice::detail::retry_on_eintr([&] {
        return ::pread(fd, dst, len, off);
    });
    if (n < 0) return make_unexpected<std::size_t>(sluice::from_errno_value(errno));
    return static_cast<std::size_t>(n);
}
Result<std::size_t> do_write(int fd, const std::byte* src, std::size_t len, off_t off) {
    ssize_t n = sluice::detail::retry_on_eintr([&] {
        return ::pwrite(fd, src, len, off);
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
    Completion<std::size_t>* cp = &c;
    std::size_t worker_idx = kNoWorker;
    try {
        // workers_ is touched by the reaper (poll) and the test seam, so its
        // mutation must be under mtx_ like every other shared member. The
        // worker pushes the terminal result together with its own index so the
        // reaper can join exactly this thread after draining the result. If
        // the spawn throws, the lock is released during unwind BEFORE the
        // catch runs, so fail_spawn_* can re-lock (std::mutex is not
        // recursive).
        std::lock_guard<std::mutex> lk(mtx_);
        ++outstanding_;
        worker_idx = workers_.size();
        workers_.emplace_back([this, cp, worker_idx, work = std::move(work)] {
            Result<std::size_t> r = work();
            {
                std::lock_guard<std::mutex> lk(mtx_);
                ready_size_.push_back(ReadySize{cp, std::move(r), worker_idx});
            }
            cv_.notify_one();
        });
    } catch (const std::bad_alloc&) {
        fail_spawn_size(cp, IoError{IoError::Code::no_space});
    } catch (const std::system_error& e) {
        IoError err{IoError::Code::backend_error};
        if (e.code().value() > 0) err.os_errno = e.code().value();
        fail_spawn_size(cp, err);
    } catch (...) {
        fail_spawn_size(cp, IoError{IoError::Code::backend_error});
    }
}

void ThreadPoolBackend::enqueue_void(Completion<void>& c,
                                     std::function<Result<void>()> work) {
    c.mark_outstanding();
    Completion<void>* cp = &c;
    std::size_t worker_idx = kNoWorker;
    try {
        std::lock_guard<std::mutex> lk(mtx_);
        ++outstanding_;
        worker_idx = workers_.size();
        workers_.emplace_back([this, cp, worker_idx, work = std::move(work)] {
            Result<void> r = work();
            {
                std::lock_guard<std::mutex> lk(mtx_);
                ready_void_.push_back(ReadyVoid{cp, std::move(r), worker_idx});
            }
            cv_.notify_one();
        });
    } catch (const std::bad_alloc&) {
        fail_spawn_void(cp, IoError{IoError::Code::no_space});
    } catch (const std::system_error& e) {
        IoError err{IoError::Code::backend_error};
        if (e.code().value() > 0) err.os_errno = e.code().value();
        fail_spawn_void(cp, err);
    } catch (...) {
        fail_spawn_void(cp, IoError{IoError::Code::backend_error});
    }
}

void ThreadPoolBackend::fail_spawn_size(Completion<std::size_t>* c,
                                        const IoError& err) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ready_size_.push_back(ReadySize{c, make_unexpected<std::size_t>(err),
                                        kNoWorker});
    }
    cv_.notify_one();
}

void ThreadPoolBackend::fail_spawn_void(Completion<void>* c,
                                        const IoError& err) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ready_void_.push_back(ReadyVoid{c, make_unexpected<void>(err),
                                        kNoWorker});
    }
    cv_.notify_one();
}

void ThreadPoolBackend::shutting_down_for_test() {
    std::lock_guard<std::mutex> lk(mtx_);
    destroying_ = true;
}

Result<void> ThreadPoolBackend::submit_read(ReadOp op, Completion<std::size_t>& c) {
    if (!c.idle() || !accepting_new_work())
        return make_unexpected<void>(sluice::IoError{sluice::IoError::Code::invalid_state});
    auto native_offset = sluice::detail::checked_posix_offset(op.offset);
    if (!native_offset.has_value())
        return make_unexpected<void>(native_offset.error());
    enqueue_size(c, [op, off = native_offset.value()] {
        return do_read(op.fd, op.dst, op.len, off);
    });
    return {};
}
Result<void> ThreadPoolBackend::submit_write(WriteOp op, Completion<std::size_t>& c) {
    if (!c.idle() || !accepting_new_work())
        return make_unexpected<void>(sluice::IoError{sluice::IoError::Code::invalid_state});
    auto native_offset = sluice::detail::checked_posix_offset(op.offset);
    if (!native_offset.has_value())
        return make_unexpected<void>(native_offset.error());
    enqueue_size(c, [op, off = native_offset.value()] {
        return do_write(op.fd, op.src, op.len, off);
    });
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
    std::deque<ReadySize> rs;
    std::deque<ReadyVoid> rv;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        rs.swap(ready_size_);
        rv.swap(ready_void_);
    }

    // Reap each entry individually: complete the op, decrement outstanding_,
    // take the worker out, then join OUTSIDE mtx_. This avoids any dynamic
    // allocation between consuming a ready entry and committing its state
    // transition (the old to_join.reserve() could throw std::bad_alloc after
    // completions were published but before outstanding_ was decremented,
    // leaving a half-committed state that would hang wait_one()). join()
    // blocks the caller until thread teardown finishes; holding mtx_ across
    // it would stall every other submit and worker publication. A drained
    // worker has already pushed its result (that is how it got here) and
    // holds no lock afterwards, so join() returns promptly. Each worker is
    // moved out exactly once (its slot becomes a non-joinable placeholder).
    // Spawn-failure entries carry kNoWorker and have nothing to join.
    std::size_t n = 0;

    auto reap_size = [this](ReadySize& e) {
        e.c->complete_with(std::move(e.r));
        std::thread worker;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            --outstanding_;
            if (e.worker != kNoWorker)
                worker = take_worker_for_join_locked(e.worker);
        }
        if (worker.joinable())
            worker.join();
    };
    auto reap_void = [this](ReadyVoid& e) {
        e.c->complete_with(std::move(e.r));
        std::thread worker;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            --outstanding_;
            if (e.worker != kNoWorker)
                worker = take_worker_for_join_locked(e.worker);
        }
        if (worker.joinable())
            worker.join();
    };

    for (auto& e : rs) {
        reap_size(e);
        ++n;
    }
    for (auto& e : rv) {
        reap_void(e);
        ++n;
    }
    return n;
}

std::thread ThreadPoolBackend::take_worker_for_join_locked(std::size_t index) {
    // Caller holds mtx_. Defensive range/joinability validation: the index
    // comes from the worker's own spawn record (workers_ only ever grows, so
    // it stays valid), and a slot is taken at most once.
    if (index >= workers_.size()) return {};
    std::thread& w = workers_[index];
    if (!w.joinable()) return {};
    return std::move(w);
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
