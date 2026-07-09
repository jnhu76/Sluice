// CPP-STATIC-1 negative compiler proof N3: Control guarded field.
//
// Compile: clang++ -std=c++20 -Wthread-safety -Werror=thread-safety \
//          -I include tsa_negative_n3.cpp
//
// Expected: REJECTED.  Proves TSA catches unguarded read/write of a
// GUARDED_BY field.  Mirrors SchedulerWakeHandle::Control's
// alive/scheduler GUARDED_BY(mtx) pattern.

#include <sluice/async/mutex.hpp>
#include <sluice/async/thread_annotations.hpp>

struct Control {
    sluice::async::Mutex mtx;
    void* scheduler SLUICE_GUARDED_BY(mtx);
    bool alive SLUICE_GUARDED_BY(mtx);
};

void n3_write_bad() {
    Control c;
    c.alive = false;  // unguarded write — REJECTED
}

void n3_read_bad() {
    Control c;
    bool v = c.alive;  // unguarded read — REJECTED
    (void)v;
}

int main() { n3_write_bad(); n3_read_bad(); return 0; }