// Implementation of Group (sluice-CORE-029, T3). See group.hpp for the model.
#include <sluice/async/group.hpp>

namespace sluice::async {

void Group::await() {
    // Snapshot the task list under the lock, then join + await outside the lock
    // (cannot join while holding our own mutex — a worker calling back into the
    // group would deadlock; and join can block). After this, tasks_ is empty.
    std::vector<std::thread> local_tasks;
    std::vector<std::shared_ptr<Future<void>>> local_futures;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        local_tasks.swap(tasks_);
        local_futures.swap(futures_);
    }
    // Await every future (each completes when its worker returns). Idempotent.
    for (auto& f : local_futures) {
        (void)f->await();
    }
    // Join every worker thread (RAII: they spawned in async(), we own them).
    for (auto& t : local_tasks) {
        if (t.joinable()) t.join();
    }
}

Group::~Group() {
    // If the caller never awaited, drain on destruction so worker threads do
    // not outlive the group (no detached threads — CP.26). This is await() if
    // not already done.
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

}  // namespace sluice::async
