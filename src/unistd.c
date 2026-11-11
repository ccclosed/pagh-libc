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
/* dup/dup2 keep the kernel's descriptor rules: dup2(oldfd, oldfd) is a
 * successful no-op, dup2's third argument (flags) must be 0, and both report
 * failures through errno. */
int dup(int fd) {
    return (int)ret(__pagh_syscall3(SYS_DUP, fd, 0, 0));
}
int dup2(int oldfd, int newfd) {
    return (int)ret(__pagh_syscall6(SYS_DUP2, oldfd, newfd, 0, 0, 0, 0));
}
/* pipe2's flags must be 0 on this ABI; a non-zero `flags` is -EINVAL from the
 * kernel, not silently ignored. */
int pipe2(int fds[2], int flags) {
    return (int)ret(__pagh_syscall6(SYS_PIPE2, (long)fds, flags, 0, 0, 0, 0));
}
int pipe(int fds[2]) {
    return pipe2(fds, 0);
}
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
void __run_atexit(void);

/* POSIX `_exit` ends the *process*, not just the calling thread: with a live
 * pthread, SYS_EXIT (93) would leave the other threads holding the run open
 * (the kernel ends it only when the last thread exits), so the shell's `run`
 * would never return. SYS_EXIT_GROUP (94) is the ABI's whole-run call.
 * `pthread_exit` and the created-thread trampoline keep SYS_EXIT on purpose.
 *
 * `exit` is the POSIX one: it runs the LIFO atexit handlers first (crto does
 * the same before it calls `exit` for a program whose main returns, and
 * `__run_atexit` drains the list, so the double call is harmless), then exits
 * the process. Calling exit() used to skip the handlers entirely. */
void _exit(int code) { __pagh_syscall3(SYS_EXIT_GROUP, code, 0, 0); for (;;) {} }
/* set_tid_address records the clear-child-tid address for this thread and
 * returns its tid. The kernel documents that it does not clear the word on
 * exit, so the wrapper promises nothing beyond that. */
int set_tid_address(int *tidptr) {
    return (int)ret(__pagh_syscall3(SYS_SET_TID_ADDRESS, (long)tidptr, 0, 0));
}
void exit(int code) { __run_atexit(); _exit(code); }
long sysconf(int name) { (void)name; return 4096; }
