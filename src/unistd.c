#include <pagh.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <pagh_internal.h>
#include <stdarg.h>

static long ret(long r) { if (r < 0) { errno = (int)-r; return -1; } return r; }

long read(int fd, void *buf, size_t n) {
    return ret(__pagh_syscall3(SYS_READ, fd, (long)buf, (long)n));
}
long write(int fd, const void *buf, size_t n) {
    return ret(__pagh_syscall3(SYS_WRITE, fd, (long)buf, (long)n));
}
int open(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap);
    }
    char buf[512];
    const char *abs = __pagh_abs(path, buf, sizeof buf);
    return (int)ret(__pagh_syscall6(SYS_OPENAT, AT_FDCWD, (long)abs, flags, mode, 0, 0));
}
int close(int fd) { return (int)ret(__pagh_syscall3(SYS_CLOSE, fd, 0, 0)); }
long lseek(int fd, long off, int whence) {
    return ret(__pagh_syscall6(SYS_LSEEK, fd, off, whence, 0, 0, 0));
}
int unlink(const char *path) {
    return (int)ret(__pagh_syscall6(SYS_UNLINKAT, AT_FDCWD, (long)__pagh_abs(path, (char[512]){0}, 512), 0, 0, 0, 0));
}
int rmdir(const char *path) {
    return (int)ret(__pagh_syscall6(SYS_UNLINKAT, AT_FDCWD, (long)__pagh_abs(path, (char[512]){0}, 512), AT_REMOVEDIR, 0, 0, 0));
}
int mkdir(const char *path, int mode) {
    return (int)ret(__pagh_syscall6(SYS_MKDIRAT, AT_FDCWD, (long)__pagh_abs(path, (char[512]){0}, 512), mode, 0, 0, 0));
}
int rename(const char *old, const char *new_) {
    return (int)ret(__pagh_syscall6(SYS_RENAMEAT2, AT_FDCWD, (long)__pagh_abs(old, (char[512]){0}, 512), AT_FDCWD, (long)__pagh_abs(new_, (char[512]){0}, 512), 0, 0));
}
int symlink(const char *target, const char *path) {
    return (int)ret(__pagh_syscall6(SYS_SYMLINKAT, (long)target, AT_FDCWD, (long)path, 0, 0, 0));
}
int link(const char *old, const char *new_) {
    return (int)ret(__pagh_syscall6(SYS_LINKAT, AT_FDCWD, (long)old, AT_FDCWD, (long)new_, 0, 0));
}
int chmod(const char *path, int mode) {
    return (int)ret(__pagh_syscall6(SYS_FCHMODAT, AT_FDCWD, (long)path, mode, 0, 0, 0));
}
int ftruncate(int fd, long len) {
    return (int)ret(__pagh_syscall6(SYS_FTRUNCATE, fd, len, 0, 0, 0, 0));
}
int truncate(const char *path, long len) {
    int fd = open(path, O_WRONLY, 0);
    if (fd < 0) return -1;
    int r = ftruncate(fd, len);
    int e = errno;
    close(fd);
    errno = e;
    return r;
}
int getpid(void) { return (int)__pagh_syscall3(SYS_GETPID, 0, 0, 0); }
void _exit(int code) { __pagh_syscall3(SYS_EXIT, code, 0, 0); for (;;) {} }
void exit(int code) { _exit(code); }
long sysconf(int name) { (void)name; return 4096; }
