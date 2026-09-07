






























#pragma once

namespace sluice::async::detail {




enum class MutexTestOperation : unsigned char {
    lock,
    try_lock,
};

#if defined(SLUICE_ASYNC_INTERNAL_TESTING)





void maybe_inject_mutex_failure(MutexTestOperation op) noexcept(false);





namespace test_hooks {








void arm_lock_countdown(unsigned n) noexcept;


void arm_next_try_lock_fail() noexcept;




void disarm() noexcept;

}

#endif

}
