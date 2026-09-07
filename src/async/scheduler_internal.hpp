










#ifndef SLUICE_ASYNC_SCHEDULER_INTERNAL_HPP
#define SLUICE_ASYNC_SCHEDULER_INTERNAL_HPP

#include <sluice/async/scheduler.hpp>

#include <condition_variable>
#include <mutex>

namespace sluice::async {







inline thread_local WorkerState* g_worker = nullptr;













struct RwWaitCtx {
    enum class Mode : std::uint8_t { read, write };
    Mode mode;
    ActorId actor;
};



struct SchedulerWakeHandle::Control {

















    Mutex mtx;
    Scheduler* scheduler SLUICE_GUARDED_BY(mtx){nullptr};
    bool alive SLUICE_GUARDED_BY(mtx){false};








#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    bool lifetime_seam_armed{false};
    bool lifetime_seam_paused{false};
    std::mutex lifetime_seam_mtx;
    std::condition_variable lifetime_seam_cv;
#endif
};

}

#endif
