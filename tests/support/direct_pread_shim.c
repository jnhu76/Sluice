#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

ssize_t pread(int fd, void* buffer, size_t count, off_t offset) {
    const char* limit = getenv("SLUICE_DIRECT_READ_LIMIT");
    const char* inode = getenv("SLUICE_DIRECT_FAIL_INODE");
    if (inode && offset >= 34) {
        struct stat info;
        if (fstat(fd, &info) == 0 && info.st_ino == strtoull(inode, NULL, 10)) {
            errno = EIO;
            return -1;
        }
    }
    if (limit) {
        const size_t maximum = strtoull(limit, NULL, 10);
        if (maximum && count > maximum)
            count = maximum;
    }
    return syscall(SYS_pread64, fd, buffer, count, offset);
}
