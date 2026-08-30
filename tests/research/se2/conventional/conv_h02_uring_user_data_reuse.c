/* conv_h02_uring_user_data_reuse.c — SE-2 conventional probe P-C02 (family H02).
 *
 * SE1-CA-H02-1 normalized trace, raw io_uring spelling. The program obeys
 * normal liburing usage EXCEPT the ONE normalized hazard: an in-flight
 * request identity (user_data) is reused for a second operation, and the
 * caller's bookkeeping assumes identity is 1:1 with a logical request. The
 * two completions that arrive under one identity are then misattributed.
 *
 * Expected (pre-registered): plain run — two CQEs carry the SAME user_data;
 * the caller's single-logical-request map double-consumes / misattributes;
 * no layer objects (liburing documents uniqueness as a caller obligation
 * only). MISSES. There is no sanitizer dimension for this hazard class at
 * this layer (no memory error, no data race).
 *
 * Deterministic: both ops read from a pipe that already holds data; both are
 * submitted before any CQE is consumed.
 *
 * RESEARCH PROBE — not part of any test group; run per the SE-2 plan.
 */
#include <liburing.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define USER_DATA 0x1000u

int main(void) {
    struct io_uring ring;
    if (io_uring_queue_init(8, &ring, 0) != 0) {
        perror("io_uring_queue_init");
        return 2;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        perror("pipe");
        return 2;
    }
    static const char msg[13] = "HELLO-SE2-H02";
    if (write(fds[1], msg, sizeof(msg)) != (ssize_t)sizeof(msg) ||
        write(fds[1], msg, sizeof(msg)) != (ssize_t)sizeof(msg)) {
        perror("write");
        return 2;
    }

    unsigned char b1[13], b2[13];
    memset(b1, 0, sizeof(b1));
    memset(b2, 0, sizeof(b2));

    /* Op 1 in flight under identity USER_DATA... */
    struct io_uring_sqe* sqe1 = io_uring_get_sqe(&ring);
    io_uring_prep_read(sqe1, fds[0], b1, sizeof(b1), 0);
    sqe1->user_data = USER_DATA;

    /* ...and the SAME identity handed to op 2 while op 1 is still in flight
     * (op 1's CQE has not been consumed). Normal io_uring usage otherwise. */
    struct io_uring_sqe* sqe2 = io_uring_get_sqe(&ring);
    io_uring_prep_read(sqe2, fds[0], b2, sizeof(b2), 0);
    sqe2->user_data = USER_DATA;

    if (io_uring_submit(&ring) != 2) {
        perror("io_uring_submit");
        return 2;
    }

    /* Caller bookkeeping that assumes identity -> one logical request, one
     * terminal (the normalized defect). */
    int logical_request_terminals = 0;
    int cqes_under_identity = 0;

    for (int i = 0; i < 2; ++i) {
        struct io_uring_cqe* cqe = NULL;
        if (io_uring_wait_cqe(&ring, &cqe) != 0) {
            perror("io_uring_wait_cqe");
            return 2;
        }
        ++cqes_under_identity;
        if (cqe->user_data == USER_DATA) {
            if (cqe->res >= 0) {
                ++logical_request_terminals;  /* "request USER_DATA finished" */
            }
        }
        io_uring_cqe_seen(&ring, cqe);
    }

    printf(
        "SE2-C02: %d CQEs arrived under one reused identity; caller bookkeeping recorded %d terminals for ONE logical request "
        "— RAW IO_URING LAYERS: no rejection, no detection; identity misuse is documented-only (MISSES)\n",
        cqes_under_identity, logical_request_terminals);

    io_uring_queue_exit(&ring);
    close(fds[0]);
    close(fds[1]);
    return (logical_request_terminals > 1) ? 0 : 2;
}
