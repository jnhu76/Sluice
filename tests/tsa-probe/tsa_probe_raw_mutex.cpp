// CPP-STATIC-1 external probe: raw std::mutex TSA recognition.
// Compile: clang++ -std=c++20 -Wthread-safety -Werror=thread-safety tsa_probe_raw_mutex.cpp
// Purpose: determine whether Clang 21 TSA tracks raw std::mutex +
// std::lock_guard as capability acquisition.

#include <mutex>

// Use the annotation macro directly (inline, no header dependency)
#if defined(__clang__)
#define TSA_GUARDED_BY(...) __attribute__((guarded_by(__VA_ARGS__)))
#else
#define TSA_GUARDED_BY(...)
#endif

std::mutex mu;

// ---- Probe  A: GUARDED_BY on raw std::mutex ----
int value_a TSA_GUARDED_BY(mu);  // TSA must recognise mu as a capability

// Expected: compile REJECTED
void bad_a() {
    value_a++;  // unguarded — TSA should reject
}

// Expected: compile PASS
void good_a() {
    std::lock_guard<std::mutex> lk(mu);
    value_a++;
}

// ---- Probe B: does std::lock_guard get recognised as SCOPED_CAPABILITY? ----
// (Already tested implicitly by good_a; if good_a fails, lock_guard is not tracked.)

int main() {
    bad_a();
    good_a();
    return 0;
}