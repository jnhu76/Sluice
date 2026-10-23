/* Minimal io_uring+eventfd+epoll repro: buffered tmpfs reads, kernel CQE ->
 * registered eventfd -> epoll. Single thread. No Sluice. Watchdog via alarm. */
#include <liburing.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char bufs[8][4096];
static volatile int stuck;

static void onalarm(int sig) { (void)sig; stuck = 1; }

int main(void) {
    struct io_uring ring;
    if (io_uring_queue_init(8, &ring, 0) != 0) { perror("queue_init"); return 2; }
    int efd = eventfd(0, EFD_NONBLOCK);
    if (efd < 0) { perror("eventfd"); return 2; }
    if (io_uring_register_eventfd(&ring, efd) != 0) { perror("register_eventfd"); return 2; }
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = efd };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) != 0) { perror("epoll_ctl"); return 2; }

    int fd = open("/tmp/e5-fixture/w03-uring-4k-d4.bin", O_RDONLY);
    if (fd < 0) { perror("open fixture"); return 2; }

    signal(SIGALRM, onalarm);
    const int total = 20000;
    int submitted = 0, completed = 0, hangs = 0;
    while (completed < total) {
        while (submitted < total && submitted - completed < 4) {
            struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
            if (!sqe) break;
            off_t off = (off_t)(submitted % 2000) * 4096;
            io_uring_prep_read(sqe, fd, bufs[submitted % 8], 4096, off);
            io_uring_sqe_set_data64(sqe, (unsigned)(submitted + 1));
            ++submitted;
        }
        int rc = io_uring_submit(&ring);
        if (rc < 0) { fprintf(stderr, "submit: %s\n", strerror(-rc)); return 3; }
        struct epoll_event out;
        alarm(20);
        int ready = epoll_wait(ep, &out, 1, -1);
        alarm(0);
        if (ready <= 0) {
            if (stuck) {
                fprintf(stdout, "HANG: epoll_wait timed out via SIGALRM; submitted=%d completed=%d\n",
                        submitted, completed);
                ++hangs;
                stuck = 0;
                /* drain whatever completed so the loop can continue */
                struct io_uring_cqe* cqe;
                unsigned head, n = 0;
                io_uring_for_each_cqe(&ring, head, cqe) { ++completed; ++n; }
                io_uring_cq_advance(&ring, n);
                if (n == 0 && completed < total) {
                    fprintf(stdout, "CONFIRMED_STALL: no CQEs, %d submitted, %d completed\n",
                            submitted, completed);
                    break;
                }
                continue;
            }
            break;
        }
        /* drain eventfd then reap CQEs */
        unsigned long long v;
        (void)!read(efd, &v, sizeof v);
        struct io_uring_cqe* cqe;
        unsigned head, n = 0;
        io_uring_for_each_cqe(&ring, head, cqe) { ++completed; ++n; }
        io_uring_cq_advance(&ring, n);
    }
    fprintf(stdout, "done completed=%d hangs=%d\n", completed, hangs);
    return hangs > 0 ? 1 : 0;
}
