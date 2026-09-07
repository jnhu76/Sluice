#include <sluice/async/group.hpp>

#include <sluice/async/detail/fail_fast.hpp>
#include <sluice/async/evented_wait_policy.hpp>
#include <sluice/async/scheduler.hpp>

namespace sluice::async {

bool Group::group_stop_predicate(void* ctx) {
    auto* self = static_cast<Group*>(ctx);
    std::lock_guard<std::mutex> lk(self->mtx_);
    for (auto& f : self->futures_) {
        if (!f->ready())
            return false;
    }
    return true;
}

Group::Group(Scheduler& sched) : sched_(&sched) {
    detail::require_evented_supported(detail::evented_admission_check());

    evented_policy_ = std::make_unique<EventedWaitPolicy>(sched);
}

void Group::await() {
    if (sched_) {
        while (true) {
            std::size_t pending = 0;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (auto& f : futures_) {
                    if (!f->ready())
                        ++pending;
                }
            }
            if (pending == 0)
                break;

            sched_->run_live(1, &group_stop_predicate, this);
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            bool all_terminal = true;
            for (auto& f : futures_) {
                if (!f->ready()) {
                    all_terminal = false;
                    break;
                }
            }
            if (all_terminal) {
                futures_.clear();
                evented_fibers_.clear();
                evented_stacks_.clear();
            }
        }
        return;
    }

    std::vector<std::thread> local_tasks;
    std::vector<std::shared_ptr<Future<void>>> local_futures;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        local_tasks.swap(tasks_);
        local_futures.swap(futures_);
    }

    for (auto& t : local_tasks)
        if (t.joinable())
            t.join();
    for (auto& f : local_futures)
        (void)f->await();
}

Group::~Group() {
    if (sched_) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& f : futures_) {
            if (!f->ready()) {
                detail::group_lifetime_fail_fast();
            }
        }

        futures_.clear();
        evented_fibers_.clear();
        evented_stacks_.clear();
        return;
    }

    std::vector<std::thread> local_tasks;
    std::vector<std::shared_ptr<Future<void>>> local_futures;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        local_tasks.swap(tasks_);
        local_futures.swap(futures_);
    }

    for (auto& t : local_tasks)
        if (t.joinable())
            t.join();
    for (auto& f : local_futures)
        (void)f->await();
}

} // namespace sluice::async
