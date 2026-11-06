#include <pagh.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

int fstat(int fd, struct pagh_stat *st) {
    long r = __pagh_syscall3(SYS_FSTAT, fd, (long)st, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}
int stat(const char *path, struct pagh_stat *st) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    int r = fstat(fd, st);
    int e = errno;
    close(fd);
    errno = e;
    return r;
}
