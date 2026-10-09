/* e4 (#474) test-only LD_PRELOAD interposer for the real sluice-tail child.
 *
 * Active only when every SLUICE_TAIL_SHIM_* variable is present; the parent
 * harness gates it to one target file (dev/ino) and one fifo pair. PRE /
 * ENTERED / EXIT events go to the event fifo; a single 'R' byte on the
 * command fifo releases the held read. Forwarding uses SYS_pread64 directly
 * so no dlsym recursion is possible; a hold never runs under the shim's own
 * locking because the shim keeps none.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

static dev_t shim_dev;
static ino_t shim_ino;
static int shim_evt_fd = -1;
static int shim_cmd_fd = -1;
static long shim_hold_from = -1;
static long shim_seq;
static int shim_ready;

static void shim_init(void) {
    const char* dev = getenv("SLUICE_TAIL_SHIM_DEV");
    const char* ino = getenv("SLUICE_TAIL_SHIM_INO");
    const char* evt = getenv("SLUICE_TAIL_SHIM_EVT");
    const char* cmd = getenv("SLUICE_TAIL_SHIM_CMD");
    const char* hold = getenv("SLUICE_TAIL_SHIM_HOLD_FROM");
    if (!dev || !ino || !evt || !cmd)
        return;
    shim_dev = (dev_t)strtoull(dev, NULL, 0);
    shim_ino = (ino_t)strtoull(ino, NULL, 0);
    shim_hold_from = hold ? strtol(hold, NULL, 0) : -1;
    shim_evt_fd = open(evt, O_RDWR);
    shim_cmd_fd = cmd ? open(cmd, O_RDWR) : -1;
    shim_ready = shim_evt_fd >= 0;
}

static int shim_is_target(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0)
        return 0;
    return st.st_dev == shim_dev && st.st_ino == shim_ino;
}

static void shim_send(const char* ev) {
    size_t off = 0;
    size_t len = strlen(ev);
    while (off < len) {
        ssize_t r = write(shim_evt_fd, ev + off, len - off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        off += (size_t)r;
    }
}

static void shim_wait_release(void) {
    for (;;) {
        char b = 0;
        ssize_t r = read(shim_cmd_fd, &b, 1);
        if (r == 1 && b == 'R')
            return;
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            /* Command fifo closed early: park; the harness watchdog owns the
             * process from here. */
            for (;;)
                pause();
        }
    }
}

ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
    if (!shim_ready) {
        shim_init();
        if (!shim_ready)
            return (ssize_t)syscall(SYS_pread64, fd, buf, count, offset);
    }
    if (!shim_is_target(fd))
        return (ssize_t)syscall(SYS_pread64, fd, buf, count, offset);

    long seq = ++shim_seq;
    char ev[128];
    snprintf(ev, sizeof ev, "PRE %ld %ld %zu\n", seq, (long)offset, count);
    shim_send(ev);

    int hold = shim_hold_from >= 0 && seq >= shim_hold_from;
    if (hold) {
        snprintf(ev, sizeof ev, "ENTERED %ld %ld %zu\n", seq, (long)offset, count);
        shim_send(ev);
        shim_wait_release();
    }

    ssize_t r = (ssize_t)syscall(SYS_pread64, fd, buf, count, offset);
    snprintf(ev, sizeof ev, "EXIT %ld %ld\n", seq, (long)r);
    shim_send(ev);
    return r;
}
