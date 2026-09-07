







#pragma once

#if defined(__clang__) && !defined(SWIG)
#define SLUICE_TSA_ATTR__(x) __attribute__((x))
#else
#define SLUICE_TSA_ATTR__(x)
#endif




#define SLUICE_CAPABILITY(x)  SLUICE_TSA_ATTR__(capability(x))


#define SLUICE_SCOPED_CAPABILITY  SLUICE_TSA_ATTR__(scoped_lockable)




#define SLUICE_GUARDED_BY(...)  SLUICE_TSA_ATTR__(guarded_by(__VA_ARGS__))


#define SLUICE_PT_GUARDED_BY(...)  SLUICE_TSA_ATTR__(pt_guarded_by(__VA_ARGS__))




#define SLUICE_REQUIRES(...)  SLUICE_TSA_ATTR__(requires_capability(__VA_ARGS__))


#define SLUICE_REQUIRES_SHARED(...)  \
    SLUICE_TSA_ATTR__(requires_shared_capability(__VA_ARGS__))


#define SLUICE_EXCLUDES(...)  SLUICE_TSA_ATTR__(locks_excluded(__VA_ARGS__))




#define SLUICE_ACQUIRE(...)  SLUICE_TSA_ATTR__(acquire_capability(__VA_ARGS__))


#define SLUICE_ACQUIRE_SHARED(...)  \
    SLUICE_TSA_ATTR__(acquire_shared_capability(__VA_ARGS__))


#define SLUICE_RELEASE(...)  SLUICE_TSA_ATTR__(release_capability(__VA_ARGS__))


#define SLUICE_RELEASE_SHARED(...)  \
    SLUICE_TSA_ATTR__(release_shared_capability(__VA_ARGS__))


#define SLUICE_TRY_ACQUIRE(...)  \
    SLUICE_TSA_ATTR__(try_acquire_capability(__VA_ARGS__))


#define SLUICE_TRY_ACQUIRE_SHARED(...)  \
    SLUICE_TSA_ATTR__(try_acquire_shared_capability(__VA_ARGS__))




#define SLUICE_ASSERT_CAPABILITY(...)  \
    SLUICE_TSA_ATTR__(assert_capability(__VA_ARGS__))


#define SLUICE_ASSERT_SHARED_CAPABILITY(...)  \
    SLUICE_TSA_ATTR__(assert_shared_capability(__VA_ARGS__))




#define SLUICE_NO_THREAD_SAFETY_ANALYSIS  \
    SLUICE_TSA_ATTR__(no_thread_safety_analysis)




#define SLUICE_RETURN_CAPABILITY(...)  \
    SLUICE_TSA_ATTR__(lock_returned(__VA_ARGS__))
