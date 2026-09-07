






































#pragma once

#include <sluice/async/async_io_context.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>

namespace sluice::async::detail {

class ReadyWaitSource final : public BackendWaitSource {
  public:
    ReadyWaitSource() = default;
    BackendWaitToken snapshot() const noexcept override {
        std::lock_guard<std::mutex> lk(mtx_);
        return BackendWaitToken{ready_epoch_, control_epoch_};
    }

    BackendWakeReason wait_for_change(BackendWaitToken observed) noexcept override {


        return wait_for_change(observed, std::chrono::nanoseconds::max());
    }



    bool supports_bounded_wait() const noexcept override { return true; }

    BackendWakeReason wait_for_change(BackendWaitToken observed,
                                      std::chrono::nanoseconds max_park) noexcept override {
        std::unique_lock<std::mutex> lk(mtx_);
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)



        if (auto* f = wait_phase_flag_.load(std::memory_order_acquire)) {
            f->store(true, std::memory_order_release);



            f->notify_all();
        }





        if (auto* c = prepark_counter_.load(std::memory_order_acquire)) {
            c->fetch_add(1, std::memory_order_relaxed);



            c->notify_all();
        }
#endif




        if (max_park == std::chrono::nanoseconds::max()) {
            ready_cv_.wait(lk, [&] {
                return ready_epoch_ != observed.progress_generation ||
                       control_epoch_ != observed.control_generation;
            });
        } else {
            ready_cv_.wait_for(lk, max_park, [&] {
                return ready_epoch_ != observed.progress_generation ||
                       control_epoch_ != observed.control_generation;
            });
        }


        if (control_epoch_ != observed.control_generation) {
            return BackendWakeReason::interrupted;
        }
        return BackendWakeReason::progress;
    }





    void interrupt_all() noexcept override {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ++control_epoch_;
        }
        ready_cv_.notify_all();
    }













    BackendWaitToken arm_committed_wait() noexcept override {
        std::lock_guard<std::mutex> lk(mtx_);
        armed_control_generation_ = control_epoch_;
        armed_ = true;
        return BackendWaitToken{ready_epoch_, control_epoch_};
    }
    BackendWaitToken consume_committed_wait() noexcept override {
        std::lock_guard<std::mutex> lk(mtx_);
        if (armed_) {
            armed_ = false;
            return BackendWaitToken{ready_epoch_, armed_control_generation_};
        }
        return BackendWaitToken{ready_epoch_, control_epoch_};
    }






    void signal_progress() noexcept {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ++ready_epoch_;
        }
        ready_cv_.notify_all();
    }

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    void set_wait_phase_flag(std::atomic<bool>* flag) noexcept {
        wait_phase_flag_.store(flag, std::memory_order_release);
    }


    void set_wait_prepark_counter(std::atomic<int>* counter) noexcept {
        prepark_counter_.store(counter, std::memory_order_release);
    }














    void wait_epoch_changed(BackendWaitToken observed) noexcept {
        std::unique_lock<std::mutex> lk(mtx_);
        ready_cv_.wait(lk, [&] {
            return ready_epoch_ != observed.progress_generation ||
                   control_epoch_ != observed.control_generation;
        });
    }







    std::optional<BackendWaitToken> try_snapshot() const noexcept {
        std::unique_lock<std::mutex> lk(mtx_, std::try_to_lock);
        if (!lk.owns_lock()) {
            return std::nullopt;
        }
        return BackendWaitToken{ready_epoch_, control_epoch_};
    }
#endif

  private:
    mutable std::mutex mtx_;
    std::condition_variable ready_cv_;
    std::uint64_t ready_epoch_ = 0;
    std::uint64_t control_epoch_ = 0;


    std::uint64_t armed_control_generation_ = 0;
    bool armed_ = false;

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)



    std::atomic<std::atomic<bool>*> wait_phase_flag_{nullptr};

    std::atomic<std::atomic<int>*> prepark_counter_{nullptr};
#endif
};

}
