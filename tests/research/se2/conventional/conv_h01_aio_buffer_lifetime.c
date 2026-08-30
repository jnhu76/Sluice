/* conv_h01_aio_buffer_lifetime.c — SE-2 conventional probe P-C01 (family H01).
 *
 * SE1-CA-H01-1 normalized trace, POSIX AIO spelling. The program obeys normal
 * POSIX AIO usage EXCEPT the ONE normalized hazard: the borrowed target
 * buffer is freed while the aio_read is still in flight. The post-completion
 * read of the freed buffer is the normalized final trace step ("completion
 * later accesses R").
 *
 * Expected (pre-registered, SE-2 §45):
 *   plain: silent — completion reports 13 bytes "successfully" into freed
 *          memory; the deref prints garbage. No layer objects. MISSES.
 *   ASan : heap-use-after-free at the post-completion deref. DETECTS.
 *          (The kernel's own write into the freed pages is outside ASan's
 *          sight; the instrumented deref is the observable access.)
 *
 * Deterministic: the aio stays pending because the pipe is empty until the
 * main thread writes into it — after the free.
 *
 * RESEARCH PROBE — not part of any test group; run per the SE-2 plan.
 */
#include <aio.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    int fds[2];
    if (pipe(fds) != 0) {
        perror("pipe");
        return 2;
    }

    unsigned char* buf = malloc(64);
    if (!buf) return 2;
    memset(buf, 0xAB, 64);

    struct aiocb cb;
    memset(&cb, 0, sizeof(cb));
    cb.aio_fildes = fds[0];
    cb.aio_buf = buf;
    cb.aio_nbytes = 64;
    cb.aio_offset = 0;
    cb.aio_sigevent.sigev_notify = SIGEV_NONE;

    if (aio_read(&cb) != 0) {
        perror("aio_read");
        return 2;
    }

    /* THE VIOLATION: free the borrow while the read is in flight. */
    free(buf);

    static const char msg[13] = "HELLO-SE2-H01";
    if (write(fds[1], msg, sizeof(msg)) != (ssize_t)sizeof(msg)) {
        perror("write");
        return 2;
    }

    /* Wait for completion (bounded). */
    for (int i = 0; i < 5000 && aio_error(&cb) == EINPROGRESS; ++i) {
        usleep(1000);
    }
    ssize_t n = aio_return(&cb);
    if (n < 0) {
        printf("SE2-C01: aio failed with errno=%d (hazard not exercised)\n", errno);
        return 2;
    }

    /* Normalized final step: the program consumes the completed read — from
     * the freed borrow. */
    volatile unsigned char first = buf[0];
    printf(
        "SE2-C01: completion reported %zd bytes delivered; first byte of the freed borrow read back as 0x%02x "
        "— POSIX AIO LAYERS: no rejection, no detection, silent corruption (MISSES)\n",
        n, first);

    close(fds[0]);
    close(fds[1]);
    return 0;
}
