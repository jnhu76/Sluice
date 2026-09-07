
























































#pragma once

#include <atomic>
#include <cassert>

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/detail/select_port.hpp>
#include <sluice/async/scheduler.hpp>
#include <sluice/async/wait_node.hpp>
#include <sluice/async/wait_queue.hpp>

namespace sluice::async {







class Event {
public:


    explicit Event(Scheduler& scheduler, bool initially_set = false) noexcept
        : scheduler_(scheduler), set_(initially_set) {}












    ~Event() noexcept {
        const bool select_registry_empty = select_port_.empty();
        assert(select_registry_empty &&
               "Event destroyed with live Select arms — caller contract violation");
        if (!select_registry_empty) {
            detail::select_invariant_fail_fast();
        }
    }

    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) = delete;
    Event& operator=(Event&&) = delete;



    [[nodiscard]] bool is_set() const noexcept {
        return set_.load(std::memory_order::acquire);
    }






    void set() {
        scheduler_.event_set_broadcast(*this);
    }





    void reset() {
        scheduler_.event_reset(set_);
    }








    void wait(WaitNode& node) {
        scheduler_.await_event_wait(waiters_, set_, node);
    }

















    void wait_until(WaitNode& node, Scheduler::deadline_t deadline) {
        scheduler_.await_event_wait_deadline(waiters_, set_, node, deadline);
    }

























    [[nodiscard]] bool cancel(WaitNode& node) {
        return scheduler_.event_cancel_wait(waiters_, node);
    }

private:
    friend class Scheduler;

    Scheduler& scheduler_;
    std::atomic<bool> set_;
    WaitQueue waiters_;
    detail::SelectPort select_port_;
};

}
