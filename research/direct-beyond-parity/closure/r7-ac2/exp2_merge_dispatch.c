/* EXP-2 (R7 AC-2): dispatch of short reads / EOF through a merged covering
 * pread on a STATIC file, against the per-member independent-read oracle.
 *
 * Implements the candidate's reconstruction rule (API_CAPABILITY_CANDIDATES
 * AC-2 §1/§4): planner = sort + union/merge (overlap or gap <= T); one pread
 * per merged component into staging; per-member outcome reconstructed:
 *   count_member = clamp(observed_count, member offset within component)
 *   EOF flag     = count 0 on a nonempty member (SEM-05: EOF at observed pos)
 *   zero-length member = logical no-op: success 0, no EOF observation
 *   (SEM-03 step 3); it contributes no extent and is never served.
 * Reference: each member through its own independent pread (the read_at
 * oracle). Gate: per-member (count, bytes, eof0) equal.
 * Static file only — this validates the reconstruction rules, NOT dedup
 * semantics under mutation (EXP-1's subject).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define S 10000u   /* file size */

typedef struct { uint64_t off, len; const char* name; } Req;

static const Req reqs[] = {
    {0,     100, "m0 inside"},
    {200,   100, "m1 inside, gap 100 <= T after m0"},
    {250,   100, "m2 overlaps m1"},
    {9900,  200, "m3 straddles EOF (prefix 100)"},
    {9950,    0, "m4 zero-length valid offset (no-op)"},
    {12000, 100, "m5 wholly past EOF"},
};
#define K ((int)(sizeof(reqs) / sizeof(reqs[0])))
#define T 4096u

typedef struct { uint64_t start, end; } Comp;   /* [start, end) */

int main(void) {
    const char* path = "/tmp/r7ac2-exp2.bin";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("open"); return 2; }
    unsigned char* seed = malloc(S);
    for (unsigned i = 0; i < S; ++i) seed[i] = (unsigned char)(i * 31 + 5);
    if (write(fd, seed, S) != (ssize_t)S) { perror("seed"); return 2; }

    /* --- reference: independent per-member preads (the read_at oracle) --- */
    struct { size_t count; int eof0; } ref[K];
    static unsigned char dst[K][256];
    memset(dst, 0xEE, sizeof dst);
    for (int i = 0; i < K; ++i) {
        if (reqs[i].len == 0) { ref[i].count = 0; ref[i].eof0 = 0; continue; }
        ssize_t n = pread(fd, dst[i], reqs[i].len, (off_t)reqs[i].off);
        if (n < 0) { fprintf(stderr, "ref pread %s: %s\n", reqs[i].name, strerror(errno)); return 2; }
        ref[i].count = (size_t)n;
        ref[i].eof0 = (n == 0);
    }

    /* --- gathered executor: sort, merge (overlap or gap <= T), one pread per
     *     component, reconstruct per-member outcomes --- */
    Req sorted[K]; memcpy(sorted, reqs, sizeof reqs);
    for (int i = 1; i < K; ++i)            /* insertion sort by offset (stable) */
        for (int j = i; j > 0 && sorted[j].off < sorted[j-1].off; --j) {
            Req t = sorted[j]; sorted[j] = sorted[j-1]; sorted[j-1] = t;
        }
    Comp comp[K]; int m = 0;
    uint64_t issued = 0;
    for (int i = 0; i < K; ++i) {
        if (sorted[i].len == 0) continue;             /* no-op member: no extent */
        uint64_t s = sorted[i].off, e = sorted[i].off + sorted[i].len;
        if (m > 0 && (s <= comp[m-1].end || s - comp[m-1].end <= (uint64_t)T)) {
            if (e > comp[m-1].end) comp[m-1].end = e; /* extend (covers overlap+gap) */
        } else {
            comp[m].start = s; comp[m].end = e; ++m;
        }
    }
    static unsigned char staging[8192];
    struct { size_t count; int eof0; int served; } got[K];
    for (int i = 0; i < K; ++i) { got[i].count = (size_t)-1; got[i].eof0 = -1; got[i].served = 0; }
    for (int c = 0; c < m; ++c) {
        uint64_t clen = comp[c].end - comp[c].start;
        if (clen > sizeof staging) { fprintf(stderr, "staging too small\n"); return 2; }
        ssize_t n = pread(fd, staging, (size_t)clen, (off_t)comp[c].start);
        if (n < 0) { fprintf(stderr, "merged pread: %s\n", strerror(errno)); return 2; }
        issued += (uint64_t)n;   /* argument caliber is clen; observed = n */
        uint64_t obs_end = comp[c].start + (uint64_t)n;
        for (int i = 0; i < K; ++i) {
            if (reqs[i].len == 0) continue;
            uint64_t s = reqs[i].off, e = reqs[i].off + reqs[i].len;
            if (s < comp[c].start || s >= comp[c].end) continue;
            if (e > comp[c].end) { fprintf(stderr, "member escapes component\n"); return 2; }
            if (got[i].served) { fprintf(stderr, "member served twice\n"); return 2; }
            got[i].served = 1;
            if (s >= obs_end) { got[i].count = 0; got[i].eof0 = 1; continue; }
            uint64_t avail = obs_end - s;
            size_t cnt = (size_t)((e - s) < avail ? (e - s) : avail);
            if (memcmp(dst[i], staging + (s - comp[c].start), cnt) != 0) {
                fprintf(stderr, "BYTE MISMATCH member %s\n", reqs[i].name); return 3;
            }
            got[i].count = cnt;
            got[i].eof0 = (cnt == 0);
        }
    }

    /* --- gate: per-member equality --- */
    int pass = 1;
    printf("%-38s %8s %8s %8s %7s %7s\n", "member", "ref_cnt", "got_cnt", "offset", "refEOF", "gotEOF");
    for (int i = 0; i < K; ++i) {
        int ok;
        if (reqs[i].len == 0) ok = (got[i].served == 0) && (ref[i].count == 0);
        else ok = got[i].served && (got[i].count == ref[i].count) && (got[i].eof0 == ref[i].eof0);
        pass &= ok;
        printf("%-38s %8zu %8zu %8llu %7d %7d %s\n", reqs[i].name, ref[i].count,
               got[i].count == (size_t)-1 ? 0u : got[i].count,
               (unsigned long long)reqs[i].off, ref[i].eof0,
               got[i].eof0 == -1 ? 0 : got[i].eof0, ok ? "OK" : "MISMATCH");
    }
    uint64_t requested = 0; int nonempty = 0;
    for (int i = 0; i < K; ++i) { requested += reqs[i].len; if (reqs[i].len) ++nonempty; }
    printf("components=%d (member preads %d -> %d; zero-length no-op issues none), requested=%llu B, observed(issued-clamp)=%llu B\n",
           m, nonempty, m, (unsigned long long)requested, (unsigned long long)issued);
    printf("EXP2_GATE=%s\n", pass ? "PASS" : "FAIL");
    close(fd); unlink(path);
    return pass ? 0 : 1;
}
