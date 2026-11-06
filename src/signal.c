#include <pagh.h>
#include <signal.h>
#include <errno.h>
#include <string.h>

/* The kernel requires SA_RESTORER: the handler's `ret` jumps here, and this
   stub executes rt_sigreturn with the frame the kernel armed. */
static void sigrestorer_c(void) { __asm__ volatile("li a7, 139\n\tecall"); }

static long sigaction_raw(int signum, const struct sigaction *act, struct sigaction *old) {
    struct sigaction copy;
    if (act) {
        copy = *act;
        if (copy.sa_handler != SIG_DFL && copy.sa_handler != SIG_IGN) {
            copy.sa_flags |= SA_RESTORER;
            copy.sa_restorer = sigrestorer_c;
        }
        return __pagh_syscall6(SYS_RT_SIGACTION, signum, (long)&copy,
                               (long)old, 8, 0, 0);
    }
    return __pagh_syscall6(SYS_RT_SIGACTION, signum, 0, (long)old, 8, 0, 0);
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    long r = sigaction_raw(signum, act, oldact);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

void (*signal(int signum, void (*handler)(int)))(int) {
    struct sigaction act, old;
    memset(&act, 0, sizeof act);
    act.sa_handler = handler;
    if (sigaction(signum, &act, &old) < 0) return SIG_ERR;
    return old.sa_handler;
}

int raise(int sig) {
    long tgid = __pagh_syscall3(SYS_GETPID, 0, 0, 0);
    long tid = __pagh_syscall3(SYS_GETTID, 0, 0, 0);
    long r = __pagh_syscall6(SYS_TGKILL, tgid, tid, sig, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}
int kill(int pid, int sig) {
    long r = __pagh_syscall6(SYS_KILL, pid, sig, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}
int sigemptyset(sigset_t *set) { if (!set) return -1; *set = 0; return 0; }
int sigaddset(sigset_t *set, int sig) { if (!set || sig < 1 || sig > 31) return -1; *set |= 1UL << (sig - 1); return 0; }
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    long r = __pagh_syscall6(SYS_RT_SIGPROCMASK, how, (long)set, (long)oldset, 8, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}
