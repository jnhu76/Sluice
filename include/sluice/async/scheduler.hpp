#pragma once

#include <sluice/async/async_io_context.hpp>
#include <sluice/async/completion.hpp>
#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/queue_port.hpp>
#include <sluice/async/detail/ready_sink.hpp>
#include <sluice/async/detail/select_registration.hpp>
#include <sluice/async/select_fwd.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/lock_guard.hpp>
#include <sluice/async/mutex.hpp>
#include <sluice/async/thread_annotations.hpp>
#include <sluice/async/timer_registration.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sluice::async {

class Event;
class SelectResult;
class ApplicationRuntime;

namespace detail {
class SelectGroup;
class SelectPort;
struct SelectArmSlot;
enum class ArmState : std::uint8_t;
class SelectCaseDescriptor;
} // namespace detail

class SchedulerWakeHandle {
  public:
    SchedulerWakeHandle() = default;

    SchedulerWakeHandle(const SchedulerWakeHandle&) = delete;
    SchedulerWakeHandle& operator=(const SchedulerWakeHandle&) = delete;
    SchedulerWakeHandle(SchedulerWakeHandle&&) noexcept = default;
    SchedulerWakeHandle& operator=(SchedulerWakeHandle&&) noexcept = default;

    bool notify() noexcept;

    bool bound() const noexcept;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    void lifetime_seam_arm() noexcept;
    void lifetime_seam_wait_paused() noexcept;
    bool lifetime_seam_is_paused() const noexcept;
    void lifetime_seam_release() noexcept;
#endif

  private:
    friend class Scheduler;
    struct Control;
    explicit SchedulerWakeHandle(std::shared_ptr<Control> ctrl) noexcept
        : control_(std::move(ctrl)) {}
    std::shared_ptr<Control> control_;
};

enum class RunMode : unsigned char {
    drain,
    live,
};

struct WorkerState {
    fiber_ctx::Context sched_ctx{};

    std::atomic<Fiber*> current{nullptr};
    std::deque<Fiber*> local_runnable{};
    unsigned id = 0;

    std::mutex inbox_mtx;
    std::atomic<bool> active{false};

    std::atomic<bool> suspend_switch_pending{false};

    std::uint64_t observed_epoch{0};

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    enum class LoopExitReason : unsigned char {
        live,
        mw_s1_terminate_observed,
        mw_s2_no_progress_terminate,
        e14f1_last_idle_terminate,
        last_idle_terminate,
        final_park_terminate,
    };
    std::atomic<LoopExitReason> loop_exit_reason{LoopExitReason::live};

    std::atomic<bool> loop_exited{false};

    std::atomic<int> last_classify{-1};
    std::atomic<std::uint64_t> classify_seq{0};
#endif
    enum class ParkDomain : unsigned char { None, Scheduler, Backend };
    std::atomic<ParkDomain> park_domain{ParkDomain::None};

    std::atomic<unsigned> idle_dance_contributed_{0};

    std::atomic<std::uint64_t> dance_epoch_at_contribution_{0};

    Scheduler* owner_scheduler{nullptr};

    WorkerState() = default;
    WorkerState(const WorkerState&) = delete;
    WorkerState& operator=(const WorkerState&) = delete;
};

class AsyncRwLock;

class Scheduler {
  public:
    explicit Scheduler(AsyncIoContext& ctx, std::size_t wait_capacity = 256);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    bool init_fiber(Fiber& fiber, std::byte* stack_base, std::size_t stack_size);

    void spawn(Fiber& fiber) noexcept;

    void spawn_on(Fiber& fiber, unsigned worker_id) noexcept;

    void run(unsigned worker_count);

    void run_live(unsigned worker_count);

    void run_live(unsigned worker_count, bool (*stop_fn)(void*), void* stop_ctx);

    void run_until_idle() { run(1); }

    Result<void> await_completion_size(Completion<std::size_t>& c);
    Result<void> await_completion_void(Completion<void>& c);
    void await_ready_flag(const std::atomic<bool>& ready);

    Result<bool> cancel_waiter(Completion<std::size_t>& c);
    Result<bool> cancel_waiter(Completion<void>& c);

    void await_wait(WaitQueue& q, WaitNode& node);

    bool wake_wait_one(WaitQueue& q);

    bool cancel_wait(WaitQueue& q, WaitNode& node);

    using deadline_t = deadline_tick_t;

    deadline_t monotonic_now() const noexcept;

    void advance_clock(deadline_t t);

    void await_wait_deadline(WaitQueue& q, WaitNode& node, deadline_t deadline);

    bool expire_wait(WaitQueue& q, WaitNode& node);

    std::size_t event_set_broadcast(Event& event);

    void event_reset(std::atomic<bool>& set_flag);

    void await_event_wait(WaitQueue& q, const std::atomic<bool>& set_flag, WaitNode& node);

    void await_event_wait_deadline(WaitQueue& q, const std::atomic<bool>& set_flag, WaitNode& node,
                                   deadline_t deadline);

    bool event_cancel_wait(WaitQueue& q, WaitNode& node);

    enum class WaitAdmitDisposition : std::uint8_t {
        rejected,
        resolved_inline,

        authorized,
    };
    WaitAdmitDisposition event_wait_admit_locked(WaitQueue& q, const std::atomic<bool>& set_flag,
                                                 WaitNode& node, const WaitResume& resume,
                                                 bool timed, deadline_t deadline)
        SLUICE_REQUIRES(global_mtx_, q.mtx());

    [[nodiscard]] bool sem_try_acquire(WaitQueue& waiters, std::atomic<std::uint32_t>& available);

    void sem_acquire(WaitQueue& waiters, std::atomic<std::uint32_t>& available, WaitNode& node);

    void sem_acquire_until(WaitQueue& waiters, std::atomic<std::uint32_t>& available,
                           WaitNode& node, deadline_t deadline);

    [[nodiscard]] bool sem_cancel(WaitQueue& waiters, WaitNode& node);

    [[nodiscard]] bool sem_release(WaitQueue& waiters, std::atomic<std::uint32_t>& available,
                                   std::uint32_t max_permits);

    [[nodiscard]] bool mutex_try_lock(WaitQueue& waiters, Fiber*& owner);

    void mutex_lock(WaitQueue& waiters, Fiber*& owner, WaitNode& node);

    void mutex_lock_until(WaitQueue& waiters, Fiber*& owner, WaitNode& node, deadline_t deadline);

    [[nodiscard]] bool mutex_cancel(WaitQueue& waiters, WaitNode& node);

    void mutex_unlock(WaitQueue& waiters, Fiber*& owner);

    WaitNode* mutex_handoff_one_locked(WaitQueue& waiters, Fiber*& owner)
        SLUICE_REQUIRES(global_mtx_);

    WaitOutcome condition_wait_prepare(WaitQueue& cond_waiters, WaitNode& cond_node,
                                       WaitQueue& mutex_waiters, Fiber*& owner,
                                       bool& released_mutex);

    enum class ConditionAdmitDisposition : std::uint8_t {
        rejected_retain = 0,
        resolved_inline_retain = 1,
        resolved_inline_released = 2,
        authorized = 3,
    };
    ConditionAdmitDisposition
    condition_wait_admit_locked(WaitQueue& cond_waiters, WaitNode& cond_node,
                                const WaitResume& resume, WaitQueue& mutex_waiters, Fiber*& owner,
                                bool timed, deadline_t deadline) SLUICE_REQUIRES(global_mtx_);

    WaitOutcome condition_wait_prepare_until(WaitQueue& cond_waiters, WaitNode& cond_node,
                                             WaitQueue& mutex_waiters, Fiber*& owner,
                                             deadline_t deadline, bool& released_mutex);

    void condition_notify_one(WaitQueue& cond_waiters);

    std::size_t condition_notify_all(WaitQueue& cond_waiters);

    [[nodiscard]] bool condition_cancel_wait(WaitQueue& cond_waiters, WaitNode& cond_node);

    void queue_push_admit(detail::QueuePort& port, WaitNode& node, detail::QueueItemLease& lease);

    void queue_pop_admit(detail::QueuePort& port, WaitNode& node, detail::QueueItemLease& out);

    void queue_push_admit_until(detail::QueuePort& port, WaitNode& node,
                                detail::QueueItemLease& lease, deadline_t deadline);
    void queue_pop_admit_until(detail::QueuePort& port, WaitNode& node, detail::QueueItemLease& out,
                               deadline_t deadline);

    [[nodiscard]] bool queue_cancel(detail::QueuePort& port, detail::QueueRole role,
                                    WaitNode& node);

    enum class QueueAdmitDisposition : std::uint8_t {
        rejected,
        resolved_inline,

        resolved_inline_grant,

        authorized,
    };
    QueueAdmitDisposition
    queue_push_admit_locked(detail::QueuePort& port, detail::QueueItemLease& lease, WaitNode& node,
                            const WaitResume& resume, bool timed, deadline_t deadline)
        SLUICE_REQUIRES(global_mtx_, port.state_mtx_, port.waiters_[0].mtx());
    QueueAdmitDisposition
    queue_pop_admit_locked(detail::QueuePort& port, detail::QueueItemLease& out, WaitNode& node,
                           const WaitResume& resume, bool timed, deadline_t deadline)
        SLUICE_REQUIRES(global_mtx_, port.state_mtx_, port.waiters_[1].mtx());

    void queue_publish_winner_locked(detail::QueuePort& port, WaitNode& won)
        SLUICE_REQUIRES(global_mtx_);

    WaitNode* queue_grant_consumer_locked(detail::QueuePort& port);

    WaitNode* queue_grant_producer_locked(detail::QueuePort& port);

    bool queue_role_waiters_empty_locked(detail::QueuePort& port) SLUICE_REQUIRES(global_mtx_);

    static void queue_timer_on_resolve(void* owner_ctx, bool timer_won) noexcept;

    [[nodiscard]] bool rwlock_try_read_lock(WaitQueue& waiters, std::size_t& active_readers,
                                            bool& writer_active);

    void rwlock_read_lock(WaitQueue& waiters, std::size_t& active_readers, bool& writer_active,
                          WaitNode& node);

    void rwlock_read_lock_until(WaitQueue& waiters, std::size_t& active_readers,
                                bool& writer_active, WaitNode& node, deadline_t deadline,
                                void* expire_ctx);

    [[nodiscard]] bool rwlock_try_write_lock(WaitQueue& waiters, std::size_t& active_readers,
                                             bool& writer_active, ActorId& writer_owner);

    void rwlock_write_lock(WaitQueue& waiters, std::size_t& active_readers, bool& writer_active,
                           ActorId& writer_owner, WaitNode& node);

    void rwlock_write_lock_until(WaitQueue& waiters, std::size_t& active_readers,
                                 bool& writer_active, ActorId& writer_owner, WaitNode& node,
                                 deadline_t deadline, void* expire_ctx);

    void rwlock_unlock_read(WaitQueue& waiters, std::size_t& active_readers, bool& writer_active,
                            ActorId& writer_owner);

    void rwlock_unlock_write(WaitQueue& waiters, std::size_t& active_readers, bool& writer_active,
                             ActorId& writer_owner);

    [[nodiscard]] bool rwlock_cancel(WaitQueue& waiters, std::size_t& active_readers,
                                     bool& writer_active, ActorId& writer_owner, WaitNode& node);

    bool rwlock_expire_wait(WaitQueue& waiters, std::size_t& active_readers, bool& writer_active,
                            ActorId& writer_owner, WaitNode& node) SLUICE_REQUIRES(global_mtx_);

    WaitAdmitDisposition rwlock_read_admit_locked(WaitQueue& waiters, std::size_t& active_readers,
                                                  bool& writer_active, WaitNode& node,
                                                  const WaitResume& resume, bool timed,
                                                  deadline_t deadline, void* expire_ctx)
        SLUICE_REQUIRES(global_mtx_, waiters.mtx());
    WaitAdmitDisposition rwlock_write_admit_locked(WaitQueue& waiters, std::size_t& active_readers,
                                                   bool& writer_active, ActorId& writer_owner,
                                                   WaitNode& node, const WaitResume& resume,
                                                   const ActorId& actor, bool timed,
                                                   deadline_t deadline, void* expire_ctx)
        SLUICE_REQUIRES(global_mtx_, waiters.mtx());

    bool rwlock_try_write_admission_locked(WaitQueue& waiters, std::size_t& active_readers,
                                           bool& writer_active, ActorId& writer_owner,
                                           const ActorId& caller)
        SLUICE_REQUIRES(global_mtx_, waiters.mtx());

    void rwlock_unlock_write_core_locked(WaitQueue& waiters, std::size_t& active_readers,
                                         bool& writer_active, ActorId& writer_owner,
                                         const ActorId& caller) SLUICE_REQUIRES(global_mtx_);

    SchedulerWakeHandle make_wake_handle() noexcept;

    void attach_ready_wake(const std::atomic<bool>& ready, SchedulerWakeHandle& wh);

    std::size_t runnable_count() const;
    std::size_t waiting_count() const {
        LockGuard lk(global_mtx_);

        LockGuard rlk(wait_registry_mtx_);
        return wait_record_live_count_ + waiting_size_.size() + waiting_void_.size() +
               waiting_ready_.size() + waiting_waitq_count_;
    }
    std::size_t waiting_ready_count() const {
        LockGuard lk(global_mtx_);
        return waiting_ready_.size();
    }

    static unsigned current_worker_id() {
        WorkerState* w = current_worker();
        return w ? w->id : static_cast<unsigned>(-1);
    }

    static void* current_fiber_execution_tag() {
        WorkerState* w = current_worker();
        Fiber* cur = w ? w->current.load(std::memory_order_acquire) : nullptr;
        return cur ? cur->execution_tag() : nullptr;
    }

  private:
    friend class ApplicationRuntime;
    static void set_current_fiber_execution_tag(void* tag) {
        WorkerState* w = current_worker();
        if (w) {
            if (Fiber* cur = w->current.load(std::memory_order_acquire)) {
                cur->set_execution_tag(tag);
            }
        }
    }

  public:
    WorkerState* owner_of(const Fiber& f) const {
        LockGuard lk(global_mtx_);
        auto it = fiber_owner_.find(const_cast<Fiber*>(&f));
        return it == fiber_owner_.end() ? nullptr : it->second;
    }
    unsigned owner_id_of(const Fiber& f) const {
        WorkerState* o = owner_of(f);
        return o ? o->id : static_cast<unsigned>(-1);
    }

  private:
    friend class SchedulerWakeHandle;

    friend class ::sluice::async::detail::QueuePort;

    template <class... Cases>
        requires(sizeof...(Cases) >= 1 && sizeof...(Cases) <= kSelectMaxArms &&
                 (SelectCaseType<Cases> && ...))
    friend SelectResult select(Scheduler& scheduler, Cases&&... cases);

    void select_event_link_locked(Event& event, detail::SelectArmSlot& arm)
        SLUICE_REQUIRES(global_mtx_);

    void select_event_unlink_locked(Event& event, detail::SelectArmSlot& arm)
        SLUICE_REQUIRES(global_mtx_);

    std::size_t select_event_scan_locked(Event& event) SLUICE_REQUIRES(global_mtx_);

    struct WaitReg {
        Fiber* fiber;
        WorkerState* owner;
    };

    enum class WaitRecordState : std::uint8_t {
        free,
        registered,
        delivered,
        cancelled,
    };

    struct WaitRecord {
        std::uint32_t index = 0;
        std::uint32_t generation = 0;
        WaitRecordState state = WaitRecordState::free;
        Fiber* fiber = nullptr;
        WorkerState* owner = nullptr;
        const void* completion = nullptr;
        WaitRecord* next_delivered = nullptr;
        WaitRecord* next_free = nullptr;
    };

    class ReadyRoutingSink final : public detail::SynchronousReadySink {
      public:
        explicit ReadyRoutingSink(Scheduler* scheduler) noexcept : scheduler_(scheduler) {}
        void on_ready(detail::ReadyEvent event) noexcept override;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

        std::size_t deliveries() const noexcept { return deliveries_; }
        std::size_t routed() const noexcept { return routed_; }
        std::size_t stale_dropped() const noexcept { return stale_dropped_; }
        std::size_t cancel_lost() const noexcept { return cancel_lost_; }
#endif

      private:
        Scheduler* scheduler_;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        std::size_t deliveries_ = 0;
        std::size_t routed_ = 0;
        std::size_t stale_dropped_ = 0;
        std::size_t cancel_lost_ = 0;
#endif
    };

    bool drain_routed_completion_waits_locked() SLUICE_REQUIRES(global_mtx_);

    WaitRecord* acquire_wait_record_locked(Fiber* fiber, WorkerState* owner, const void* completion,
                                           std::uint64_t& lease_id_out)
        SLUICE_REQUIRES(global_mtx_);
    void retire_wait_record_locked(std::uint32_t index) SLUICE_REQUIRES(global_mtx_);
    std::size_t wait_record_live_count_locked() const SLUICE_REQUIRES(global_mtx_);

    enum class MwState {
        mw_s1,
        mw_s2,
        mw_s3_unresolved,
        quiescent,
    };

    enum class AdmissionState : unsigned {
        none,
        candidate,
        committed,
    };

    bool wake_ready_flags_locked() SLUICE_REQUIRES(global_mtx_);
    void route_runnable(Fiber* f, WorkerState* owner) SLUICE_REQUIRES(global_mtx_);
    void route_runnable_locked(Fiber* f, WorkerState* owner) SLUICE_REQUIRES(global_mtx_);

    WorkerState* owner_for_fiber_locked(Fiber* fiber) SLUICE_REQUIRES(global_mtx_);

    bool publish_waiting_fiber_runnable_locked(Fiber* fiber) SLUICE_REQUIRES(global_mtx_);

    void publish_wait_winner_locked(WaitNode& won) SLUICE_REQUIRES(global_mtx_);

    void defer_publication_locked(void* delivery_record) noexcept SLUICE_REQUIRES(global_mtx_);

    std::size_t take_deferred_publications(void** out, std::size_t cap);

    std::vector<void*> deferred_publications_ SLUICE_GUARDED_BY(global_mtx_){};

    bool cancel_primitive_wait_locked(WaitQueue& waiters, WaitNode& node)
        SLUICE_REQUIRES(global_mtx_, waiters.mtx());

    WaitNode* wake_wait_one_locked(WaitQueue& q) SLUICE_REQUIRES(global_mtx_);

    void rwlock_grant_from_head_locked(WaitQueue& waiters, std::size_t& active_readers,
                                       bool& writer_active, ActorId& writer_owner)
        SLUICE_REQUIRES(global_mtx_);

    bool rwlock_claim_node_woken_locked(WaitQueue& waiters, WaitNode& node)
        SLUICE_REQUIRES(global_mtx_, waiters.mtx_);

    static void rwlock_timer_expire_reconcile(void* owner_ctx, bool timer_won) noexcept;

    using WorkerSnapshot = std::vector<WorkerState*>;

    void ensure_workers_locked(unsigned worker_count, WorkerSnapshot& run_workers)
        SLUICE_REQUIRES(global_mtx_);

    void worker_loop(WorkerState* ws, const WorkerSnapshot& run_workers);

    void run_impl(unsigned worker_count, RunMode mode);
    void run_next_on(WorkerState* ws, Fiber* fiber);

    void commit_suspend_locked(WorkerState* ws, Fiber* fiber) SLUICE_REQUIRES(global_mtx_);

    bool try_steal(WorkerState* thief, const WorkerSnapshot& run_workers);

    MwState classify_locked(const WorkerSnapshot& run_workers,
                            WorkerState* classify_ws = nullptr) const SLUICE_REQUIRES(global_mtx_);

    MwState classify_locked_impl(const WorkerSnapshot& run_workers) const
        SLUICE_REQUIRES(global_mtx_);

    bool unguarded_progress_pending_locked() const SLUICE_REQUIRES(global_mtx_);

    static WorkerState* current_worker();

    AsyncIoContext& ctx_;

    mutable Mutex global_mtx_;

    std::unordered_map<void*, WaitReg> waiting_size_ SLUICE_GUARDED_BY(global_mtx_){};
    std::unordered_map<void*, WaitReg> waiting_void_ SLUICE_GUARDED_BY(global_mtx_){};
    std::unordered_map<const std::atomic<bool>*, WaitReg>
        waiting_ready_ SLUICE_GUARDED_BY(global_mtx_){};

    std::size_t waiting_waitq_count_ SLUICE_GUARDED_BY(global_mtx_){0};

    mutable Mutex wait_registry_mtx_;

    const std::size_t wait_capacity_ SLUICE_GUARDED_BY(wait_registry_mtx_){0};
    std::vector<std::unique_ptr<WaitRecord>> wait_records_ SLUICE_GUARDED_BY(wait_registry_mtx_);

    WaitRecord* wait_record_free_head_ SLUICE_GUARDED_BY(wait_registry_mtx_) = nullptr;
    WaitRecord* wait_delivered_head_ SLUICE_GUARDED_BY(wait_registry_mtx_) = nullptr;

    std::size_t wait_record_live_count_ SLUICE_GUARDED_BY(wait_registry_mtx_){0};

    std::uint64_t wait_lease_serial_ SLUICE_GUARDED_BY(wait_registry_mtx_){1};

    const std::uint64_t scheduler_identity_;

    ReadyRoutingSink ready_sink_{this};

    std::size_t waiting_select_count_ SLUICE_GUARDED_BY(global_mtx_){0};

    std::unordered_map<Fiber*, WorkerState*> fiber_owner_ SLUICE_GUARDED_BY(global_mtx_){};

    std::deque<Fiber*> pending_spawn_ SLUICE_GUARDED_BY(global_mtx_){};
    unsigned next_spawn_worker_ SLUICE_GUARDED_BY(global_mtx_) = 0;

    std::vector<std::unique_ptr<WorkerState>> workers_ SLUICE_GUARDED_BY(global_mtx_);

    std::atomic<unsigned> active_worker_count_{0};
    std::atomic<unsigned> running_fiber_count_{0};
    std::atomic<unsigned> idle_workers_{0};

    std::atomic<std::uint64_t> dance_epoch_{0};
    std::atomic<bool> global_terminate_{false};
    std::condition_variable global_idle_cv_;
    bool in_coordinated_run_ = false;

    unsigned live_loop_workers_ SLUICE_GUARDED_BY(global_mtx_) = 0;

    RunMode run_mode_{RunMode::drain};

    bool (*invocation_stop_fn_)(void*){nullptr};
    void* invocation_stop_ctx_{nullptr};

    std::atomic<bool> force_init_fiber_fail_{false};

    AdmissionState admission_ SLUICE_GUARDED_BY(global_mtx_){AdmissionState::none};
    unsigned admission_owner_ SLUICE_GUARDED_BY(global_mtx_) = static_cast<unsigned>(-1);

    Mutex wake_mtx_;
    std::condition_variable_any wake_cv_;
    std::uint64_t wake_epoch_ SLUICE_GUARDED_BY(wake_mtx_){0};

    std::shared_ptr<SchedulerWakeHandle::Control> wake_control_;

    std::atomic<bool> backend_wait_active_{false};

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    struct ParkLedgerRecord {
        std::uint64_t park_seq = 0;
        unsigned worker_id = 0;
        std::uint64_t epoch_at_commit = 0;
        std::uint64_t ready_generation = 0;
        std::uint64_t control_generation = 0;
        std::size_t backend_outstanding = 0;
        std::size_t waiting_registered = 0;
        unsigned idle_workers = 0;
        bool backend_wait_active = false;
        bool ready_flag_bounded = false;
        bool global_terminate = false;
        bool external_wake_possible = false;
        int last_classify = -1;
        std::uint64_t classify_seq = 0;
    };
    static constexpr std::size_t kParkLedgerCapacity = 64;

    mutable std::atomic<bool> park_forensics_enabled_{false};

    mutable std::mutex park_ledger_mtx_;
    ParkLedgerRecord park_ledger_[kParkLedgerCapacity]{};
    std::size_t park_ledger_count_{0};
    std::size_t park_ledger_next_{0};
    std::uint64_t park_ledger_total_{0};

    void dump_park_forensics_for_test(const char* tag);
#endif

    void signal_wake_locked();

    void notify_external_wake() noexcept;

    void park_on_wake_source(WorkerState* ws,
                             bool bounded_backend_observation) SLUICE_NO_THREAD_SAFETY_ANALYSIS;

    std::list<TimerRegistration> timer_pool_ SLUICE_GUARDED_BY(global_mtx_){};

    std::vector<detail::DeadlineHeapEntry> deadline_heap_ SLUICE_GUARDED_BY(global_mtx_){};

    std::list<detail::SelectTimerRegistration> select_timer_pool_ SLUICE_GUARDED_BY(global_mtx_){};

    std::size_t active_deadline_count_ SLUICE_GUARDED_BY(global_mtx_){0};

    std::atomic<deadline_t> clock_{0};
    std::atomic<bool> test_clock_mode_{false};

    static constexpr deadline_t kNoDeadline = static_cast<deadline_t>(-1);
    std::atomic<deadline_t> earliest_active_deadline_{kNoDeadline};

    deadline_t clock_now_unlocked() const noexcept;

    void recompute_earliest_deadline_locked() SLUICE_REQUIRES(global_mtx_);

    bool earliest_active_deadline_locked(deadline_t& out) const SLUICE_REQUIRES(global_mtx_);

    std::size_t pump_deadlines_locked() SLUICE_REQUIRES(global_mtx_);

    void heap_push_entry_locked(const detail::DeadlineHeapEntry& e) SLUICE_REQUIRES(global_mtx_);

    void heap_push_ordinary_locked(TimerRegistration* r) SLUICE_REQUIRES(global_mtx_);
    void heap_pop_min_locked() SLUICE_REQUIRES(global_mtx_);
    void heap_sift_up_locked(std::size_t i) SLUICE_REQUIRES(global_mtx_);
    void heap_sift_down_locked(std::size_t i) SLUICE_REQUIRES(global_mtx_);

    void erase_popped_registration_locked(TimerRegistration* r) SLUICE_REQUIRES(global_mtx_);

    TimerRegistration* prepare_ordinary_deadline_locked(WaitNode* node, WaitQueue* q,
                                                        deadline_t deadline)
        SLUICE_REQUIRES(global_mtx_);

    void publish_ordinary_deadline_locked(TimerRegistration* reg,
                                          TimerRegistration::OnResolveFn on_resolve = nullptr,
                                          void* owner_ctx = nullptr) noexcept
        SLUICE_REQUIRES(global_mtx_);

    TimerRegistration*
    arm_ordinary_deadline_locked(WaitNode* node, WaitQueue* q, deadline_t deadline,
                                 TimerRegistration::OnResolveFn on_resolve = nullptr,
                                 void* owner_ctx = nullptr) SLUICE_REQUIRES(global_mtx_);

    bool consume_ordinary_deadline_locked(TimerRegistration& reg) SLUICE_REQUIRES(global_mtx_);

    bool retire_ordinary_deadline_locked(TimerRegistration& reg) SLUICE_REQUIRES(global_mtx_);

    detail::SelectTimerRegistration*
    select_timer_splice_one_locked(std::list<detail::SelectTimerRegistration>& tmp_pool,
                                   std::list<detail::SelectTimerRegistration>::iterator it)
        SLUICE_REQUIRES(global_mtx_);

    bool select_timer_retire_locked(detail::SelectTimerRegistration& reg)
        SLUICE_REQUIRES(global_mtx_);

    bool select_timer_consume_locked(detail::SelectTimerRegistration& reg)
        SLUICE_REQUIRES(global_mtx_);

    void erase_popped_select_registration_locked(detail::SelectTimerRegistration* r)
        SLUICE_REQUIRES(global_mtx_);

    bool pool_owns_select_block_locked(const detail::SelectTimerRegistration& reg) const noexcept
        SLUICE_REQUIRES(global_mtx_);

    bool select_timer_pump_entry_locked(detail::SelectTimerRegistration& reg)
        SLUICE_REQUIRES(global_mtx_);

    bool select_process_group_locked(detail::SelectGroup& group, std::uint32_t candidate_index)
        SLUICE_REQUIRES(global_mtx_);

    void select_preflight_shape_locked(detail::SelectGroup& group,
                                       std::uint32_t candidate_index) const
        SLUICE_REQUIRES(global_mtx_);
    void select_preflight_claim_locked(detail::SelectGroup& group,
                                       std::uint32_t candidate_index) const
        SLUICE_REQUIRES(global_mtx_);

    void select_commit_winner_locked(detail::SelectGroup& group, std::uint32_t winner_index)
        SLUICE_REQUIRES(global_mtx_);

    void select_finalize_loser_locked(detail::SelectGroup& group, std::uint32_t loser_index)
        SLUICE_REQUIRES(global_mtx_);

    void select_finalize_event_winner_locked(detail::SelectGroup& group, detail::SelectArmSlot& arm)
        SLUICE_REQUIRES(global_mtx_);
    void select_finalize_event_loser_locked(detail::SelectGroup& group, detail::SelectArmSlot& arm)
        SLUICE_REQUIRES(global_mtx_);
    void select_finalize_timer_winner_locked(detail::SelectGroup& group, detail::SelectArmSlot& arm)
        SLUICE_REQUIRES(global_mtx_);
    void select_finalize_timer_loser_locked(detail::SelectGroup& group, detail::SelectArmSlot& arm)
        SLUICE_REQUIRES(global_mtx_);

    bool select_all_authority_closed_locked(const detail::SelectGroup& group) const
        SLUICE_REQUIRES(global_mtx_);

    SelectResult select_admit(detail::SelectCaseDescriptor* descs, std::size_t count);

    void select_begin_rollback_locked(detail::SelectGroup& group) noexcept
        SLUICE_REQUIRES(global_mtx_);

    void select_rollback_arm_locked(detail::SelectGroup& group, detail::SelectArmSlot& arm) noexcept
        SLUICE_REQUIRES(global_mtx_);

    void select_finish_rollback_locked(detail::SelectGroup& group, detail::SelectArmSlot* arms,
                                       std::size_t arm_count, std::size_t registered_count) noexcept
        SLUICE_REQUIRES(global_mtx_);

    void select_rollback_registration_locked(detail::SelectGroup& group,
                                             detail::SelectArmSlot* arms, std::size_t arm_count,
                                             std::size_t registered_count) noexcept
        SLUICE_REQUIRES(global_mtx_);

    void select_publish_locked(detail::SelectGroup& group) SLUICE_REQUIRES(global_mtx_);

    bool select_resolve_event_locked(Event& event) SLUICE_REQUIRES(global_mtx_);

    bool select_resolve_timer_locked(detail::SelectTimerRegistration& reg)
        SLUICE_REQUIRES(global_mtx_);

    TimerRegistration* register_test_deadline_locked(WaitNode* node, WaitQueue* q,
                                                     deadline_t deadline)
        SLUICE_REQUIRES(global_mtx_);

    bool any_active_deadline_locked() const SLUICE_REQUIRES(global_mtx_);

    void retire_timer_for_node_locked(WaitNode& node) SLUICE_REQUIRES(global_mtx_);

    bool external_wake_possible_locked() const SLUICE_REQUIRES(global_mtx_) {
        return !waiting_ready_.empty() || waiting_waitq_count_ > 0 ||
               any_active_deadline_locked() || waiting_select_count_ > 0;
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
  public:
    std::size_t select_timer_arm_load_count_{0};

    struct AsyncTestAccess;
#endif
};

} // namespace sluice::async

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

#include "scheduler_test_access.hpp"
#endif
