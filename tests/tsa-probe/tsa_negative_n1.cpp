// CPP-STATIC-1 negative compiler proof N1: GUARDED_BY structural access.
//
// Compile: clang++ -std=c++20 -Wthread-safety -Werror=thread-safety \
//          -I include tsa_negative_n1.cpp
//
// Expected: REJECTED.  Proves TSA catches unguarded access to a
// GUARDED_BY field.  (WaitQueue fields are private; this fixture
// mirrors the exact GUARDED_BY(mtx_) pattern used in production.)

#include <sluice/async/mutex.hpp>
#include <sluice/async/thread_annotations.hpp>

struct FixtureN1 {
    sluice::async::Mutex mtx_;
    // N1: access head_ without holding mtx_ — REJECTED
    void* head_ SLUICE_GUARDED_BY(mtx_);
};

void n1_bad() {
    FixtureN1 q;
    q.head_ = nullptr;  // unguarded write — REJECTED
}

int main() { n1_bad(); return 0; }