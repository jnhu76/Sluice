#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

typedef int (*lock_fn)(pthread_mutex_t*);

static lock_fn real_lock;
static _Atomic uint64_t slot_locks[128];
static _Atomic uint64_t slot_contended[128];
static _Atomic int next_slot;
static __thread int my_slot = -1;

static void ensure_init(void) {
    if (real_lock == NULL) {
        real_lock = (lock_fn)dlsym(RTLD_NEXT, "pthread_mutex_lock");
    }
    if (my_slot < 0) {
        int s = atomic_fetch_add(&next_slot, 1);
        my_slot = (s < 128) ? s : 127;
    }
}

int pthread_mutex_lock(pthread_mutex_t* m) {
    ensure_init();
    if (real_lock == NULL || pthread_mutex_trylock(m) == 0) {
        if (real_lock != NULL) {
            atomic_fetch_add(&slot_locks[my_slot], 1);
        }
        return 0;
    }
    atomic_fetch_add(&slot_contended[my_slot], 1);
    const int rc = real_lock(m);
    atomic_fetch_add(&slot_locks[my_slot], 1);
    return rc;
}

__attribute__((destructor)) static void report(void) {
    uint64_t locks = 0, contended = 0;
    for (int i = 0; i < 128; ++i) {
        locks += atomic_load(&slot_locks[i]);
        contended += atomic_load(&slot_contended[i]);
    }
    const double rate = locks ? (double)contended / (double)locks : 0.0;
    fprintf(stderr, "MUTEX_SHIM locks=%llu contended=%llu contended_rate=%.4f\n",
            (unsigned long long)locks, (unsigned long long)contended, rate);
}
