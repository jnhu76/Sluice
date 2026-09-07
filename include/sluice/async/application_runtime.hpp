













#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/cancel.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/result.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#ifdef SLUICE_ASYNC_INTERNAL_TESTING
#include <future>
#endif

namespace sluice::async {


class Group;
class Fiber;
class ApplicationRuntime;














class RuntimeTaskContext {
public:
    CancelToken& cancel_token() noexcept;

    Result<void> submit_read(ReadOp op, Completion<std::size_t>& c);
    Result<void> submit_write(WriteOp op, Completion<std::size_t>& c);
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& c);
    Result<void> submit_sync_all(SyncAllOp op, Completion<void>& c);






    Result<RequestHandle> submit_read_request(ReadOp op, Completion<std::size_t>& c);
    Result<RequestHandle> submit_write_request(WriteOp op, Completion<std::size_t>& c);
    Result<RequestHandle> submit_sync_data_request(SyncDataOp op, Completion<void>& c);
    Result<RequestHandle> submit_sync_all_request(SyncAllOp op, Completion<void>& c);



































    Result<void> await_completion(Completion<std::size_t>& c);
    Result<void> await_completion(Completion<void>& c);









    Result<bool> cancel_waiter(Completion<std::size_t>& c);
    Result<bool> cancel_waiter(Completion<void>& c);

#ifdef SLUICE_ASYNC_INTERNAL_TESTING







    void suspend(std::atomic<bool>& flag);
#endif

    RuntimeTaskContext(const RuntimeTaskContext&) = delete;
    RuntimeTaskContext& operator=(const RuntimeTaskContext&) = delete;

private:
    friend class ApplicationRuntime;







    RuntimeTaskContext(AsyncIoContext& ctx, CancelToken& token,
                       Scheduler& sched) noexcept
        : ctx_(&ctx), token_(&token), sched_(&sched) {}

    AsyncIoContext* ctx_;
    CancelToken* token_;
    Scheduler* sched_;
};



using RuntimeTaskFn = std::function<void(RuntimeTaskContext&)>;





class RuntimeBuilder {
public:
    RuntimeBuilder() = default;









    RuntimeBuilder& backend(std::unique_ptr<AsyncBackend> b);


    RuntimeBuilder& workers(unsigned n);



    Result<std::unique_ptr<ApplicationRuntime>> build();

private:
    std::unique_ptr<AsyncBackend> backend_;
    unsigned workers_ = 1;
};














class ApplicationRuntime {
public:
    ~ApplicationRuntime();

    ApplicationRuntime(const ApplicationRuntime&) = delete;
    ApplicationRuntime& operator=(const ApplicationRuntime&) = delete;
    ApplicationRuntime(ApplicationRuntime&&) = delete;
    ApplicationRuntime& operator=(ApplicationRuntime&&) = delete;




    Result<void> start();



    Result<void> submit(RuntimeTaskFn task);



    void request_stop() noexcept;




    Result<void> drain();



    Result<void> join();



    Result<void> shutdown();

#ifdef SLUICE_ASYNC_INTERNAL_TESTING




    std::future<void> test_driver_barrier_reached() {
        return barrier_promise_.get_future();
    }









    void test_set_pause_at_commit_checkpoint(bool enable) {
        test_pause_at_commit_checkpoint_.store(enable, std::memory_order::release);
    }








    std::future<void> test_start_owner_at_commit_checkpoint() {
        return commit_checkpoint_promise_.get_future();
    }




    void test_release_start_owner_at_commit_checkpoint() {
        commit_release_flag_.store(true, std::memory_order::release);
        runtime_cv_.notify_all();
    }




    Scheduler& test_scheduler_for_worker_topology() noexcept { return *sched_; }








    void test_dump_forensics(const char* tag);









    void test_inject_next_submit_throw();
#endif

private:
    friend class RuntimeBuilder;
    friend class RuntimeTaskContext;


    ApplicationRuntime(std::unique_ptr<AsyncBackend> backend, unsigned workers);


    enum class State : std::uint8_t {
        Constructed,
        Starting,
        Running,
        Stopping,
        Draining,
        Stopped,
        StartFailed,
        Fatal,
    };


    enum class CloseState : std::uint8_t {
        Open,
        InProgress,
        Closed,
    };


    enum class DriverState : std::uint8_t {
        not_started,
        barrier_wait,
        in_run_live,
        between_invocations,
        drained_wait,
        exiting,
        exited,
    };


    void driver_main();
    bool stop_predicate_fn();
    static bool stop_predicate_trampoline(void* ctx);
    void recompute_task_set_terminal_locked();
    void close_resources();
    bool is_runtime_task() const noexcept;


    std::unique_ptr<AsyncIoContext> io_ctx_;
    std::unique_ptr<Scheduler> sched_;
    std::unique_ptr<Group> root_group_;
    SchedulerWakeHandle wake_handle_;


    unsigned worker_count_;


    mutable std::mutex lifecycle_mtx_;
    std::condition_variable runtime_cv_;
    State state_{State::Constructed};
    CloseState close_state_{CloseState::Open};
    DriverState driver_state_{DriverState::not_started};
    bool admission_open_{false};
    bool stop_requested_{false};
    bool startup_abort_requested_{false};
    bool root_cancel_published_{false};
    bool drain_complete_{false};
    bool driver_exit_requested_{false};
    std::uint64_t control_epoch_{0};
    std::uint64_t observed_epoch_{0};
    std::size_t admitted_count_{0};
    std::size_t terminal_count_{0};


    std::atomic<bool> fatal_snapshot_{false};
    std::atomic<bool> driver_exit_snapshot_{false};
    std::atomic<bool> task_set_terminal_snapshot_{true};
    std::atomic<bool> admission_closed_snapshot_{false};


    std::thread driver_thread_;
    bool driver_spawned_{false};








    static void set_current_fiber_tag(ApplicationRuntime* rt) noexcept;
    static ApplicationRuntime* current_fiber_tag() noexcept;

private:


#ifdef SLUICE_ASYNC_INTERNAL_TESTING
    std::promise<void> barrier_promise_;
    std::promise<void> commit_checkpoint_promise_;
    std::atomic<bool> commit_release_flag_{false};
    std::atomic<bool> test_pause_at_commit_checkpoint_{false};
#endif
};

}
