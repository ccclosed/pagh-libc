#include <pagh.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

pid_t fork(void) {
    long r = __pagh_syscall3(SYS_FORK, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (pid_t)r;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    long r = __pagh_syscall6(SYS_EXECVE, (long)path, (long)argv, (long)envp, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

pid_t waitpid(pid_t pid, int *status, int options) {
    long r = __pagh_syscall6(SYS_WAIT4, pid, (long)status, options, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (pid_t)r;
}

pid_t getppid(void) {
    long r = __pagh_syscall3(SYS_GETPPID, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (pid_t)r;
}
