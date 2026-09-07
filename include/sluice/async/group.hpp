






















#pragma once

#include <sluice/async/cancel.hpp>
#include <sluice/async/fiber.hpp>
#include <sluice/async/future.hpp>
#include <sluice/result.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace sluice::async {














class Scheduler;

class Group {
public:


    Group() = default;
















    explicit Group(Scheduler& sched);

    ~Group();

    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;
    Group(Group&&) = delete;
    Group& operator=(Group&&) = delete;







    template <class Fn>
    void async(Fn fn) {
        if (sched_) {
            async_evented<Fn>(std::move(fn));
        } else {
            async_threaded<Fn>(std::move(fn));
        }
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)





    void test_set_tasks_throw_on_nth(std::size_t n) { tasks_throw_on_nth_ = n; }
    std::size_t test_tasks_throw_on_nth() const { return tasks_throw_on_nth_; }








    enum class EventedAdmissionFailPoint {
        none,
        before_fiber_storage_reserve,
        before_stack_storage_reserve,
        before_future_storage_reserve,
    };
    void test_set_evented_admission_fail(EventedAdmissionFailPoint fp) {
        evented_fail_point_ = fp;
    }
    EventedAdmissionFailPoint test_evented_admission_fail() const {
        return evented_fail_point_;
    }



    struct EventedStorageSnapshot {
        std::size_t fibers;
        std::size_t stacks;
        std::size_t futures;
    };
    EventedStorageSnapshot test_evented_storage_snapshot() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return {evented_fibers_.size(), evented_stacks_.size(), futures_.size()};
    }
#endif



    CancelToken& group_token() noexcept { return token_; }






    void await();



    void cancel() {
        token_.request();
        await();
    }



    std::size_t size() const noexcept {
        std::lock_guard<std::mutex> lk(mtx_);
        return futures_.size();
    }

private:



    static bool group_stop_predicate(void* ctx);













    template <class Fn>
    void async_threaded(Fn fn) {
        auto fut = std::make_shared<Future<void>>();




        {
            std::lock_guard<std::mutex> lk(mtx_);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
            if (tasks_throw_on_nth_ > 0 && (tasks_.size() + 1) == tasks_throw_on_nth_) {
                throw std::bad_alloc();
            }
#endif
            tasks_.reserve(tasks_.size() + 1);
            futures_.reserve(futures_.size() + 1);
        }


        std::thread w([fut, fn = std::move(fn), tok = &token_]() mutable {
            try {
                fn(*tok);
            } catch (...) {


            }
            fut->complete_with(sluice::Result<void>{});
        });


        {
            std::lock_guard<std::mutex> lk(mtx_);
            tasks_.push_back(std::move(w));
            futures_.push_back(std::move(fut));
        }
    }




    template <class Fn>
    void async_evented(Fn fn);

    mutable std::mutex mtx_;
    std::vector<std::thread> tasks_;
    std::vector<std::shared_ptr<Future<void>>> futures_;
    CancelToken token_;
    Scheduler* sched_ = nullptr;
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)



    std::size_t tasks_throw_on_nth_ = 0;



    EventedAdmissionFailPoint evented_fail_point_ = EventedAdmissionFailPoint::none;
#endif




    std::unique_ptr<class EventedWaitPolicy> evented_policy_;



    std::vector<std::unique_ptr<Fiber>> evented_fibers_;
    std::vector<std::unique_ptr<std::byte[]>> evented_stacks_;
};

}


#include <sluice/async/evented_wait_policy.hpp>
#include <sluice/async/scheduler.hpp>

namespace sluice::async {

template <class Fn>
void Group::async_evented(Fn fn) {





    auto fut = std::make_shared<Future<void>>(*evented_policy_);



    constexpr std::size_t kStackBytes = 64 * 1024;
    auto stack_up = std::unique_ptr<std::byte[]>(new std::byte[kStackBytes]);
    std::byte* stack_base = stack_up.get();


    auto fiber_up = std::make_unique<Fiber>();
    Fiber* fiber_raw = fiber_up.get();




    fiber_up->set_entry([fut, fn = std::move(fn), tok = &token_](Fiber&) mutable {
        try {
            fn(*tok);
        } catch (...) {

        }
        fut->complete_with(sluice::Result<void>{});
    });





    bool ok = sched_->init_fiber(*fiber_raw, stack_base, kStackBytes);
    if (!ok) {
        throw std::runtime_error(
            "sluice::async::Group::async_evented: init_fiber failed "
            "(invalid stack or unsupported architecture)");
    }






















    static_assert(std::is_nothrow_move_constructible_v<std::unique_ptr<Fiber>>,
                  "unique_ptr<Fiber> move must be noexcept for transactional commit");
    static_assert(std::is_nothrow_move_constructible_v<std::unique_ptr<std::byte[]>>,
                  "unique_ptr<std::byte[]> move must be noexcept for transactional commit");
    static_assert(std::is_nothrow_move_constructible_v<std::shared_ptr<Future<void>>>,
                  "shared_ptr<Future<void>> move must be noexcept for transactional commit");
    Fiber* spawn_target = nullptr;
    {
        std::lock_guard<std::mutex> lk(mtx_);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)





        const auto fp = evented_fail_point_;
        evented_fail_point_ = EventedAdmissionFailPoint::none;
        if (fp == EventedAdmissionFailPoint::before_fiber_storage_reserve) {
            throw std::bad_alloc();
        }
#endif
        evented_fibers_.reserve(evented_fibers_.size() + 1);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        if (fp == EventedAdmissionFailPoint::before_stack_storage_reserve) {
            throw std::bad_alloc();
        }
#endif
        evented_stacks_.reserve(evented_stacks_.size() + 1);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        if (fp == EventedAdmissionFailPoint::before_future_storage_reserve) {
            throw std::bad_alloc();
        }
#endif
        futures_.reserve(futures_.size() + 1);



        evented_fibers_.push_back(std::move(fiber_up));
        evented_stacks_.push_back(std::move(stack_up));
        futures_.push_back(std::move(fut));
        spawn_target = fiber_raw;
    }



    sched_->spawn(*spawn_target);
}

}
