#include <sluice/async/scheduler.hpp>

#include <sluice/async/async_rwlock.hpp>
#include <sluice/async/select.hpp>
#include <sluice/async/event.hpp>
#include <sluice/async/fiber_ctx.hpp>
#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/select_port.hpp>

#include "scheduler_internal.hpp"

#include <utility>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <type_traits>

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
#include "async_test_control_internal.hpp"
#endif

namespace sluice::async {
Scheduler::deadline_t Scheduler::monotonic_now() const noexcept {
    if (test_clock_mode_.load(std::memory_order::acquire)) {
        return clock_.load(std::memory_order::acquire);
    }
    auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch);
    return static_cast<deadline_t>(ms.count());
}

Scheduler::deadline_t Scheduler::clock_now_unlocked() const noexcept {
    if (test_clock_mode_) {
        return clock_.load(std::memory_order::acquire);
    }
    auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch);
    return static_cast<deadline_t>(ms.count());
}

void Scheduler::advance_clock(deadline_t t) {
    {
        LockGuard lk(global_mtx_);
        if (!test_clock_mode_)
            return;
        deadline_t cur = clock_.load(std::memory_order::acquire);
        if (t > cur)
            clock_.store(t, std::memory_order::release);

        (void)pump_deadlines_locked();
    }

    signal_wake_locked();
}

void Scheduler::await_wait_deadline(WaitQueue& q, WaitNode& node, deadline_t deadline) {
    WorkerState* ws = g_worker;
    Fiber* me = ws->current;
    TimerRegistration* reg = nullptr;
    {
        LockGuard lk(global_mtx_);
        LockGuard qlk(q.mtx());

        reg = prepare_ordinary_deadline_locked(&node, &q, deadline);
        if (!q.register_wait_locked(node, WaitResume::fiber(me))) {
            erase_popped_registration_locked(reg);
            return;
        }
        ++waiting_waitq_count_;

        publish_ordinary_deadline_locked(reg);

        if (clock_now_unlocked() >= deadline) {
            if (q.expire_locked(node)) {
                (void)consume_ordinary_deadline_locked(*reg);
                recompute_earliest_deadline_locked();
                if (waiting_waitq_count_ > 0)
                    --waiting_waitq_count_;

                return;
            }
        }

        if (node.is_terminal()) {
            q.unlink_locked(node);
            --waiting_waitq_count_;
            (void)retire_ordinary_deadline_locked(*reg);
            recompute_earliest_deadline_locked();
            return;
        }
        commit_suspend_locked(ws, me);
    }
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    sluice_async_test::test_phase(
        *this, sluice_async_test::PhaseTag::scheduler_suspend_before_physical_switch);
#endif
    fiber_ctx::Switch s;
    s.old = &me->ctx;
    s.new_ = &ws->sched_ctx;
    (void)fiber_ctx::context_switch(&s);
}

bool Scheduler::expire_wait(WaitQueue& q, WaitNode& node) {
    LockGuard lk(global_mtx_);
    LockGuard qlk(q.mtx());
    if (!q.expire_locked(node))
        return false;
    if (waiting_waitq_count_ > 0)
        --waiting_waitq_count_;

    const WaitResume& r = node.resume();
    if (r.kind() == WaitResume::Kind::fiber) {
        Fiber* f = r.as_fiber();
        if (f != nullptr) {
            if (publish_waiting_fiber_runnable_locked(f)) {
                return true;
            }
        }
        return false;
    }
    if (r.kind() == WaitResume::Kind::deferred) {
        defer_publication_locked(r.as_deferred());
        return true;
    }
    return false;
}

std::size_t Scheduler::pump_deadlines_locked() {
    std::size_t won = 0;
    const deadline_t now = clock_now_unlocked();
    while (!deadline_heap_.empty()) {
        const detail::DeadlineHeapEntry front = deadline_heap_.front();
        if (front.deadline > now)
            break;

        heap_pop_min_locked();
        if (front.kind == detail::DeadlineHeapEntry::Kind::select) {
            select_timer_pump_entry_locked(*front.target.select);
            erase_popped_select_registration_locked(front.target.select);
            continue;
        }
        TimerRegistration* top = front.target.ordinary;

        if (!consume_ordinary_deadline_locked(*top)) {
            erase_popped_registration_locked(top);
            continue;
        }

        WaitNode* n = top->node();
        WaitQueue* q = top->queue();

        if (n != nullptr && q != nullptr) {
            if (top->on_resolve_ == &rwlock_timer_expire_reconcile) {
                auto* ctx = static_cast<AsyncRwLock::ExpireCtx*>(top->owner_ctx_);
                if (ctx == nullptr) {
                    assert(false && "E12-F pump: RwLock timer with null ExpireCtx "
                                    "(Category B internal invariant violation)");
                    std::abort();
                }
                if (rwlock_expire_wait(*ctx->waiters, *ctx->active_readers, *ctx->writer_active,
                                       *ctx->writer_owner, *n)) {
                    ++won;
                }
                erase_popped_registration_locked(top);
                continue;
            }

            LockGuard qlk(q->mtx());
            if (q->expire_locked(*n)) {
                if (top->has_on_resolve()) {
                    auto* port = static_cast<detail::QueuePort*>(top->owner_ctx_);
                    if (port != nullptr && port->active_wait_associations_ > 0) {
                        --port->active_wait_associations_;
                    }
                    top->fire_on_resolve_locked(true);
                }
                if (waiting_waitq_count_ > 0)
                    --waiting_waitq_count_;

                const WaitResume& r = n->resume();
                if (r.kind() == WaitResume::Kind::fiber) {
                    Fiber* f = r.as_fiber();
                    if (f != nullptr && f->make_runnable()) {
                        WorkerState* owner = owner_for_fiber_locked(f);
                        route_runnable_locked(f, owner);
                        ++won;
                    }
                } else if (r.kind() == WaitResume::Kind::deferred) {
                    defer_publication_locked(r.as_deferred());
                    ++won;
                }
            }
        }
        erase_popped_registration_locked(top);
    }

    recompute_earliest_deadline_locked();
    return won;
}

static_assert(std::is_nothrow_copy_constructible_v<detail::DeadlineHeapEntry>,
              "publish_ordinary_deadline_locked noexcept relies on a "
              "trivially copyable deadline heap entry");
static_assert(std::is_nothrow_swappable_v<detail::DeadlineHeapEntry>,
              "heap sift swaps must stay nothrow under the admission publish");

TimerRegistration* Scheduler::prepare_ordinary_deadline_locked(WaitNode* node, WaitQueue* q,
                                                               deadline_t deadline) {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)

    if (sluice_async_test::ordinary_deadline_alloc_should_fail(*this)) {
        throw std::bad_alloc();
    }
#endif

    if (deadline_heap_.size() == deadline_heap_.capacity()) {
        const std::size_t cap = deadline_heap_.capacity();
        const std::size_t max_cap = deadline_heap_.max_size();
        std::size_t grown = cap < max_cap / 2 ? cap * 2 : max_cap;
        if (grown == 0)
            grown = 1;
        if (grown <= deadline_heap_.size()) {
            throw std::length_error("timed admission: deadline heap capacity overflow on reserve");
        }
        deadline_heap_.reserve(grown);
    }
    timer_pool_.emplace_back(node, q, deadline);
    return &timer_pool_.back();
}

void Scheduler::publish_ordinary_deadline_locked(TimerRegistration* reg,
                                                 TimerRegistration::OnResolveFn on_resolve,
                                                 void* owner_ctx) noexcept {
    reg->on_resolve_ = on_resolve;
    reg->owner_ctx_ = owner_ctx;
    ++active_deadline_count_;
    heap_push_ordinary_locked(reg);
    recompute_earliest_deadline_locked();
}

TimerRegistration*
Scheduler::arm_ordinary_deadline_locked(WaitNode* node, WaitQueue* q, deadline_t deadline,
                                        TimerRegistration::OnResolveFn on_resolve,
                                        void* owner_ctx) {
    TimerRegistration* reg = prepare_ordinary_deadline_locked(node, q, deadline);
    publish_ordinary_deadline_locked(reg, on_resolve, owner_ctx);
    return reg;
}

bool Scheduler::consume_ordinary_deadline_locked(TimerRegistration& reg) {
    if (!reg.try_claim_expiry())
        return false;
    --active_deadline_count_;
    return true;
}

bool Scheduler::retire_ordinary_deadline_locked(TimerRegistration& reg) {
    if (!reg.retire())
        return false;
    --active_deadline_count_;
    return true;
}

void Scheduler::retire_timer_for_node_locked(WaitNode& node) {
    for (auto& r : timer_pool_) {
        if (!r.is_active())
            continue;
        if (r.node() == &node) {
            if (retire_ordinary_deadline_locked(r)) {
                r.fire_on_resolve_locked(false);
            }
            recompute_earliest_deadline_locked();
            return;
        }
    }
}

bool Scheduler::any_active_deadline_locked() const {
    return active_deadline_count_ > 0;
}

bool Scheduler::earliest_active_deadline_locked(deadline_t& out) const {
    bool found = false;
    deadline_t best = 0;
    for (const auto& r : timer_pool_) {
        if (!r.is_active())
            continue;
        if (!found || r.deadline() < best) {
            best = r.deadline();
            found = true;
        }
    }
    for (const auto& r : select_timer_pool_) {
        if (!r.is_active())
            continue;
        if (!found || r.deadline() < best) {
            best = r.deadline();
            found = true;
        }
    }
    if (found)
        out = best;
    return found;
}

void Scheduler::recompute_earliest_deadline_locked() {
    deadline_t best = kNoDeadline;
    bool found = false;
    for (const auto& r : timer_pool_) {
        if (!r.is_active())
            continue;
        if (!found || r.deadline() < best) {
            best = r.deadline();
            found = true;
        }
    }
    for (const auto& r : select_timer_pool_) {
        if (!r.is_active())
            continue;
        if (!found || r.deadline() < best) {
            best = r.deadline();
            found = true;
        }
    }
    earliest_active_deadline_.store(found ? best : kNoDeadline, std::memory_order::release);
}

void Scheduler::erase_popped_registration_locked(TimerRegistration* r) {
    if (r == nullptr)
        return;
    for (auto it = timer_pool_.begin(); it != timer_pool_.end(); ++it) {
        if (&*it == r) {
            timer_pool_.erase(it);
            return;
        }
    }
}

TimerRegistration* Scheduler::register_test_deadline_locked(WaitNode* node, WaitQueue* q,
                                                            deadline_t deadline) {
    if (clock_now_unlocked() >= deadline)
        return nullptr;
    if (q != nullptr) {
        LockGuard qlk(q->mtx());
        if (!q->register_wait_locked(*node, WaitResume::none()))
            return nullptr;
    }
    ++waiting_waitq_count_;

    return arm_ordinary_deadline_locked(node, q, deadline);
}

void Scheduler::heap_push_entry_locked(const detail::DeadlineHeapEntry& e) {
    deadline_heap_.push_back(e);
    heap_sift_up_locked(deadline_heap_.size() - 1);
}

void Scheduler::heap_push_ordinary_locked(TimerRegistration* r) {
    heap_push_entry_locked(detail::DeadlineHeapEntry::for_ordinary(*r));
}

void Scheduler::heap_pop_min_locked() {
    if (deadline_heap_.empty())
        return;
    detail::DeadlineHeapEntry last = deadline_heap_.back();
    deadline_heap_.pop_back();
    if (!deadline_heap_.empty()) {
        deadline_heap_[0] = last;
        heap_sift_down_locked(0);
    }
}

void Scheduler::heap_sift_up_locked(std::size_t i) {
    while (i > 0) {
        std::size_t parent = (i - 1) / 2;
        if (!detail::heap_less_entry(deadline_heap_[i], deadline_heap_[parent]))
            break;
        std::swap(deadline_heap_[i], deadline_heap_[parent]);
        i = parent;
    }
}

void Scheduler::heap_sift_down_locked(std::size_t i) {
    const std::size_t n = deadline_heap_.size();
    while (true) {
        std::size_t l = 2 * i + 1;
        std::size_t r = 2 * i + 2;
        std::size_t best = i;
        if (l < n && detail::heap_less_entry(deadline_heap_[l], deadline_heap_[best]))
            best = l;
        if (r < n && detail::heap_less_entry(deadline_heap_[r], deadline_heap_[best]))
            best = r;
        if (best == i)
            break;
        std::swap(deadline_heap_[i], deadline_heap_[best]);
        i = best;
    }
}

} // namespace sluice::async
