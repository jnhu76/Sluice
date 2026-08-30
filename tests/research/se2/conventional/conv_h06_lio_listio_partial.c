/* conv_h06_lio_listio_partial.c — SE-2 conventional probe P-C06 (family H06).
 *
 * SE1-CA-H06-1 normalized trace (batch submission whose per-entry outcomes
 * surface on a separate channel; failed submission leaves residue that the
 * caller must reconcile), POSIX lio_listio spelling, invalid-entry shape.
 *
 * MEASURED HOST BEHAVIOR (probe bring-up, 2026-08-30, glibc aio):
 *   lio_listio(LIO_WAIT) with one invalid entry returns -1/EIO and executes
 *   NOTHING — the file stays empty and the valid entries' aio_return is
 *   never set. On this host, invalid-entry initiation is ATOMIC: the
 *   batch-level failure gives no per-entry identification, but no residue
 *   exists either, so the documented all-or-nothing recovery is safe here.
 *
 * POSIX itself allows a listio call to initiate a subset of the list (the
 * normalized partial-initiation hazard class), and valid entries can still
 * fail asynchronously at completion (ENOSPC class) — that per-entry
 * reconciliation obligation remains documented-only; this probe cannot
 * trigger deterministic mid-list initiation failure or injected ENOSPC on
 * this host, and the matrix records that half accordingly.
 *
 * Expected (pre-registered): batch with invalid entry -> -1/EIO, zero
 * execution (conventional mechanism PREVENTS the residue for this shape);
 * clean resubmission of the valid batch -> single execution. MISSES is NOT
 * the outcome for the invalid-entry shape on this host — recorded per §37.
 *
 * RESEARCH PROBE — not part of any test group; run per the SE-2 plan.
 */
#include <aio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof(path), "/tmp/se2_h06_batch_%d.txt", (int)getpid());
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) {
        perror("open");
        return 2;
    }

    struct aiocb cbs[3];
    struct aiocb* list[3];
    memset(cbs, 0, sizeof(cbs));
    for (int i = 0; i < 3; ++i) {
        cbs[i].aio_fildes = (i == 2) ? -1 : fd;  /* entry 2 invalid */
        cbs[i].aio_buf = (i == 0) ? (void*)"A-" : (void*)"B-";
        cbs[i].aio_nbytes = 2;
        cbs[i].aio_offset = 0;
        cbs[i].aio_sigevent.sigev_notify = SIGEV_NONE;
        list[i] = &cbs[i];
    }
    errno = 0;
    int list_rc = lio_listio(LIO_WAIT, list, 3, NULL);

    char content[32];
    memset(content, 0, sizeof(content));
    ssize_t n = pread(fd, content, sizeof(content) - 1, 0);

    /* Did the valid entries execute despite the batch failure? */
    ssize_t e0 = aio_return(&cbs[0]);
    if (list_rc != 0 && n == 0 && e0 == 0 /* aio_return unset => never ran */) {
        printf(
            "SE2-C06: batch with one invalid entry: lio_listio rc=-1 errno=%d; file empty (n=%zd), valid entries never ran "
            "— GLIBC LIO INITIATION IS ATOMIC for this shape: batch-level rejection with no residue; "
            "conventional mechanism matches Sluice transactional submission here (PREVENTS at this shape; "
            "per-entry async failure reconciliation remains documented-only)\n",
            errno, n);
        close(fd);
        unlink(path);
        return 0;
    }

    printf(
        "SE2-C06: unexpected shape list_rc=%d errno=%d n=%zd e0=%zd — measured host behavior differs from bring-up record\n",
        list_rc, errno, n, e0);
    close(fd);
    unlink(path);
    return 2;
}
