/*
 * r6ac1_probe.c — AC-1 (#605) independent semantic probe for preadv/pwritev.
 *
 * Pure POSIX/Linux C, no Sluice headers. Each subcommand is one probe cell;
 * output is machine-readable "obs key=value" lines between CELL markers so a
 * reviewer can diff runs. Observations are recorded verbatim; nothing here
 * asserts a Sluice contract — the mapping to SEM-03/04/05 lives in the
 * design documents.
 *
 * Build: cc -O2 -Wall -Wextra -D_FILE_OFFSET_BITS=64 -o r6ac1_probe r6ac1_probe.c -lpthread
 * Usage: ./r6ac1_probe <cell> [scratch_dir]
 * Cells: env array-order short-read iov-fault-read iov-fault-write iovmax
 *        sum-overflow null-empty fsize-partial-write eintr cursor atomicity
 *        nonseekable all
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

#ifndef SSIZE_MAX
#define SSIZE_MAX ((ssize_t)(LONG_MAX))
#endif

#include <gnu/libc-version.h>

static const char *scratch = "/tmp/r6-ac1-scratch";

static ssize_t raw_preadv(int fd, const struct iovec *iov, int cnt, off_t off) {
    errno = 0; /* keep printed errno attributable to this call only */
    return (ssize_t)syscall((long)SYS_preadv, fd, iov, cnt, off);
}
static ssize_t raw_pwritev(int fd, const struct iovec *iov, int cnt, off_t off) {
    errno = 0;
    return (ssize_t)syscall((long)SYS_pwritev, fd, iov, cnt, off);
}

static void cell_begin(const char *name) { printf("CELL %s BEGIN\n", name); }
static void cell_end(const char *name) { printf("CELL %s END\n", name); }
static void obs(const char *key, long long v) { printf("obs %s=%lld\n", key, v); }

static void die(const char *what) {
    fprintf(stderr, "FATAL %s: %s (errno=%d)\n", what, strerror(errno), errno);
    exit(2);
}

static void scratch_prepare(void) {
    if (mkdir(scratch, 0755) != 0 && errno != EEXIST)
        die("mkdir scratch");
    char path[512];
    snprintf(path, sizeof path, "%s/probe-warmup", scratch);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        die("open warmup");
    close(fd);
    unlink(path);
}

/* Deterministic byte pattern: byte at file offset o is (o * 2654435761u) >> 24. */
static unsigned char pat(unsigned long long o) {
    return (unsigned char)(((o * 2654435761ULL) >> 24) & 0xFF);
}

static void fill_pattern(unsigned char *buf, size_t n, unsigned long long file_off) {
    for (size_t i = 0; i < n; i++)
        buf[i] = pat(file_off + i);
}

static int buf_matches_pattern(const unsigned char *buf, size_t n, unsigned long long file_off) {
    for (size_t i = 0; i < n; i++)
        if (buf[i] != pat(file_off + i))
            return 0;
    return 1;
}

static int buf_is(const unsigned char *buf, size_t n, unsigned char v) {
    for (size_t i = 0; i < n; i++)
        if (buf[i] != v)
            return 0;
    return 1;
}

static long long file_size(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0)
        die("fstat");
    return (long long)st.st_size;
}

static void make_pattern_file(const char *name, long long size) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", scratch, name);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        die("make_pattern_file open");
    static unsigned char chunk[65536];
    long long off = 0;
    while (off < size) {
        long long n = size - off < (long long)sizeof chunk ? size - off : (long long)sizeof chunk;
        fill_pattern(chunk, (size_t)n, (unsigned long long)off);
        if (pwrite(fd, chunk, (size_t)n, off) != n)
            die("make_pattern_file pwrite");
        off += n;
    }
    close(fd);
}

static void open_in(const char *name, int flags, int *fd) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", scratch, name);
    *fd = open(path, flags, 0644);
    if (*fd < 0)
        die(name);
}

/* ------------------------------------------------------------------ */
/* env                                                                 */
/* ------------------------------------------------------------------ */

static void cell_env(void) {
    cell_begin("env");
    obs("sysconf_SC_IOV_MAX", sysconf(_SC_IOV_MAX));
    obs("page_size", sysconf(_SC_PAGESIZE));
    obs("ssize_max", (long long)SSIZE_MAX);
    obs("off_t_bits", (long long)(sizeof(off_t) * 8));
    printf("obs glibc=%s\n", gnu_get_libc_version());
    cell_end("env");
}

/* ------------------------------------------------------------------ */
/* array-order: one call fills k buffers in order from one extent      */
/* ------------------------------------------------------------------ */

static void cell_array_order(void) {
    const long long SIZE = 12288;
    make_pattern_file("ao.dat", SIZE);

    unsigned char b0[4096], b1[4096], b2[4096];
    struct iovec iov[3] = {{b0, sizeof b0}, {b1, sizeof b1}, {b2, sizeof b2}};

    int fd;
    open_in("ao.dat", O_RDONLY, &fd);
    memset(b0, 0x55, sizeof b0);
    memset(b1, 0x55, sizeof b1);
    memset(b2, 0x55, sizeof b2);
    ssize_t r = raw_preadv(fd, iov, 3, 0);
    obs("read_ret", r);
    obs("read_errno", errno);
    obs("buf0_pattern_ok", buf_matches_pattern(b0, sizeof b0, 0));
    obs("buf1_pattern_ok", buf_matches_pattern(b1, sizeof b1, 4096));
    obs("buf2_pattern_ok", buf_matches_pattern(b2, sizeof b2, 8192));
    close(fd);

    /* wrapper parity: glibc preadv on the same file/shape */
    open_in("ao.dat", O_RDONLY, &fd);
    memset(b0, 0x55, sizeof b0);
    memset(b1, 0x55, sizeof b1);
    memset(b2, 0x55, sizeof b2);
    errno = 0;
    r = preadv(fd, iov, 3, 0);
    obs("wrapper_read_ret", r);
    obs("wrapper_errno", errno);
    obs("wrapper_buf0_ok", buf_matches_pattern(b0, sizeof b0, 0));
    obs("wrapper_buf2_ok", buf_matches_pattern(b2, sizeof b2, 8192));
    close(fd);

    /* write twin: three distinct source buffers -> one contiguous extent */
    unsigned char s0[2048], s1[2048], s2[2048];
    memset(s0, 0xA0, sizeof s0);
    memset(s1, 0xA1, sizeof s1);
    memset(s2, 0xA2, sizeof s2);
    struct iovec wiov[3] = {{s0, sizeof s0}, {s1, sizeof s1}, {s2, sizeof s2}};
    char wpath[512];
    snprintf(wpath, sizeof wpath, "%s/ao-w.dat", scratch);
    int wfd = open(wpath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0)
        die("open ao-w");
    r = raw_pwritev(wfd, wiov, 3, 1024);
    obs("write_ret", r);
    obs("write_errno", errno);
    obs("write_size_after", file_size(wfd));

    unsigned char back[6144];
    ssize_t rr = pread(wfd, back, sizeof back, 1024);
    obs("verify_pread_ret", rr);
    int ok = rr == 6144;
    for (int i = 0; ok && i < 2048; i++)
        ok = back[i] == 0xA0 && back[2048 + i] == 0xA1 && back[4096 + i] == 0xA2;
    obs("write_contiguous_in_order", ok);

    /* pwritev wrapper parity */
    if (ftruncate(wfd, 0) != 0)
        die("ftruncate");
    errno = 0;
    r = pwritev(wfd, wiov, 3, 1024);
    obs("wrapper_write_ret", r);
    close(wfd);
    cell_end("array-order");
}

/* ------------------------------------------------------------------ */
/* short-read: EOF-bounded extent; prefix fill across iov boundaries   */
/* ------------------------------------------------------------------ */

static void cell_short_read(void) {
    const long long SIZE = 6144;
    make_pattern_file("sr.dat", SIZE);

    unsigned char b0[4096], b1[4096], b2[4096], b16[16];
    struct iovec iov[3] = {{b0, sizeof b0}, {b1, sizeof b1}, {b2, sizeof b2}};

    int fd;
    open_in("sr.dat", O_RDONLY, &fd);
    memset(b0, 0xAA, sizeof b0);
    memset(b1, 0xAA, sizeof b1);
    memset(b2, 0xAA, sizeof b2);
    ssize_t r = raw_preadv(fd, iov, 3, 0);
    obs("short_ret", r);
    obs("short_errno", errno);
    /* file is 6144 B, request is 12288 B: buf0 full, buf1 half, buf2 untouched */
    obs("buf0_full_pattern", buf_matches_pattern(b0, sizeof b0, 0));
    obs("buf1_prefix2048_pattern", buf_matches_pattern(b1, 2048, 4096));
    int guard1 = 1;
    for (int i = 2048; i < 4096; i++)
        guard1 = guard1 && b1[i] == 0xAA;
    obs("buf1_trailer_untouched", guard1);
    obs("buf2_fully_untouched", buf_is(b2, sizeof b2, 0xAA));
    close(fd);

    /* EOF-0 exactly at EOF */
    struct iovec eiov = {b16, sizeof b16};
    open_in("sr.dat", O_RDONLY, &fd);
    memset(b16, 0xAA, sizeof b16);
    r = raw_preadv(fd, &eiov, 1, SIZE);
    obs("eof0_ret", r);
    obs("eof0_errno", errno);
    /* wholly past EOF */
    r = raw_preadv(fd, &eiov, 1, SIZE + 4096);
    obs("past_eof_ret", r);
    obs("past_eof_errno", errno);
    /* straddling start: 8 bytes before EOF */
    memset(b16, 0xAA, sizeof b16);
    r = raw_preadv(fd, &eiov, 1, SIZE - 8);
    obs("straddle_ret", r);
    obs("straddle_prefix_ok", buf_matches_pattern(b16, 8, SIZE - 8));
    int guard8 = 1;
    for (int i = 8; i < 16; i++)
        guard8 = guard8 && b16[i] == 0xAA;
    obs("straddle_trailer_untouched", guard8);
    close(fd);

    /* zero-length entries interleaved */
    memset(b0, 0xAA, sizeof b0);
    memset(b1, 0xAA, sizeof b1);
    memset(b2, 0xAA, sizeof b2);
    struct iovec ziov[3] = {{b0, 0}, {b1, sizeof b1}, {b2, 0}};
    open_in("sr.dat", O_RDONLY, &fd);
    r = raw_preadv(fd, ziov, 3, 0);
    obs("mixed_zero_len_ret", r);
    obs("mixed_zero_len_buf1_pattern", buf_matches_pattern(b1, sizeof b1, 0));
    int guardb0 = 1;
    for (int i = 0; i < 4096; i++)
        guardb0 = guardb0 && b0[i] == 0xAA;
    obs("mixed_zero_len_buf0_untouched", guardb0);
    close(fd);
    cell_end("short-read");
}

/* ------------------------------------------------------------------ */
/* iov-fault-read: EFAULT row and the mid-transfer fault question      */
/* ------------------------------------------------------------------ */

static void cell_iov_fault_read(void) {
    const long long SIZE = 12288;
    make_pattern_file("fr.dat", SIZE);
    long ps = sysconf(_SC_PAGESIZE);

    void *faultpg = mmap(NULL, (size_t)ps, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (faultpg == MAP_FAILED)
        die("mmap PROT_NONE");

    unsigned char b0[4096], b2[4096];

    /* case A: fault in the middle iov, valid iovs around */
    struct iovec iovA[3] = {{b0, sizeof b0}, {faultpg, (size_t)ps}, {b2, sizeof b2}};
    int fd;
    open_in("fr.dat", O_RDONLY, &fd);
    memset(b0, 0x77, sizeof b0);
    memset(b2, 0x77, sizeof b2);
    ssize_t r = raw_preadv(fd, iovA, 3, 0);
    obs("mid_fault_ret", r);
    obs("mid_fault_errno", errno);
    if (r > 0) {
        long long got = (long long)r;
        obs("mid_fault_buf0_full_pattern", got >= 4096 && buf_matches_pattern(b0, sizeof b0, 0));
        int b2_ok = 1;
        if (got > 4096) {
            long long into_b2 = got - 4096;
            if (into_b2 > 4096)
                into_b2 = 4096;
            b2_ok = buf_matches_pattern(b2, (size_t)into_b2, 8192);
            int guard = 1;
            for (size_t i = (size_t)into_b2; i < sizeof b2; i++)
                guard = guard && b2[i] == 0x77;
            b2_ok = b2_ok && guard;
        } else {
            for (size_t i = 0; i < sizeof b2; i++)
                b2_ok = b2_ok && b2[i] == 0x77;
        }
        obs("mid_fault_buf2_as_derived_from_total", b2_ok);
    } else {
        obs("mid_fault_buf0_guard_intact", buf_is(b0, sizeof b0, 0x77));
        obs("mid_fault_buf2_guard_intact", buf_is(b2, sizeof b2, 0x77));
    }
    close(fd);

    /* case B: fault in the first iov */
    struct iovec iovB[2] = {{faultpg, (size_t)ps}, {b0, sizeof b0}};
    open_in("fr.dat", O_RDONLY, &fd);
    memset(b0, 0x77, sizeof b0);
    r = raw_preadv(fd, iovB, 2, 0);
    obs("first_fault_ret", r);
    obs("first_fault_errno", errno);
    obs("first_fault_buf1_guard_intact", buf_is(b0, sizeof b0, 0x77));
    close(fd);

    /* case C: single faulting iov */
    struct iovec iovC[1] = {{faultpg, (size_t)ps}};
    open_in("fr.dat", O_RDONLY, &fd);
    r = raw_preadv(fd, iovC, 1, 0);
    obs("single_fault_ret", r);
    obs("single_fault_errno", errno);
    close(fd);

    /* wrapper parity for the mid-fault shape (glibc preadv) */
    open_in("fr.dat", O_RDONLY, &fd);
    memset(b0, 0x77, sizeof b0);
    memset(b2, 0x77, sizeof b2);
    errno = 0;
    r = preadv(fd, iovA, 3, 0);
    obs("wrapper_mid_fault_ret", r);
    obs("wrapper_mid_fault_errno", errno);
    close(fd);

    munmap(faultpg, (size_t)ps);
    cell_end("iov-fault-read");
}

/* ------------------------------------------------------------------ */
/* iov-fault-write                                                     */
/* ------------------------------------------------------------------ */

static void cell_iov_fault_write(void) {
    long ps = sysconf(_SC_PAGESIZE);
    void *faultpg = mmap(NULL, (size_t)ps, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (faultpg == MAP_FAILED)
        die("mmap PROT_NONE");

    static unsigned char s0[4096];
    memset(s0, 0x3C, sizeof s0);

    char path[512];
    snprintf(path, sizeof path, "%s/fw.dat", scratch);

    /* case A: valid prefix then faulting source */
    struct iovec iovA[2] = {{s0, sizeof s0}, {faultpg, (size_t)ps}};
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        die("open fw");
    ssize_t r = raw_pwritev(fd, iovA, 2, 0);
    obs("mid_fault_ret", r);
    obs("mid_fault_errno", errno);
    obs("mid_fault_size_after", file_size(fd));
    unsigned char back[4096];
    memset(back, 0, sizeof back);
    ssize_t pr = pread(fd, back, sizeof back, 0);
    obs("mid_fault_prefix_pread_ret", pr);
    int ok = pr == 4096;
    for (int i = 0; ok && i < 4096; i++)
        ok = back[i] == 0x3C;
    obs("mid_fault_prefix_landed", ok);
    close(fd);

    /* case B: single faulting source on empty file */
    struct iovec iovB[1] = {{faultpg, (size_t)ps}};
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        die("open fw2");
    r = raw_pwritev(fd, iovB, 1, 0);
    obs("single_fault_ret", r);
    obs("single_fault_errno", errno);
    obs("single_fault_size_after", file_size(fd));
    close(fd);

    munmap(faultpg, (size_t)ps);
    cell_end("iov-fault-write");
}

/* ------------------------------------------------------------------ */
/* iovmax                                                              */
/* ------------------------------------------------------------------ */

static void cell_iovmax(void) {
    enum { KMAX = 4096 };
    struct iovec *iov = malloc(sizeof(struct iovec) * (size_t)KMAX);
    if (!iov)
        die("malloc iov");
    long ps = sysconf(_SC_PAGESIZE);
    unsigned char *page = mmap(NULL, (size_t)ps, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED)
        die("mmap page");

    make_pattern_file("im.dat", KMAX);

    int fd;
    open_in("im.dat", O_RDONLY, &fd);
    struct {
        int k;
        const char *name;
    } cases[] = {
        {1023, "k1023"}, {1024, "k1024"}, {1025, "k1025"}, {4096, "k4096"},
    };
    for (unsigned ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
        int k = cases[ci].k;
        for (int i = 0; i < k; i++) {
            iov[i].iov_base = page;
            iov[i].iov_len = 1;
        }
        errno = 0;
        ssize_t r = raw_preadv(fd, iov, k, 0);
        printf("obs %s_ret=%lld\n", cases[ci].name, (long long)r);
        printf("obs %s_errno=%d\n", cases[ci].name, errno);
    }
    close(fd);

    /* write twin at the boundary */
    static unsigned char src[KMAX];
    memset(src, 0x5A, sizeof src);
    char wpath[512];
    snprintf(wpath, sizeof wpath, "%s/im-w.dat", scratch);
    int wfd = open(wpath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0)
        die("open im-w");
    for (int ki = 0; ki < 2; ki++) {
        int k = ki == 0 ? 1024 : 1025;
        for (int i = 0; i < k; i++) {
            iov[i].iov_base = src + i;
            iov[i].iov_len = 1;
        }
        errno = 0;
        ssize_t r = raw_pwritev(wfd, iov, k, 0);
        printf("obs %s_ret=%lld\n", ki == 0 ? "w_k1024" : "w_k1025", (long long)r);
        printf("obs %s_errno=%d\n", ki == 0 ? "w_k1024" : "w_k1025", errno);
        if (ki == 0)
            printf("obs w_k1024_size_after=%lld\n", file_size(wfd));
    }
    printf("obs w_final_size=%lld\n", file_size(wfd));
    close(wfd);

    munmap(page, (size_t)ps);
    free(iov);
    cell_end("iovmax");
}

/* ------------------------------------------------------------------ */
/* sum-overflow                                                        */
/* ------------------------------------------------------------------ */

static void cell_sum_overflow(void) {
    long ps = sysconf(_SC_PAGESIZE);
    unsigned char *page = mmap(NULL, (size_t)ps, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED)
        die("mmap page");
    make_pattern_file("so.dat", 8192);

    /* case A: two entries of 2^62 -> sum = 2^63 = negative as ssize_t */
    struct iovec iovA[2] = {{page, (size_t)1 << 62}, {page, (size_t)1 << 62}};
    int fd;
    open_in("so.dat", O_RDONLY, &fd);
    errno = 0;
    ssize_t r = raw_preadv(fd, iovA, 2, 0);
    obs("overflow_negative_ret", r);
    obs("overflow_negative_errno", errno);
    close(fd);

    /* case B: single entry len = SSIZE_MAX (positive, unallocatable) */
    struct iovec iovB[1] = {{page, (size_t)SSIZE_MAX}};
    open_in("so.dat", O_RDONLY, &fd);
    errno = 0;
    r = raw_preadv(fd, iovB, 1, 0);
    obs("ssize_max_single_ret", r);
    obs("ssize_max_single_errno", errno);
    close(fd);

    /*
     * case C: single backed entry of exactly 2 GiB > kernel per-call transfer
     * cap (MAX_RW_COUNT = INT_MAX & PAGE_MASK = 2147479552 on x86-64), on a
     * sparse 4 GiB tmpfs file. ret == 2147479552 means the kernel silently
     * clamps the requested total; ret == 2147483648 means it accepts it.
     */
    const long long GIANT = 4LL * 1024 * 1024 * 1024;
    const long long REQ = 2LL * 1024 * 1024 * 1024;
    char gpath[512];
    snprintf(gpath, sizeof gpath, "%s/so-sparse.dat", scratch);
    int gfd = open(gpath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (gfd < 0)
        die("open sparse");
    if (ftruncate(gfd, GIANT) != 0)
        die("ftruncate sparse");
    void *big = mmap(NULL, (size_t)REQ, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (big == MAP_FAILED)
        die("mmap big");
    struct iovec iovC[1] = {{big, (size_t)REQ}};
    errno = 0;
    r = raw_preadv(gfd, iovC, 1, 0);
    obs("above_cap_ret", r);
    obs("above_cap_errno", errno);
    obs("above_cap_requested", REQ);
    munmap(big, (size_t)REQ);
    close(gfd);
    unlink(gpath);

    /* case D: 1024 entries x 1 MiB = 1 GiB positive sum — accepted shape */
    const long long MEG = 1024LL * 1024;
    void *meg = mmap(NULL, (size_t)MEG, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (meg == MAP_FAILED)
        die("mmap meg");
    struct iovec *iovD = malloc(sizeof(struct iovec) * 1024);
    if (!iovD)
        die("malloc iovD");
    for (int i = 0; i < 1024; i++) {
        iovD[i].iov_base = meg;
        iovD[i].iov_len = (size_t)MEG;
    }
    open_in("so.dat", O_RDONLY, &fd);
    errno = 0;
    r = raw_preadv(fd, iovD, 1024, 0);
    obs("k1024_1mib_sum_1gib_ret", r);
    obs("k1024_1mib_sum_1gib_errno", errno);
    close(fd);
    free(iovD);
    munmap(meg, (size_t)MEG);

    munmap(page, (size_t)ps);
    cell_end("sum-overflow");
}

/* ------------------------------------------------------------------ */
/* null-empty                                                          */
/* ------------------------------------------------------------------ */

static void cell_null_empty(void) {
    make_pattern_file("ne.dat", 4096);
    unsigned char b[4096];
    struct iovec one = {b, sizeof b};

    int fd;
    open_in("ne.dat", O_RDONLY, &fd);

    errno = 0;
    ssize_t r = raw_preadv(fd, NULL, 0, 0);
    obs("read_iovcnt0_null_ret", r);
    obs("read_iovcnt0_null_errno", errno);

    errno = 0;
    r = raw_preadv(fd, &one, 0, 0);
    obs("read_iovcnt0_valid_ret", r);
    obs("read_iovcnt0_valid_errno", errno);

    errno = 0;
    r = raw_preadv(fd, NULL, 1, 0);
    obs("read_null_iovcnt1_ret", r);
    obs("read_null_iovcnt1_errno", errno);

    struct iovec z3[3] = {{b, 0}, {b, 0}, {b, 0}};
    errno = 0;
    r = raw_preadv(fd, z3, 3, 0);
    obs("read_all_zero_len_ret", r);
    obs("read_all_zero_len_errno", errno);
    close(fd);

    char wpath[512];
    snprintf(wpath, sizeof wpath, "%s/ne-w.dat", scratch);
    int wfd = open(wpath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0)
        die("open ne-w");
    errno = 0;
    r = raw_pwritev(wfd, NULL, 0, 0);
    obs("write_iovcnt0_ret", r);
    obs("write_iovcnt0_errno", errno);
    obs("write_iovcnt0_size_after", file_size(wfd));

    struct iovec z3w[3] = {{b, 0}, {b, 0}, {b, 0}};
    errno = 0;
    r = raw_pwritev(wfd, z3w, 3, 12345);
    obs("write_all_zero_len_ret", r);
    obs("write_all_zero_len_errno", errno);
    obs("write_all_zero_len_size_after", file_size(wfd));
    close(wfd);
    cell_end("null-empty");
}

/* ------------------------------------------------------------------ */
/* fsize-partial-write: real partial write + EFBIG via RLIMIT_FSIZE    */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_sigxfsz = 0;
static void on_sigxfsz(int sig) {
    (void)sig;
    g_sigxfsz++;
}

static void cell_fsize_partial_write(void) {
    /*
     * A lowered RLIMIT_FSIZE hard limit cannot be raised back without
     * privileges, so the limited writes run in a forked child. The child's
     * report goes through a pipe: RLIMIT_FSIZE would otherwise clamp the
     * child's own writes to the redirected stdout file as well.
     */
    static unsigned char src[4096];
    memset(src, 0x71, sizeof src);

    char path[512];
    snprintf(path, sizeof path, "%s/fp.dat", scratch);

    cell_begin("fsize-partial-write");
    int rpt[2];
    if (pipe(rpt) != 0)
        die("pipe");
    fflush(stdout); /* fork must not duplicate pending stdio buffers */
    pid_t pid = fork();
    if (pid < 0)
        die("fork");
    if (pid == 0) {
        if (dup2(rpt[1], STDOUT_FILENO) != 1)
            _exit(3);
        close(rpt[0]);
        struct sigaction sa = {0};
        sa.sa_handler = on_sigxfsz;
        if (sigaction(SIGXFSZ, &sa, NULL) != 0)
            _exit(3);
        struct rlimit rl = {100, 100};
        if (setrlimit(RLIMIT_FSIZE, &rl) != 0)
            _exit(4);
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            _exit(5);

        struct iovec iov[3] = {{src, sizeof src}, {src, sizeof src}, {src, sizeof src}};
        errno = 0;
        g_sigxfsz = 0;
        ssize_t r = raw_pwritev(fd, iov, 3, 0);
        obs("partial_ret", r);
        obs("partial_errno", errno);
        obs("partial_sigxfsz_count", g_sigxfsz);
        obs("partial_size_after", file_size(fd));
        unsigned char back[128];
        ssize_t pr = pread(fd, back, sizeof back, 0);
        int ok = pr == 100;
        for (int i = 0; ok && i < 100; i++)
            ok = back[i] == 0x71;
        obs("partial_prefix100_landed", ok);

        /* second attempt starting exactly at the limit */
        errno = 0;
        g_sigxfsz = 0;
        r = raw_pwritev(fd, iov, 3, 100);
        obs("efbig_ret", r);
        obs("efbig_errno", errno);
        obs("efbig_sigxfsz_count", g_sigxfsz);
        obs("efbig_size_after", file_size(fd));
        close(fd);
        fflush(stdout);
        _exit(0);
    }
    close(rpt[1]);
    static char report[4096];
    size_t got = 0;
    for (;;) {
        if (got >= sizeof report - 1)
            break;
        ssize_t n = read(rpt[0], report + got, sizeof report - 1 - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(rpt[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    report[got] = 0;
    fputs(report, stdout);
    obs("child_exit", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    cell_end("fsize-partial-write");
}

/* ------------------------------------------------------------------ */
/* eintr: seccomp-injected EINTR for SYS_preadv / SYS_pwritev          */
/* ------------------------------------------------------------------ */

static int install_eintr_filter(void) {
    struct sock_filter prog[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0), /* seccomp_data.nr */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)SYS_preadv, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned)SYS_pwritev, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (unsigned)EINTR),
    };
    struct sock_fprog fprog = {
        .len = (unsigned short)(sizeof prog / sizeof prog[0]),
        .filter = prog,
    };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return -1;
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fprog) != 0)
        return -1;
    return 0;
}

static void cell_eintr(void) {
    make_pattern_file("ei.dat", 8192);
    char wpath[512];
    snprintf(wpath, sizeof wpath, "%s/ei-w.dat", scratch);

    cell_begin("eintr");
    fflush(stdout); /* fork must not duplicate pending stdio buffers */
    pid_t pid = fork();
    if (pid < 0)
        die("fork");
    if (pid == 0) {
        if (install_eintr_filter() != 0)
            _exit(3);
        unsigned char b[4096];
        memset(b, 0x88, sizeof b);
        struct iovec iov[2] = {{b, sizeof b}, {b, sizeof b}};
        char rpath[512];
        snprintf(rpath, sizeof rpath, "%s/ei.dat", scratch);
        int fd = open(rpath, O_RDONLY);
        if (fd < 0)
            _exit(4);
        errno = 0;
        ssize_t r = raw_preadv(fd, iov, 2, 0);
        printf("obs child_read_ret=%lld\n", (long long)r);
        printf("obs child_read_errno=%d\n", errno);
        int guard = 1;
        for (size_t i = 0; i < sizeof b; i++)
            guard = guard && b[i] == 0x88;
        printf("obs child_read_buffer_guard_intact=%d\n", guard);
        close(fd);

        int wfd = open(wpath, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (wfd < 0)
            _exit(5);
        static unsigned char src[2048];
        memset(src, 0x66, sizeof src);
        struct iovec wiov[1] = {{src, sizeof src}};
        errno = 0;
        r = raw_pwritev(wfd, wiov, 1, 0);
        printf("obs child_write_ret=%lld\n", (long long)r);
        printf("obs child_write_errno=%d\n", errno);
        printf("obs child_write_size_after=%lld\n", file_size(wfd));
        close(wfd);
        fflush(stdout); /* _exit skips stdio flush; output is redirected */
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    obs("child_exit", WIFEXITED(status) ? WEXITSTATUS(status) : -1);

    /* parent control: same shapes without the filter succeed */
    unsigned char b[4096];
    struct iovec iov[1] = {{b, sizeof b}};
    int fd;
    open_in("ei.dat", O_RDONLY, &fd);
    errno = 0;
    ssize_t r = raw_preadv(fd, iov, 1, 0);
    obs("parent_control_read_ret", r);
    obs("parent_control_read_errno", errno);
    close(fd);

    int wfd = open(wpath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0)
        die("open parent w");
    static unsigned char src[2048];
    memset(src, 0x66, sizeof src);
    struct iovec wiov[1] = {{src, sizeof src}};
    errno = 0;
    r = raw_pwritev(wfd, wiov, 1, 0);
    obs("parent_control_write_ret", r);
    obs("parent_control_write_errno", errno);
    close(wfd);
    cell_end("eintr");
}

/* ------------------------------------------------------------------ */
/* cursor: preadv/pwritev must not move the shared cursor              */
/* ------------------------------------------------------------------ */

static void cell_cursor(void) {
    make_pattern_file("cu.dat", 16384);
    unsigned char b[4096];
    struct iovec iov[1] = {{b, sizeof b}};

    int fd;
    open_in("cu.dat", O_RDWR, &fd);
    ssize_t rr = read(fd, b, 100); /* move cursor to 100 */
    obs("seed_read_ret", rr);

    errno = 0;
    ssize_t r = raw_preadv(fd, iov, 1, 8192);
    obs("preadv_ret", r);
    obs("preadv_errno", errno);
    obs("cursor_after_preadv", (long long)lseek(fd, 0, SEEK_CUR));

    errno = 0;
    static unsigned char src[2048];
    memset(src, 0x11, sizeof src);
    struct iovec wiov[1] = {{src, sizeof src}};
    r = raw_pwritev(fd, wiov, 1, 8192);
    obs("pwritev_ret", r);
    obs("pwritev_errno", errno);
    obs("cursor_after_pwritev", (long long)lseek(fd, 0, SEEK_CUR));

    int dupfd = dup(fd);
    obs("cursor_through_dup", (long long)lseek(dupfd, 0, SEEK_CUR));
    close(dupfd);
    close(fd);

    open_in("cu.dat", O_RDONLY, &fd);
    errno = 0;
    r = raw_preadv(fd, iov, 1, (off_t)-1);
    obs("neg_offset_ret", r);
    obs("neg_offset_errno", errno);
    errno = 0;
    r = raw_preadv(fd, iov, 1, (off_t)INT64_MAX);
    obs("int64max_offset_ret", r);
    obs("int64max_offset_errno", errno);
    close(fd);

    open_in("cu.dat", O_RDONLY, &fd);
    rr = read(fd, b, 100);
    obs("cursor_read_advances_to", (long long)rr);
    close(fd);
    cell_end("cursor");
}

/* ------------------------------------------------------------------ */
/* atomicity: descriptive torn-observation probe (OQ-M1-1 evidence)    */
/* ------------------------------------------------------------------ */

#define REGION_SIZE (1024 * 1024)
#define REGION_OFF (1024 * 1024)
#define NCHUNK 8
#define CHUNK (REGION_SIZE / NCHUNK)

static atomic_int g_writer_go = 0;
static atomic_int g_writer_stop = 0;

static void *writer_thread(void *arg) {
    int fd = *(int *)arg;
    static unsigned char buf[REGION_SIZE];
    unsigned epoch = 1;
    memset(buf, (unsigned char)(epoch & 0xFF), REGION_SIZE);
    while (!atomic_load(&g_writer_go))
        sched_yield();
    while (!atomic_load(&g_writer_stop)) {
        epoch++;
        if ((epoch & 0xFF) == 0)
            epoch++;
        memset(buf, (unsigned char)(epoch & 0xFF), REGION_SIZE);
        if (pwrite(fd, buf, REGION_SIZE, REGION_OFF) != REGION_SIZE) {
            /* descriptive cell; write failures are not the observable */
        }
        struct timespec ts = {0, 50000}; /* 50 us */
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* pages inside buf whose tag differs from buf[0], plus 1 if buf[0] != region tag0 */
static int chunk_torn(const unsigned char *buf, size_t n, unsigned char tag0) {
    int torn = buf[0] != tag0;
    for (size_t o = 4096; o < n; o += 4096)
        if (buf[o] != buf[0])
            torn++;
    return torn;
}

static void cell_atomicity(void) {
    const int ITER = 5000;
    char path[512];
    snprintf(path, sizeof path, "%s/at.dat", scratch);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        die("open at");
    if (ftruncate(fd, REGION_OFF + REGION_SIZE) != 0)
        die("ftruncate at");

    static unsigned char bufs[NCHUNK][CHUNK];
    struct iovec iov[NCHUNK];
    for (int i = 0; i < NCHUNK; i++) {
        iov[i].iov_base = bufs[i];
        iov[i].iov_len = CHUNK;
        memset(bufs[i], 0, CHUNK);
    }

    pthread_t th;
    if (pthread_create(&th, NULL, writer_thread, &fd) != 0)
        die("pthread_create");
    atomic_store(&g_writer_go, 1);

    long long preadv_calls = 0, preadv_torn = 0, split_calls = 0, split_torn = 0;
    for (int i = 0; i < ITER; i++) {
        /* form P: one preadv of the whole region */
        ssize_t r = raw_preadv(fd, iov, NCHUNK, REGION_OFF);
        if (r == REGION_SIZE) {
            preadv_calls++;
            int torn = 0;
            unsigned char tag0 = bufs[0][0];
            for (int c = 0; c < NCHUNK; c++)
                torn += chunk_torn(bufs[c], CHUNK, tag0);
            if (torn > 0)
                preadv_torn++;
        }
        /* form S: eight separate preads */
        int ok = 1, torn = 0;
        for (int c = 0; c < NCHUNK; c++) {
            ssize_t s = pread(fd, bufs[c], CHUNK, REGION_OFF + (long long)c * CHUNK);
            if (s != CHUNK) {
                ok = 0;
                break;
            }
        }
        if (ok) {
            split_calls++;
            unsigned char tag0 = bufs[0][0];
            for (int c = 0; c < NCHUNK; c++)
                torn += chunk_torn(bufs[c], CHUNK, tag0);
            if (torn > 0)
                split_torn++;
        }
    }
    atomic_store(&g_writer_stop, 1);
    pthread_join(th, NULL);
    close(fd);
    unlink(path);

    obs("iterations_requested", ITER);
    obs("preadv_complete_calls", preadv_calls);
    obs("preadv_torn_observations", preadv_torn);
    obs("split_complete_calls", split_calls);
    obs("split_torn_observations", split_torn);
    cell_end("atomicity");
}

/* ------------------------------------------------------------------ */
/* nonseekable                                                         */
/* ------------------------------------------------------------------ */

static void cell_nonseekable(void) {
    int pfd[2];
    if (pipe(pfd) != 0)
        die("pipe");
    unsigned char b[16];
    struct iovec iov[1] = {{b, sizeof b}};
    errno = 0;
    ssize_t r = raw_preadv(pfd[0], iov, 1, 0);
    obs("pipe_preadv_ret", r);
    obs("pipe_preadv_errno", errno);
    errno = 0;
    r = raw_pwritev(pfd[1], iov, 1, 0);
    obs("pipe_pwritev_ret", r);
    obs("pipe_pwritev_errno", errno);
    close(pfd[0]);
    close(pfd[1]);
    cell_end("nonseekable");
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <cell> [scratch_dir]\n", argv[0]);
        return 2;
    }
    if (argc >= 3)
        scratch = argv[2];
    scratch_prepare();

    const char *cell = argv[1];
    struct {
        const char *name;
        void (*fn)(void);
    } cells[] = {
        {"env", cell_env},
        {"array-order", cell_array_order},
        {"short-read", cell_short_read},
        {"iov-fault-read", cell_iov_fault_read},
        {"iov-fault-write", cell_iov_fault_write},
        {"iovmax", cell_iovmax},
        {"sum-overflow", cell_sum_overflow},
        {"null-empty", cell_null_empty},
        {"fsize-partial-write", cell_fsize_partial_write},
        {"eintr", cell_eintr},
        {"cursor", cell_cursor},
        {"atomicity", cell_atomicity},
        {"nonseekable", cell_nonseekable},
    };
    if (strcmp(cell, "all") == 0) {
        for (unsigned i = 0; i < sizeof cells / sizeof cells[0]; i++)
            cells[i].fn();
        return 0;
    }
    for (unsigned i = 0; i < sizeof cells / sizeof cells[0]; i++) {
        if (strcmp(cell, cells[i].name) == 0) {
            cells[i].fn();
            return 0;
        }
    }
    fprintf(stderr, "unknown cell: %s\n", cell);
    return 2;
}
