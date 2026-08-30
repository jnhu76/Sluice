/* conv_h03_uring_fd_reuse.c — SE-2 conventional probe P-C03 (family H03).
 *
 * SE1-CA-H03-1 normalized trace (completion races resource close / identity
 * lingers past close), raw io_uring spelling, user-space half. The program
 * obeys normal liburing usage EXCEPT the ONE normalized hazard: an operation
 * is aimed at an fd whose lifetime outlives the operation is assumed, but the
 * fd is closed and its number reused BEFORE submission/execution — so the
 * I/O silently targets the WRONG resource.
 *
 * Expected (pre-registered): the read SQE prepared against the pipe read end
 * executes after the fd number was recycled by a victim file; the buffer
 * receives the FILE's bytes. Silent wrong-target I/O; no layer objects.
 * MISSES. Fully deterministic (lowest-available fd numbering).
 *
 * This is the user-space half of H03. The kernel-side fixed-file UAF
 * (CVE-class, fixed upstream) is NOT reproduced here — it is below any
 * user-space probe's reach; the matrix records it as BLOCKED.
 *
 * RESEARCH PROBE — not part of any test group; run per the SE-2 plan.
 */
#include <fcntl.h>
#include <liburing.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char victim_path[128];
    snprintf(victim_path, sizeof(victim_path), "/tmp/se2_h03_victim_%d.txt", (int)getpid());

    int fds[2];
    if (pipe(fds) != 0) {
        perror("pipe");
        return 2;
    }
    const int pipe_read_fd = fds[0];

    /* The operation is prepared against the pipe read end... */
    struct io_uring ring;
    if (io_uring_queue_init(4, &ring, 0) != 0) {
        perror("io_uring_queue_init");
        return 2;
    }
    unsigned char buf[16];
    memset(buf, 0, sizeof(buf));
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_read(sqe, pipe_read_fd, buf, sizeof(buf) - 1, 0);
    sqe->user_data = 1;

    /* ...but the fd is closed and its NUMBER recycled before the SQE is
     * submitted (the normalized in-window close). */
    close(pipe_read_fd);
    int victim = open(victim_path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (victim != pipe_read_fd) {
        printf("SE2-C03: environment did not recycle fd number (victim=%d, expected %d)\n",
               victim, pipe_read_fd);
        if (victim >= 0) close(victim);
        unlink(victim_path);
        io_uring_queue_exit(&ring);
        close(fds[1]);
        return 2;
    }
    static const char data[12] = "VICTIM-DATA";
    if (pwrite(victim, data, sizeof(data), 0) != (ssize_t)sizeof(data)) {
        perror("pwrite");
        return 2;
    }

    /* Submit + complete: the read now executes against the recycled number. */
    if (io_uring_submit(&ring) != 1) {
        perror("io_uring_submit");
        return 2;
    }
    struct io_uring_cqe* cqe = NULL;
    if (io_uring_wait_cqe(&ring, &cqe) != 0) {
        perror("io_uring_wait_cqe");
        return 2;
    }
    int res = cqe->res;
    io_uring_cqe_seen(&ring, cqe);

    if (res == (int)sizeof(data) && memcmp(buf, data, sizeof(data)) == 0) {
        printf(
            "SE2-C03: read submitted against the pipe consumed the FILE's bytes (\"%s\") after fd-number reuse "
            "— RAW IO_URING LAYERS: no rejection, no detection, silent wrong-target I/O (MISSES)\n",
            (const char*)buf);
    } else {
        printf("SE2-C03: unexpected outcome res=%d buf=\"%.15s\" — hazard not exercised cleanly\n", res, (const char*)buf);
        io_uring_queue_exit(&ring);
        close(victim);
        close(fds[1]);
        unlink(victim_path);
        return 2;
    }

    io_uring_queue_exit(&ring);
    close(victim);
    close(fds[1]);
    unlink(victim_path);
    return 0;
}
