


#include <sluice/async/application_runtime.hpp>

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/group.hpp>
#include <sluice/async/scheduler.hpp>

#include <cassert>
#include <cstdio>
#include <exception>

namespace sluice::async {








void ApplicationRuntime::set_current_fiber_tag(ApplicationRuntime* rt) noexcept {
    Scheduler::set_current_fiber_execution_tag(rt);
}

ApplicationRuntime* ApplicationRuntime::current_fiber_tag() noexcept {
    return static_cast<ApplicationRuntime*>(
        Scheduler::current_fiber_execution_tag());
}




CancelToken& RuntimeTaskContext::cancel_token() noexcept { return *token_; }

Result<void> RuntimeTaskContext::submit_read(ReadOp op, Completion<std::size_t>& c) {
    return ctx_->submit_read(op, c);
}
Result<void> RuntimeTaskContext::submit_write(WriteOp op, Completion<std::size_t>& c) {
    return ctx_->submit_write(op, c);
}
Result<void> RuntimeTaskContext::submit_sync_data(SyncDataOp op, Completion<void>& c) {
    return ctx_->submit_sync_data(op, c);
}
Result<void> RuntimeTaskContext::submit_sync_all(SyncAllOp op, Completion<void>& c) {
    return ctx_->submit_sync_all(op, c);
}


Result<RequestHandle> RuntimeTaskContext::submit_read_request(ReadOp op,
                                                              Completion<std::size_t>& c) {
    return ctx_->submit_read_request(op, c);
}
Result<RequestHandle> RuntimeTaskContext::submit_write_request(WriteOp op,
                                                               Completion<std::size_t>& c) {
    return ctx_->submit_write_request(op, c);
}
Result<RequestHandle> RuntimeTaskContext::submit_sync_data_request(SyncDataOp op,
                                                                   Completion<void>& c) {
    return ctx_->submit_sync_data_request(op, c);
}
Result<RequestHandle> RuntimeTaskContext::submit_sync_all_request(SyncAllOp op,
                                                                  Completion<void>& c) {
    return ctx_->submit_sync_all_request(op, c);
}












Result<void> RuntimeTaskContext::await_completion(Completion<std::size_t>& c) {
    assert(!c.idle() &&
           "await_completion requires a submitted or ready Completion "
           "(idle-await is a caller contract violation: M1-A)");




    return sched_->await_completion_size(c);
}

Result<void> RuntimeTaskContext::await_completion(Completion<void>& c) {
    assert(!c.idle() &&
           "await_completion requires a submitted or ready Completion "
           "(idle-await is a caller contract violation: M1-A)");
    return sched_->await_completion_void(c);
}

Result<bool> RuntimeTaskContext::cancel_waiter(Completion<std::size_t>& c) {


    return sched_->cancel_waiter(c);
}

Result<bool> RuntimeTaskContext::cancel_waiter(Completion<void>& c) {
    return sched_->cancel_waiter(c);
}

#ifdef SLUICE_ASYNC_INTERNAL_TESTING
void RuntimeTaskContext::suspend(std::atomic<bool>& flag) {
    sched_->await_ready_flag(flag);
}
#endif




RuntimeBuilder& RuntimeBuilder::backend(std::unique_ptr<AsyncBackend> b) {
    backend_ = std::move(b);
    return *this;
}

RuntimeBuilder& RuntimeBuilder::workers(unsigned n) {
    workers_ = n;
    return *this;
}

Result<std::unique_ptr<ApplicationRuntime>> RuntimeBuilder::build() {
    if (!backend_) {
        return make_unexpected<std::unique_ptr<ApplicationRuntime>>(
            IoError{IoError::Code::invalid_state});
    }
    if (workers_ == 0) {
        workers_ = 1;
    }









    if (backend_->wait_source() == nullptr && !backend_->wait_one_is_nonblocking()) {
        return make_unexpected<std::unique_ptr<ApplicationRuntime>>(
            IoError{IoError::Code::invalid_state});
    }


    std::unique_ptr<ApplicationRuntime> rt(
        new ApplicationRuntime(std::move(backend_), workers_));
    return std::move(rt);
}




ApplicationRuntime::ApplicationRuntime(std::unique_ptr<AsyncBackend> backend,
                                       unsigned workers)
    : io_ctx_(std::make_unique<AsyncIoContext>(std::move(backend)))
    , worker_count_(workers) {

    sched_ = std::make_unique<Scheduler>(*io_ctx_);

    root_group_ = std::make_unique<Group>(*sched_);

    wake_handle_ = sched_->make_wake_handle();
}

ApplicationRuntime::~ApplicationRuntime() {



    State s;
    {
        std::lock_guard lk(lifecycle_mtx_);
        s = state_;
    }
    if (s != State::Constructed && s != State::StartFailed && s != State::Stopped) {
        detail::group_lifetime_fail_fast();
    }

    if (driver_thread_.joinable()) {
        driver_thread_.join();
    }



}




Result<void> ApplicationRuntime::start() {
    std::unique_lock lk(lifecycle_mtx_);










    if (state_ != State::Constructed || close_state_ != CloseState::Open) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }









    if (stop_requested_) {
        close_state_ = CloseState::InProgress;
        lk.unlock();
        close_resources();
        return make_unexpected_void(IoError{IoError::Code::canceled});
    }

    state_ = State::Starting;


    try {
        driver_thread_ = std::thread([this] { driver_main(); });
        driver_spawned_ = true;
    } catch (const std::system_error&) {
        state_ = State::StartFailed;
        driver_state_ = DriverState::not_started;
        return make_unexpected_void(IoError{IoError::Code::backend_error});
    }



    runtime_cv_.wait(lk, [this] {
        return driver_state_ == DriverState::barrier_wait ||
               driver_state_ == DriverState::exited;
    });








    if (driver_state_ == DriverState::exited) {


        close_state_ = CloseState::InProgress;
        runtime_cv_.notify_all();
        lk.unlock();
        if (driver_thread_.joinable()) driver_thread_.join();
        close_resources();
        return make_unexpected_void(IoError{IoError::Code::canceled});
    }


#ifdef SLUICE_ASYNC_INTERNAL_TESTING








    if (test_pause_at_commit_checkpoint_.load(std::memory_order::acquire)) {
        commit_checkpoint_promise_.set_value();
        commit_release_flag_.store(false, std::memory_order::release);
        runtime_cv_.notify_all();
        runtime_cv_.wait(lk, [this] {
            return commit_release_flag_.load(std::memory_order::acquire);
        });
    }
#endif
    if (stop_requested_) {



        startup_abort_requested_ = true;
        close_state_ = CloseState::InProgress;
        control_epoch_++;
        runtime_cv_.notify_all();

        runtime_cv_.wait(lk, [this] {
            return driver_state_ == DriverState::exited;
        });
        lk.unlock();
        if (driver_thread_.joinable()) driver_thread_.join();
        close_resources();
        return make_unexpected_void(IoError{IoError::Code::canceled});
    }


    state_ = State::Running;
    admission_open_ = true;
    control_epoch_++;
    runtime_cv_.notify_all();
    wake_handle_.notify();

    return {};
}




Result<void> ApplicationRuntime::submit(RuntimeTaskFn task) {
    if (!task) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }

    std::unique_lock lk(lifecycle_mtx_);


    if (!admission_open_) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }




    admitted_count_++;
    drain_complete_ = false;
    recompute_task_set_terminal_locked();


    lk.unlock();


    try {
        root_group_->async([this, task = std::move(task)](CancelToken& token) mutable {




            auto* prev_tag = current_fiber_tag();
            set_current_fiber_tag(this);





            RuntimeTaskContext ctx(*io_ctx_, token, *sched_);


            try {
                task(ctx);
            } catch (...) {

            }


            {
                std::lock_guard glk(lifecycle_mtx_);
                terminal_count_++;
                recompute_task_set_terminal_locked();
                control_epoch_++;
            }
            runtime_cv_.notify_all();
            wake_handle_.notify();


            set_current_fiber_tag(prev_tag);
        });
    } catch (...) {


        lk.lock();
        admitted_count_--;
        recompute_task_set_terminal_locked();
        control_epoch_++;
        lk.unlock();
        runtime_cv_.notify_all();
        wake_handle_.notify();
        throw;
    }


    {
        std::lock_guard slk(lifecycle_mtx_);
        control_epoch_++;
    }
    runtime_cv_.notify_all();
    wake_handle_.notify();

    return {};
}




void ApplicationRuntime::request_stop() noexcept {
    std::lock_guard lk(lifecycle_mtx_);

    if (stop_requested_) return;
    stop_requested_ = true;

    if (state_ == State::Running) {

        admission_open_ = false;
        admission_closed_snapshot_.store(true, std::memory_order::release);

        if (!root_cancel_published_ && root_group_) {
            root_group_->group_token().request();
            root_cancel_published_ = true;
        }

        state_ = State::Stopping;
        control_epoch_++;
    } else if (state_ == State::Constructed) {


    } else if (state_ == State::Starting) {

        startup_abort_requested_ = true;
        control_epoch_++;
    }











    if (io_ctx_) io_ctx_->interrupt_backend_waiters();

    runtime_cv_.notify_all();
    wake_handle_.notify();
}




Result<void> ApplicationRuntime::drain() {
    if (is_runtime_task()) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }

    std::unique_lock lk(lifecycle_mtx_);

    if (state_ != State::Stopping && state_ != State::Draining) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }

    if (state_ == State::Stopping) {
        state_ = State::Draining;
    }


    runtime_cv_.wait(lk, [this] {
        return drain_complete_ || state_ == State::Fatal;
    });

    if (state_ == State::Fatal) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }

    return {};
}




Result<void> ApplicationRuntime::join() {
    if (is_runtime_task()) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }

    std::unique_lock lk(lifecycle_mtx_);


    if (!drain_complete_) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }


    if (close_state_ == CloseState::Closed) {
        return {};
    }
    if (close_state_ == CloseState::InProgress) {

        runtime_cv_.wait(lk, [this] { return close_state_ == CloseState::Closed; });
        return {};
    }


    close_state_ = CloseState::InProgress;
    driver_exit_requested_ = true;
    driver_exit_snapshot_.store(true, std::memory_order::release);
    control_epoch_++;
    runtime_cv_.notify_all();
    wake_handle_.notify();


    runtime_cv_.wait(lk, [this] {
        return driver_state_ == DriverState::exited;
    });

    lk.unlock();


    if (driver_thread_.joinable()) {
        driver_thread_.join();
    }


    close_resources();

    return {};
}




Result<void> ApplicationRuntime::shutdown() {
    if (is_runtime_task()) {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }

    std::unique_lock lk(lifecycle_mtx_);


    if (close_state_ == CloseState::Closed) {
        return {};
    }


    switch (state_) {
    case State::Constructed:
    case State::StartFailed: {








        if (close_state_ == CloseState::InProgress) {
            runtime_cv_.wait(lk, [this] { return close_state_ == CloseState::Closed; });
            return {};
        }
        close_state_ = CloseState::InProgress;
        lk.unlock();
        close_resources();
        return {};
    }

    case State::Starting: {

        startup_abort_requested_ = true;
        stop_requested_ = true;
        control_epoch_++;
        runtime_cv_.notify_all();
        wake_handle_.notify();

        runtime_cv_.wait(lk, [this] { return close_state_ == CloseState::Closed; });
        return {};
    }

    case State::Running: {

        lk.unlock();
        request_stop();
        auto dr = drain();
        if (!dr.has_value()) {

            std::lock_guard rlk(lifecycle_mtx_);
            if (close_state_ == CloseState::Closed) return {};
            return dr;
        }
        return join();
    }

    case State::Stopping: {
        lk.unlock();
        auto dr = drain();
        if (!dr.has_value()) {
            std::lock_guard rlk(lifecycle_mtx_);
            if (close_state_ == CloseState::Closed) return {};
            return dr;
        }
        return join();
    }

    case State::Draining: {

        runtime_cv_.wait(lk, [this] {
            return drain_complete_ || state_ == State::Fatal ||
                   close_state_ == CloseState::Closed;
        });
        if (close_state_ == CloseState::Closed) return {};
        lk.unlock();
        return join();
    }

    case State::Stopped: {
        return {};
    }

    case State::Fatal: {
        return make_unexpected_void(IoError{IoError::Code::invalid_state});
    }
    }

    return make_unexpected_void(IoError{IoError::Code::invalid_state});
}




void ApplicationRuntime::driver_main() {
    std::unique_lock lk(lifecycle_mtx_);


    driver_state_ = DriverState::barrier_wait;
#ifdef SLUICE_ASYNC_INTERNAL_TESTING
    barrier_promise_.set_value();
#endif
    runtime_cv_.notify_all();


    runtime_cv_.wait(lk, [this] {
        return state_ != State::Starting || startup_abort_requested_;
    });

    if (startup_abort_requested_) {

        driver_state_ = DriverState::exited;
        runtime_cv_.notify_all();
        return;
    }


    driver_state_ = DriverState::in_run_live;
    observed_epoch_ = control_epoch_;
    lk.unlock();



    for (;;) {

        sched_->run_live(worker_count_, &stop_predicate_trampoline, this);


        lk.lock();
        driver_state_ = DriverState::between_invocations;


        if (driver_exit_requested_ || fatal_snapshot_.load(std::memory_order::acquire)) {
            driver_state_ = DriverState::exited;
            runtime_cv_.notify_all();
            return;
        }






        if (task_set_terminal_snapshot_.load(std::memory_order::acquire) &&
            io_ctx_->outstanding() == 0 &&
            (state_ == State::Stopping || state_ == State::Draining)) {
            drain_complete_ = true;
            runtime_cv_.notify_all();





            observed_epoch_ = control_epoch_;
            driver_state_ = DriverState::drained_wait;
            runtime_cv_.wait(lk, [this] {
                return driver_exit_requested_ ||
                       fatal_snapshot_.load(std::memory_order::acquire) ||
                       control_epoch_ != observed_epoch_;
            });

            if (driver_exit_requested_ ||
                fatal_snapshot_.load(std::memory_order::acquire)) {
                driver_state_ = DriverState::exited;
                runtime_cv_.notify_all();
                return;
            }


            observed_epoch_ = control_epoch_;
            driver_state_ = DriverState::in_run_live;
            lk.unlock();
            continue;
        }






        if (control_epoch_ != observed_epoch_) {
            observed_epoch_ = control_epoch_;
            driver_state_ = DriverState::in_run_live;
            lk.unlock();
            continue;
        }
























        if (io_ctx_ && io_ctx_->outstanding() > 0) {
            driver_state_ = DriverState::in_run_live;
            lk.unlock();
            continue;
        }


        runtime_cv_.wait(lk, [this] {
            return driver_exit_requested_ ||
                   fatal_snapshot_.load(std::memory_order::acquire) ||
                   control_epoch_ != observed_epoch_;
        });

        if (driver_exit_requested_ ||
            fatal_snapshot_.load(std::memory_order::acquire)) {
            driver_state_ = DriverState::exited;
            runtime_cv_.notify_all();
            return;
        }


        observed_epoch_ = control_epoch_;
        driver_state_ = DriverState::in_run_live;
        lk.unlock();
    }
}





bool ApplicationRuntime::stop_predicate_fn() {




    return fatal_snapshot_.load(std::memory_order::acquire) ||
           driver_exit_snapshot_.load(std::memory_order::acquire) ||
           (admission_closed_snapshot_.load(std::memory_order::acquire) &&
            task_set_terminal_snapshot_.load(std::memory_order::acquire));
}

bool ApplicationRuntime::stop_predicate_trampoline(void* ctx) {
    return static_cast<ApplicationRuntime*>(ctx)->stop_predicate_fn();
}




void ApplicationRuntime::recompute_task_set_terminal_locked() {


    bool terminal = (terminal_count_ >= admitted_count_);
    task_set_terminal_snapshot_.store(terminal, std::memory_order::release);
}

void ApplicationRuntime::close_resources() {





    std::unique_ptr<Group> group;
    std::unique_ptr<Scheduler> sched;
    std::unique_ptr<AsyncIoContext> io_ctx;
    {
        std::lock_guard lk(lifecycle_mtx_);

        admission_open_ = false;
        admission_closed_snapshot_.store(true, std::memory_order::release);
        group = std::move(root_group_);
        sched = std::move(sched_);
        io_ctx = std::move(io_ctx_);
    }


    group.reset();
    sched.reset();
    io_ctx.reset();

    {
        std::lock_guard lk(lifecycle_mtx_);
        state_ = State::Stopped;
        close_state_ = CloseState::Closed;
        runtime_cv_.notify_all();
    }
}

bool ApplicationRuntime::is_runtime_task() const noexcept {



    return current_fiber_tag() == this;
}

#ifdef SLUICE_ASYNC_INTERNAL_TESTING
void ApplicationRuntime::test_dump_forensics(const char* tag) {







    static const char* kStateName[] = {"Constructed", "Starting", "Running",
                                       "Stopping",   "Draining", "Stopped",
                                       "StartFailed", "Fatal"};
    static const char* kDriverName[] = {"not_started", "barrier_wait",
                                        "in_run_live", "between_invocations",
                                        "drained_wait", "exited"};
    State s;
    DriverState ds;
    std::uint64_t ctrl, obs;
    std::size_t admitted, terminal;
    bool drain_done, stop;
    {
        std::lock_guard lk(lifecycle_mtx_);
        s = state_;
        ds = driver_state_;
        ctrl = control_epoch_;
        obs = observed_epoch_;
        admitted = admitted_count_;
        terminal = terminal_count_;
        drain_done = drain_complete_;
        stop = stop_requested_;
    }
    std::fprintf(stderr,
                 "[issue116-forensics] runtime %s: state=%s driver=%s "
                 "control_epoch=%lu observed_epoch=%lu admitted=%zu "
                 "terminal=%zu drain_complete=%d stop_requested=%d\n",
                 tag, kStateName[static_cast<int>(s)],
                 kDriverName[static_cast<int>(ds)],
                 static_cast<unsigned long>(ctrl),
                 static_cast<unsigned long>(obs), admitted, terminal,
                 (int)drain_done, (int)stop);
    if (io_ctx_) {
        std::fprintf(stderr,
                     "[issue116-forensics] io %s: accepted_outstanding=%zu\n",
                     tag, io_ctx_->outstanding());
    }
    if (sched_) {
        Scheduler::AsyncTestAccess::dump_park_forensics(*sched_, tag);
    }
}

void ApplicationRuntime::test_inject_next_submit_throw() {




    root_group_->test_set_evented_admission_fail(
        Group::EventedAdmissionFailPoint::before_fiber_storage_reserve);
}
#endif

}
