/* conv_h09_lost_wake.c — SE-2 conventional probe P-C09 (family H09).
 *
 * SE1-CA-H09-1 normalized trace (wake consumed by nobody; waiter stranded),
 * POSIX condition-variable spelling. The program obeys normal pthreads usage
 * EXCEPT the ONE normalized hazard: the waiter checks the predicate ONCE and
 * then sleeps unconditionally — the check and the sleep are not one atomic
 * rechecking protocol, so a wake emitted in between is consumed by nobody.
 * (This is precisely the documented caller obligation the glibc-bug family
 * normalizes: wake pairing requires the registration order + predicate
 * protocol; the API does not enforce it.)
 *
 * Choreography (deterministic, no data races — every access is mutex-held):
 *   waiter:   lock; flag==0; hand STEP1 to signaler; take STEP2; cond_wait
 *   signaler: take STEP1; lock; flag=1; unlock; cond_signal; hand STEP2
 * The signal fires while nobody is registered -> lost; the waiter then sleeps
 * forever with the predicate satisfied.
 *
 * Expected (pre-registered):
 *   plain: hang reproduced; bounded by alarm(5) -> marker + exit 42. MISSES.
 *   TSan : same hang, NO data-race report (all accesses mutex-held) —
 *          demonstrating TSan is blind to logic-level lost wakes. MISSES.
 *
 * RESEARCH PROBE — not part of any test group; run per the SE-2 plan.
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int flag = 0;

/* Deterministic step sequencer (each step one mutex-protected flag). */
static pthread_mutex_t seq_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t seq_cv = PTHREAD_COND_INITIALIZER;
static int step1 = 0, step2 = 0;

static void on_alarm(int sig) {
    (void)sig;
    const char msg[] = "SE2-C09: LOST WAKE REPRODUCED — waiter stranded with predicate satisfied; alarm fired\n";
    ssize_t ignored = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)ignored;
    _exit(42);
}

static void wait_for(int* step, int value) {
    pthread_mutex_lock(&seq_m);
    while (*step != value) {
        pthread_cond_wait(&seq_cv, &seq_m);
    }
    pthread_mutex_unlock(&seq_m);
}

static void set_step(int* step, int value) {
    pthread_mutex_lock(&seq_m);
    *step = value;
    pthread_cond_broadcast(&seq_cv);
    pthread_mutex_unlock(&seq_m);
}

static void* signaler(void* arg) {
    (void)arg;
    wait_for(&step1, 1);  /* waiter has checked the predicate and is NOT yet asleep */
    pthread_mutex_lock(&m);
    flag = 1;
    pthread_mutex_unlock(&m);
    pthread_cond_signal(&cv);  /* consumed by nobody: registration has not happened */
    set_step(&step2, 1);       /* release the waiter into cond_wait */
    return NULL;
}

int main(void) {
    signal(SIGALRM, on_alarm);
    alarm(5);

    pthread_t t;
    if (pthread_create(&t, NULL, signaler, NULL) != 0) {
        return 2;
    }

    /* Waiter: single predicate check (the hazard), then unconditional sleep. */
    pthread_mutex_lock(&m);
    if (!flag) {
        set_step(&step1, 1);   /* predicate checked false; not yet registered */
        wait_for(&step2, 1);   /* signaler's wake is emitted (and lost) here */
        pthread_cond_wait(&cv, &m);  /* sleeps with predicate already satisfied */
    }
    pthread_mutex_unlock(&m);

    /* NOT REACHED when the hazard reproduces. */
    printf("SE2-C09: waiter woke (hazard did NOT reproduce)\n");
    pthread_join(t, NULL);
    return 2;
}
