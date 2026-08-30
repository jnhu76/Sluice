/* conv_h13_sb_litmus.c — SE-2 conventional probe P-C13 (family H13).
 *
 * SE1-CA-H13-1 normalized trace (publication observed without the data it
 * announces), store-buffering litmus spelling. Two threads publish plain
 * (non-atomic) flags and read each other's flags with NO barrier — the
 * documented conventional obligation ("publication edges require full /
 * acquire-release barriers") is the thing being violated, and the hardware's
 * store buffering makes the failure observable.
 *
 * On x86-64 (TSO) the SB litmus outcome (both threads read 0) is permitted
 * and reliably observed across enough iterations.
 *
 * Expected (pre-registered):
 *   plain: 200000 iterations, both-zero observations > 0; silent publication
 *          failure with no layer objecting. MISSES.
 *   TSan : data race reported on the flag accesses. DETECTS.
 *
 * Iterations are separated by a pthread_barrier, whose full barrier semantics
 * keep iterations independent (each iteration starts from a quiesced state).
 *
 * RESEARCH PROBE — not part of any test group; run per the SE-2 plan.
 */
#include <pthread.h>
#include <stdio.h>

#define ITERATIONS 200000

static int x, y;
static int r1, r2;
static int both_zero = 0;
static pthread_barrier_t bar;

static void* worker0(void* arg) {
    (void)arg;
    for (int i = 0; i < ITERATIONS; ++i) {
        pthread_barrier_wait(&bar);
        x = 1;
        r1 = y;
        pthread_barrier_wait(&bar);
    }
    return NULL;
}

static void* worker1(void* arg) {
    (void)arg;
    for (int i = 0; i < ITERATIONS; ++i) {
        pthread_barrier_wait(&bar);
        y = 1;
        r2 = x;
        pthread_barrier_wait(&bar);
    }
    return NULL;
}

int main(void) {
    pthread_barrier_init(&bar, NULL, 3);
    pthread_t t0, t1;
    if (pthread_create(&t0, NULL, worker0, NULL) != 0) return 2;
    if (pthread_create(&t1, NULL, worker1, NULL) != 0) return 2;

    for (int i = 0; i < ITERATIONS; ++i) {
        pthread_barrier_wait(&bar);  /* start iteration from quiesced state */
        x = 0;
        y = 0;
        pthread_barrier_wait(&bar);  /* both workers did their store+load */
        if (r1 == 0 && r2 == 0) {
            ++both_zero;  /* each thread published, neither saw the other */
        }
    }

    pthread_join(t0, NULL);
    pthread_join(t1, NULL);
    pthread_barrier_destroy(&bar);

    printf(
        "SE2-C13: %d/%d iterations observed publication without visibility (both threads read the other's flag as 0) "
        "— PLAIN MEMORY LAYERS: no rejection, no detection, silent publication failure (MISSES)\n",
        both_zero, ITERATIONS);
    return (both_zero > 0) ? 0 : 2;
}
