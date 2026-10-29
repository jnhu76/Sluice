/* EXP-3 (R7 AC-2): mid-batch OS error injection and per-member admission
 * failure inside a gathered batch — raw POSIX oracle construction.
 *
 * Members (one call's logical request set):
 *   m0 (600, 100)   valid
 *   m1 admission-invalid: offset+len unrepresentable (the library's checked
 *      arithmetic rejects invalid_argument before any syscall — reproduced
 *      here by the same check, no syscall issued)
 *   m2 (5000, 100)  valid extent, destination = mmap(PROT_NONE) page ->
 *      pread returns EFAULT deterministically: a REAL mid-batch OS error
 *   m3 (7000, 100)  valid, positioned AFTER the failing member
 *
 * Policies compared (the candidate text does not pin one — this is the gap):
 *   policy continue: every member after the error is still attempted and
 *     settles with its own outcome {count|eof|error} — per-member result
 *     responsibility preserved with the EXISTING outcome shapes.
 *   policy abort: execution stops at the first member error; later members
 *     are never attempted and need a "not attempted / aborted" outcome value
 *     that the candidate's result shape {count, EOF, admission, OS error,
 *     certainty} does not have.
 *
 * Also demonstrates a whole-call (merged-form) error: pread on a directory
 * fd fails EISDIR for the entire transfer — the candidate's intersect rule
 * then attributes the same OS error to every member of the failed extent,
 * matching what each member's independent pread on that fd would return.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define S 16384u

static ssize_t try_pread(int fd, void* buf, size_t n, off_t off, int* err) {
    ssize_t r = pread(fd, buf, n, off);
    *err = (r < 0) ? errno : 0;
    return r;
}

/* mirrors detail::checked_posix_offset admission (offset+len must fit off_t) */
static int admission_reject(uint64_t off, uint64_t len) {
    return off > (uint64_t)INT64_MAX || len > (uint64_t)INT64_MAX ||
           off + len > (uint64_t)INT64_MAX;
}

int main(void) {
    const char* path = "/tmp/r7ac2-exp3.bin";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("open"); return 2; }
    unsigned char* seed = malloc(S);
    for (unsigned i = 0; i < S; ++i) seed[i] = (unsigned char)(i * 13 + 9);
    if (write(fd, seed, S) != (ssize_t)S) { perror("seed"); return 2; }

    static unsigned char ok0[100], ok3[100];
    void* fault_dst = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fault_dst == MAP_FAILED) { perror("mmap"); return 2; }

    /* --- per-member outcomes, policy CONTINUE --- */
    printf("== policy continue (every member settles) ==\n");
    int err; ssize_t n;
    n = try_pread(fd, ok0, 100, 600, &err);
    printf("m0 valid            -> count=%zd errno=%d(%s)\n", n, err, strerror(err));
    const uint64_t BAD_OFF = ~(uint64_t)0;   /* offset+len unrepresentable in off_t */
    int rejected = admission_reject(BAD_OFF, 8);
    printf("m1 admission        -> %s (no syscall; admission_reject(UINT64_MAX,8)=%d)\n",
           rejected ? "invalid_argument" : "NOT REJECTED (probe defect)", rejected);
    n = try_pread(fd, fault_dst, 100, 5000, &err);
    printf("m2 PROT_NONE dst    -> count=%zd errno=%d(%s)\n", n, err, strerror(err));
    n = try_pread(fd, ok3, 100, 7000, &err);
    printf("m3 valid after err  -> count=%zd errno=%d(%s)\n", n, err, strerror(err));
    int continue_ok = (n == 100 && err == 0);

    /* --- policy ABORT: m3 never attempted --- */
    printf("== policy abort (stop at first member error) ==\n");
    printf("m3 NOT_ATTEMPTED    -> outcome value ABSENT from candidate result shape\n");
    printf("   {count, EOF, admission rejection, OS error, certainty} cannot express it\n");

    /* --- whole-call (merged-form) error: directory fd --- */
    printf("== merged-form whole-call error (directory fd) ==\n");
    int dfd = open("/tmp", O_RDONLY | O_DIRECTORY);
    if (dfd < 0) { perror("open dir"); return 2; }
    n = try_pread(dfd, ok0, 100, 0, &err);
    printf("merged pread(dir)   -> count=%zd errno=%d(%s): every member of the failed\n", n, err, strerror(err));
    printf("                       extent gets this same error under the intersect rule\n");
    n = try_pread(dfd, ok0, 100, 600, &err);
    printf("member-shaped pread(dir) at m0's offset -> count=%zd errno=%d(%s) (matches)\n",
           n, err, strerror(err));

    printf("EXP3_GATE=%s (continue_ok=%d rejected=%d; expect EFAULT=%d, EISDIR=%d)\n",
           (continue_ok && rejected && err == EISDIR) ? "PASS" : "FAIL", continue_ok,
           rejected, EFAULT, EISDIR);
    munmap(fault_dst, 4096);
    close(dfd); close(fd); unlink(path);
    return 0;
}
