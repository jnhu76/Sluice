// CPP-STATIC-1 negative compiler proof N2: REQUIRES locked seam.
//
// Compile: clang++ -std=c++20 -Wthread-safety -Werror=thread-safety \
//          -I include tsa_negative_n2.cpp
//
// Expected: REJECTED.  Proves TSA catches a call to a REQUIRES function
// without holding the required capability.  Mirrors the Scheduler's
// classify_locked / route_runnable_locked REQUIRES(global_mtx_) pattern.

#include <sluice/async/mutex.hpp>
#include <sluice/async/thread_annotations.hpp>

sluice::async::Mutex global_mtx_;

void classify_locked() SLUICE_REQUIRES(global_mtx_) {
    // Only callable while holding global_mtx_
}

void n2_bad() {
    classify_locked();  // caller does NOT hold global_mtx_ — REJECTED
}

int main() { n2_bad(); return 0; }