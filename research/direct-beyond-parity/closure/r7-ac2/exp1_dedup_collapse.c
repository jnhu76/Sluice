/* EXP-1 (R7 AC-2): same-offset duplicate reads under a concurrent writer.
 *
 * Deterministic witness, one file, two threads, pipe handshake:
 *   arm U (un-deduped): member A = pread(off), then writer mutates, then
 *     member B = pread(off). Two independent kernel observations.
 *   arm D (dedup-simulated): member A = pread(off) at t1; writer mutates at t2;
 *     member B is SERVED FROM A's OBSERVATION (no second pread) — exactly what
 *     intra-set dedup does for two identical members; then a control pread C
 *     shows the file changed inside the members' own service window.
 *   arm UQ / DQ: identical runs with the writer quiesced (static-file control:
 *     byte-identity holds under BOTH semantics — the red-line demonstration
 *     that static oracles cannot distinguish dedup semantics).
 *
 * Gate (expected, deterministic by handshake):
 *   U:  digest(A) != digest(B) in 100/100 trials   (observations differ)
 *   D:  digest(A) == digest(B) in 100/100 AND digest(C) != digest(A) in 100/100
 *   UQ: digest(A) == digest(B) in 100/100          (static control)
 *   DQ: digest(A) == digest(B) in 100/100          (static control)
 * No timing is measured; this is a semantic witness, not a benchmark.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#define FILE_BYTES  (1u << 20)
#define OFF         4096u
#define REGION      64u
#define TRIALS      100

static uint64_t fnv1a(const unsigned char* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static int g_fd;
static unsigned char g_bufA[REGION], g_bufB[REGION], g_bufC[REGION];
static int g_mutate;              /* 1 = writer mutates, 0 = quiesced control */
static int g_dedup;               /* 1 = serve B from A's observation */
static int g_go[2], g_done[2];    /* handshake pipes */

static void pipe_wait(int* p) { char c; if (read(p[0], &c, 1) != 1) { perror("pipe read"); exit(2); } }
static void pipe_sig(int* p) { if (write(p[1], "x", 1) != 1) { perror("pipe write"); exit(2); } }

static void* writer_fn(void* arg) {
    (void)arg;
    for (int t = 0; t < 4 * TRIALS; ++t) {   /* all arms × trials handshake */
        pipe_wait(g_go);
        if (g_mutate) {
            unsigned char tmp[REGION];
            if (pread(g_fd, tmp, REGION, (off_t)OFF) != (ssize_t)REGION) { perror("writer pread"); exit(2); }
            for (unsigned i = 0; i < REGION; ++i) tmp[i] ^= 0xFF;
            if (pwrite(g_fd, tmp, REGION, (off_t)OFF) != (ssize_t)REGION) { perror("writer pwrite"); exit(2); }
        }
        pipe_sig(g_done);
    }
    return NULL;
}

static int run_arm(void) {
    for (int t = 0; t < TRIALS; ++t) {
        ssize_t na = pread(g_fd, g_bufA, REGION, (off_t)OFF);
        if (na != (ssize_t)REGION) { perror("pread A"); exit(2); }
        pipe_sig(g_go);
        pipe_wait(g_done);
        if (g_dedup) {
            memcpy(g_bufB, g_bufA, REGION);           /* served by A's observation */
        } else {
            if (pread(g_fd, g_bufB, REGION, (off_t)OFF) != (ssize_t)REGION) { perror("pread B"); exit(2); }
        }
        if (pread(g_fd, g_bufC, REGION, (off_t)OFF) != (ssize_t)REGION) { perror("pread C"); exit(2); }
        uint64_t ha = fnv1a(g_bufA, REGION), hb = fnv1a(g_bufB, REGION), hc = fnv1a(g_bufC, REGION);
        printf("trial=%d arm=%s digestA=%016llx digestB=%016llx digestC=%016llx eqAB=%d eqAC=%d\n",
               t, g_dedup ? (g_mutate ? "D" : "DQ") : (g_mutate ? "U" : "UQ"),
               (unsigned long long)ha, (unsigned long long)hb, (unsigned long long)hc,
               ha == hb, ha == hc);
        if (g_mutate) { /* restore the original pattern (g_bufA holds it) for the next trial */
            if (pwrite(g_fd, g_bufA, REGION, (off_t)OFF) != (ssize_t)REGION) { perror("restore"); exit(2); }
        }
    }
    return 0;
}

int main(void) {
    const char* path = "/tmp/r7ac2-exp1.bin";
    g_fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (g_fd < 0) { perror("open"); return 2; }
    unsigned char* big = malloc(FILE_BYTES);
    for (unsigned i = 0; i < FILE_BYTES; ++i) big[i] = (unsigned char)(i * 7 + 1);
    if (write(g_fd, big, FILE_BYTES) != (ssize_t)FILE_BYTES) { perror("seed write"); return 2; }
    free(big);

    if (pipe(g_go) || pipe(g_done)) { perror("pipe"); return 2; }
    pthread_t w;
    pthread_create(&w, NULL, writer_fn, NULL);

    struct { int mutate; int dedup; const char* name; } arms[] = {
        {1, 0, "U concurrent-writer, two observations"},
        {1, 1, "D concurrent-writer, dedup (B served by A's observation)"},
        {0, 0, "UQ static control, two observations"},
        {0, 1, "DQ static control, dedup"},
    };
    for (unsigned a = 0; a < 4; ++a) {
        g_mutate = arms[a].mutate; g_dedup = arms[a].dedup;
        printf("== arm %s ==\n", arms[a].name);
        run_arm();
    }
    pthread_join(w, NULL);
    close(g_fd);
    unlink(path);
    return 0;
}
