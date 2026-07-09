// CPP-STATIC-1 external probe: annotated LockGuard scoped capability.
// Compile: clang++ -std=c++20 -Wthread-safety -Werror=thread-safety \
//          -I include tsa_probe_lock_guard.cpp
// Purpose: verify that the annotated LockGuard is correctly tracked by TSA.

#include <sluice/async/mutex.hpp>
#include <sluice/async/lock_guard.hpp>

sluice::async::Mutex mu;

// ---- S1 — unguarded field access (expected: REJECTED) ----
int value SLUICE_GUARDED_BY(mu);

void s1_bad() {
    value++;  // unguarded — REJECTED
}

// ---- S2 — guarded field access with LockGuard (expected: PASS) ----
void s2_good() {
    sluice::async::LockGuard lk(mu);
    value++;  // guarded — PASS
}

// ---- S3 — missing REQUIRES caller lock (expected: REJECTED) ----
void s3_fn() SLUICE_REQUIRES(mu) {
    value++;
}

void s3_bad() {
    s3_fn();  // caller does NOT hold mu — REJECTED
}

// ---- S4 — valid REQUIRES call (expected: PASS) ----
void s4_good() {
    sluice::async::LockGuard lk(mu);
    s3_fn();  // caller holds mu — PASS
}

int main() {
    s1_bad();
    s2_good();
    s3_bad();
    s4_good();
    return 0;
}