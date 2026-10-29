/* EXP-5 (R7 AC-2): planner-only cost characterization across k — NON-CLAIM.
 *
 * Pure userspace (no syscalls in the timed region): builds the request set
 * the way the P3 g4k trace family does (k requests of 4096 B, gaps from a
 * seeded LCG), runs (a) sort+sweep threshold-merge planning at T=4096 and
 * (b) union dedup planning, and reports ns/op-group of PLANNER TIME ONLY at
 * k = 8, 64, 512.
 *
 * Status: small-scale cost characterization to bound item 7 (planner cost
 * ceiling). NOT a formal benchmark, NOT a wall-clock performance claim; the
 * claim-bearing planner cost evidence remains P3's [RUN] +495.2 ± 41.7
 * ns/op-group at equal syscall counts (k=8, includes dispatch bookkeeping).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { uint64_t off, len; } Req;
typedef struct { uint64_t start, end; } Comp;

static uint64_t lcg_state;
static uint64_t lcg_next(void) {
    lcg_state = lcg_state * 6364136223846793005ull + 1442695040888963407ull;
    return lcg_state >> 33;
}

static int cmp_req(const void* a, const void* b) {
    const Req* x = a; const Req* y = b;
    return (x->off > y->off) - (x->off < y->off);
}

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

int main(void) {
    const int ks[] = {8, 64, 512};
    const uint64_t T = 4096, LEN = 4096, SPACE = 1ull << 30;
    const int ITERS = 20000;
    for (unsigned ki = 0; ki < sizeof ks / sizeof ks[0]; ++ki) {
        const int k = ks[ki];
        Req* reqs = malloc(sizeof(Req) * (size_t)k);
        Req* sorted = malloc(sizeof(Req) * (size_t)k);
        Comp* comp = malloc(sizeof(Comp) * (size_t)k);
        lcg_state = 0x5eed0000ull + (uint64_t)k;
        for (int i = 0; i < k; ++i)
            reqs[i] = (Req){ (lcg_next() % (SPACE - LEN)) & ~63ull, LEN };

        double t0 = now_ns();
        int sink = 0;
        for (int it = 0; it < ITERS; ++it) {
            memcpy(sorted, reqs, sizeof(Req) * (size_t)k);
            qsort(sorted, (size_t)k, sizeof(Req), cmp_req);
            int m = 0;
            for (int i = 0; i < k; ++i) {
                uint64_t s = sorted[i].off, e = s + sorted[i].len;
                if (m > 0 && (s <= comp[m-1].end || s - comp[m-1].end <= T)) {
                    if (e > comp[m-1].end) comp[m-1].end = e;
                } else {
                    comp[m].start = s; comp[m].end = e; ++m;
                }
            }
            sink += m;                     /* consume so the planner is not dead code */
        }
        double t1 = now_ns();
        double ns_group = (t1 - t0) / (double)ITERS;
        printf("k=%-4d merge-plan planner-only: %8.1f ns/op-group (%.1f ns/request), components~%d sink=%d\n",
               k, ns_group, ns_group / (double)k, sink / ITERS, sink);

        /* dedup plan: same sort, overlap-only union (gap must be 0/overlap) */
        t0 = now_ns();
        sink = 0;
        for (int it = 0; it < ITERS; ++it) {
            memcpy(sorted, reqs, sizeof(Req) * (size_t)k);
            qsort(sorted, (size_t)k, sizeof(Req), cmp_req);
            int m = 0;
            for (int i = 0; i < k; ++i) {
                uint64_t s = sorted[i].off, e = s + sorted[i].len;
                if (m > 0 && s <= comp[m-1].end) {          /* overlap/adjacent only */
                    if (e > comp[m-1].end) comp[m-1].end = e;
                } else {
                    comp[m].start = s; comp[m].end = e; ++m;
                }
            }
            sink += m;
        }
        t1 = now_ns();
        ns_group = (t1 - t0) / (double)ITERS;
        printf("k=%-4d dedup-plan  planner-only: %8.1f ns/op-group (%.1f ns/request), components~%d sink=%d\n",
               k, ns_group, ns_group / (double)k, sink / ITERS, sink);
        free(reqs); free(sorted); free(comp);
    }
    printf("NON_CLAIM characterization; per-run variance expected; see r7-ac2.md for the claim-bearing [RUN] figures.\n");
    return 0;
}
