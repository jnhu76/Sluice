// Implementation of Group (sluice-CORE-029 + E5-B). See group.hpp for the model.
//
// Two execution modes:
//   - Threaded (Group()): std::thread per task; await blocks + joins.
//   - Evented  (Group(Scheduler&)): Fiber per task on the scheduler; await
//     drives sched.run_until_idle() cooperatively until all task Futures ready.
#include <sluice/async/group.hpp>

#include <sluice/async/evented_wait_policy.hpp>
#include <sluice/async/scheduler.hpp>

namespace sluice::async {

Group::Group(Scheduler& sched) : sched_(&sched) {
    // Create the shared EventedWaitPolicy once per group. It borrows sched_,
    // which outlives the group by contract. All task Futures in this group
    // reference *evented_policy_.
    evented_policy_ = std::make_unique<EventedWaitPolicy>(sched);
}

void Group::await() {
    if (sched_) {
        // Evented: drive the scheduler cooperatively until every task Future is
        // ready. Does NOT block on a cv; does NOT join threads (none). Each
        // run_until_idle makes progress on runnable Fibers; if a task Future is
        // not yet ready, it is waiting on an Evented Future whose producer is
        // another Fiber in the same scheduler (in-scheduler production) or an
        // external producer the caller is responsible for staging between runs.
        while (true) {
            std::size_t pending = 0;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (auto& f : futures_) {
                    if (!f->ready()) ++pending;
                }
            }
            if (pending == 0) break;
            sched_->run_until_idle();
            // Re-check after driving. If no Future became ready AND the
            // scheduler has nothing runnable, an external producer must stage
            // more work; break to avoid an infinite loop. (E5 scope: producers
            // are in-scheduler Fibers or staged by the test between awaits.)
            std::size_t still_pending = 0;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (auto& f : futures_) {
                    if (!f->ready()) ++still_pending;
                }
            }
            (void)still_pending;  // the loop re-checks at the top
            // Single pass per call; the caller may re-invoke await() if it
            // staged external work. For purely in-scheduler producers this
            // loop terminates because the producer Fiber completes inside
            // run_until_idle.
            // Guard: if run_until_idle made no task Future ready, stop.
            std::size_t after_pending = 0;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (auto& f : futures_) {
                    if (!f->ready()) ++after_pending;
                }
            }
            if (after_pending == pending) break;  // no progress; caller stages
        }
        return;
    }

    // Threaded path (existing).
    std::vector<std::thread> local_tasks;
    std::vector<std::shared_ptr<Future<void>>> local_futures;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        local_tasks.swap(tasks_);
        local_futures.swap(futures_);
    }
    for (auto& f : local_futures) (void)f->await();
    for (auto& t : local_tasks) if (t.joinable()) t.join();
}

Group::~Group() {
    // Threaded: drain if await was never called (no detached threads, CP.26).
    std::vector<std::thread> local_tasks;
    std::vector<std::shared_ptr<Future<void>>> local_futures;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        local_tasks.swap(tasks_);
        local_futures.swap(futures_);
    }
    for (auto& f : local_futures) (void)f->await();
    for (auto& t : local_tasks) if (t.joinable()) t.join();
    // Evented: fibers/stacks are owned by the group and released by the vector
    // destructors. The borrowed Scheduler outlives the group by contract.
}

}  // namespace sluice::async
