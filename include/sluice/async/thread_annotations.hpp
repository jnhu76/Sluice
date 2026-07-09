// sluice::async thread safety annotations (sluice-CORE-STATIC-1).
//
// Clang Thread Safety Analysis (TSA) macro substrate.  For non-Clang compilers
// every macro erases to a no-op; no compiler-specific runtime behaviour is
// introduced.
//
// Derived from the authoritative Clang TSA attribute vocabulary
// (https://clang.llvm.org/docs/ThreadSafetyAnalysis.html).
#pragma once

#if defined(__clang__) && !defined(SWIG)
#define SLUICE_TSA_ATTR__(x) __attribute__((x))
#else
#define SLUICE_TSA_ATTR__(x)  // no-op
#endif

// ---- Capability declaration ----

// Declare a type as a thread-safety capability (e.g. a mutex).
#define SLUICE_CAPABILITY(x)  SLUICE_TSA_ATTR__(capability(x))

// Declare a type as a scoped (RAII) capability.
#define SLUICE_SCOPED_CAPABILITY  SLUICE_TSA_ATTR__(scoped_lockable)

// ---- Guard annotations ----

// A data member is protected by the named capability.
#define SLUICE_GUARDED_BY(...)  SLUICE_TSA_ATTR__(guarded_by(__VA_ARGS__))

// A pointer member: the pointed-to data is protected by the named capability.
#define SLUICE_PT_GUARDED_BY(...)  SLUICE_TSA_ATTR__(pt_guarded_by(__VA_ARGS__))

// ---- Function contracts ----

// The caller must hold the named capability (exclusive access).
#define SLUICE_REQUIRES(...)  SLUICE_TSA_ATTR__(requires_capability(__VA_ARGS__))

// The caller must hold the named capability (shared/read access).
#define SLUICE_REQUIRES_SHARED(...)  \
    SLUICE_TSA_ATTR__(requires_shared_capability(__VA_ARGS__))

// The caller must NOT hold the named capability.
#define SLUICE_EXCLUDES(...)  SLUICE_TSA_ATTR__(locks_excluded(__VA_ARGS__))

// ---- Acquisition / release ----

// This function acquires the named capability (exclusive).
#define SLUICE_ACQUIRE(...)  SLUICE_TSA_ATTR__(acquire_capability(__VA_ARGS__))

// This function acquires the named capability (shared).
#define SLUICE_ACQUIRE_SHARED(...)  \
    SLUICE_TSA_ATTR__(acquire_shared_capability(__VA_ARGS__))

// This function releases the named capability.
#define SLUICE_RELEASE(...)  SLUICE_TSA_ATTR__(release_capability(__VA_ARGS__))

// This function releases the named capability (shared).
#define SLUICE_RELEASE_SHARED(...)  \
    SLUICE_TSA_ATTR__(release_shared_capability(__VA_ARGS__))

// This function tries to acquire the capability.  Returns true on success.
#define SLUICE_TRY_ACQUIRE(...)  \
    SLUICE_TSA_ATTR__(try_acquire_capability(__VA_ARGS__))

// This function tries to acquire the capability (shared).  Returns true on success.
#define SLUICE_TRY_ACQUIRE_SHARED(...)  \
    SLUICE_TSA_ATTR__(try_acquire_shared_capability(__VA_ARGS__))

// ---- Assertions ----

// Assert that the calling thread holds the named capability.
#define SLUICE_ASSERT_CAPABILITY(...)  \
    SLUICE_TSA_ATTR__(assert_capability(__VA_ARGS__))

// Assert that the calling thread holds the named capability (shared).
#define SLUICE_ASSERT_SHARED_CAPABILITY(...)  \
    SLUICE_TSA_ATTR__(assert_shared_capability(__VA_ARGS__))

// ---- Suppression ----

// Suppress thread-safety analysis on this function.
#define SLUICE_NO_THREAD_SAFETY_ANALYSIS  \
    SLUICE_TSA_ATTR__(no_thread_safety_analysis)

// ---- Return value ----

// The returned value is a capability reference.
#define SLUICE_RETURN_CAPABILITY(...)  \
    SLUICE_TSA_ATTR__(lock_returned(__VA_ARGS__))
