// CPP-STATIC-1 external probe: annotated Mutex + std::lock_guard
// Compile: clang++ -std=c++20 -Wthread-safety -Werror=thread-safety \
//          -I include tsa_probe_annotated_mutex.cpp
// Purpose: determine whether Clang 21 TSA tracks the annotated Mutex +
// std::lock_guard as capability acquisition.

#include <sluice/async/mutex.hpp>

#include <mutex>

sluice::async::Mutex mu;

// ---- Probe A: GUARDED_BY on annotated Mutex ----
int value_a SLUICE_GUARDED_BY(mu);

// Expected: compile REJECTED
void bad_a() {
    value_a++;  // unguarded
}

// Expected: compile PASS (if std::lock_guard is tracked)
void good_a() {
    std::lock_guard<sluice::async::Mutex> lk(mu);
    value_a++;
}

// ---- Probe B: REQUIRES on annotated Mutex ----
void locked_fn() SLUICE_REQUIRES(mu) {
    value_a++;  // OK — caller holds mu
}

// Expected: compile REJECTED
void bad_b() {
    locked_fn();  // caller does NOT hold mu
}

// Expected: compile PASS (if std::lock_guard is tracked)
void good_b() {
    std::lock_guard<sluice::async::Mutex> lk(mu);
    locked_fn();
}

int main() {
    bad_a();
    good_a();
    bad_b();
    good_b();
    return 0;
}