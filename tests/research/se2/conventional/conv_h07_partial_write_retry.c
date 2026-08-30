/* conv_h07_partial_write_retry.c — SE-2 conventional probe P-C07 (family H07).
 *
 * SE1-CA-H07-1 normalized trace (partial transfer; retrying with different
 * arguments corrupts the stream), raw POSIX pipe spelling. The program obeys
 * normal POSIX usage EXCEPT the ONE normalized hazard: after a partial write
 * of k < n bytes, the retry re-sends from the WRONG offset (start of the
 * payload instead of payload + k).
 *
 * Expected (pre-registered): the drained stream contains the first 4096-byte
 * block TWICE and the second block never — silent stream corruption. No
 * POSIX layer objects. MISSES.
 *
 * Deterministic: F_SETPIPE_SZ pins the pipe capacity to 4096 so the first
 * nonblocking write of 8192 bytes is guaranteed partial at 4096.
 *
 * RESEARCH PROBE — not part of any test group; run per the SE-2 plan.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031 /* Linux: set pipe capacity */
#endif

int main(void) {
    int fds[2];
    if (pipe(fds) != 0) {
        perror("pipe");
        return 2;
    }
    if (fcntl(fds[1], F_SETPIPE_SZ, 4096) != 4096) {
        perror("F_SETPIPE_SZ");
        return 2;
    }
    if (fcntl(fds[1], F_SETFL, O_NONBLOCK) != 0) {
        perror("O_NONBLOCK");
        return 2;
    }
    if (fcntl(fds[0], F_SETFL, O_NONBLOCK) != 0) {
        perror("O_NONBLOCK(read end)");
        return 2;
    }

    /* Two distinguishable 4096-byte blocks. */
    unsigned char* payload = malloc(8192);
    if (!payload) return 2;
    for (int i = 0; i < 8192; ++i) {
        payload[i] = (i < 4096) ? 'A' : 'B';
    }

    /* Partial transfer: k = 4096 of n = 8192. */
    ssize_t k = write(fds[1], payload, 8192);
    if (k != 4096) {
        printf("SE2-C07: environment did not produce the expected partial write (k=%zd)\n", k);
        return 2;
    }

    /* THE VIOLATION: the retry re-sends from the START of the payload instead
     * of payload + k with the remaining 4096 bytes. (Drain first so the pipe
     * has room — the wrong-arguments hazard is the offset, not the blocking.) */
    unsigned char drained[4096];
    if (read(fds[0], drained, 4096) != 4096) {
        perror("read");
        return 2;
    }
    ssize_t r = write(fds[1], payload, 4096);  /* WRONG: should be payload + 4096 */
    if (r != 4096) {
        printf("SE2-C07: retry did not complete (r=%zd)\n", r);
        return 2;
    }

    /* Consume the delivered stream: block-1 was already drained above; the
     * wrong retry delivered 4096 more bytes. The correct 8192-byte stream
     * would have been block-1 then block-2; what the consumer actually gets
     * across its two reads is block-1 twice. */
    unsigned char* stream = malloc(8192);
    if (!stream) return 2;
    memcpy(stream, drained, 4096);
    ssize_t total = 4096;
    while (total < 8192) {
        ssize_t n = read(fds[0], stream + total, (size_t)(8192 - total));
        if (n < 0) break;  /* EAGAIN: pipe empty — the tail was never written */
        if (n == 0) break;
        total += n;
    }

    int first_block_ok = 1, second_block_duplicated = 1, second_block_absent = 1;
    for (int i = 0; i < 4096; ++i) {
        if (stream[i] != 'A') first_block_ok = 0;
        if (stream[4096 + i] != 'A') second_block_duplicated = 0;  /* would be 'B' if correct */
        if (stream[4096 + i] == 'B') second_block_absent = 0;
    }

    if (first_block_ok && second_block_duplicated && second_block_absent) {
        printf(
            "SE2-C07: stream delivered block-1 twice and block-2 never (bytes 4096..8191 are a duplicate of block-1) "
            "— POSIX LAYERS: no rejection, no detection, silent stream corruption (MISSES)\n");
    } else {
        printf("SE2-C07: unexpected stream shape first_ok=%d dup=%d second_absent=%d\n",
               first_block_ok, second_block_duplicated, second_block_absent);
        free(payload);
        free(stream);
        close(fds[0]);
        close(fds[1]);
        return 2;
    }

    free(payload);
    free(stream);
    close(fds[0]);
    close(fds[1]);
    return 0;
}
