#ifndef PAGH_SIGNAL_H
#define PAGH_SIGNAL_H
#include <pagh.h>
#define SIG_ERR ((void (*)(int))-1)
typedef unsigned long sigset_t;
/* The alternate signal stack (kernel layout: sp, 32-bit flags, padding,
 * size — 24 bytes). ss_size must be at least 4 KiB (MINSIGSTKSZ). */
typedef struct {
    void *ss_sp;
    int ss_flags;
    unsigned int pad;
    size_t ss_size;
} stack_t;
#define SS_ONSTACK 1
#define SS_DISABLE 2
struct sigaction {
    void (*sa_handler)(int);
    unsigned long sa_flags;
    sigset_t sa_mask;
    void (*sa_restorer)(void);
};
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
void (*signal(int signum, void (*handler)(int)))(int);
int raise(int sig);
int kill(int pid, int sig);
int sigemptyset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigaltstack(const stack_t *ss, stack_t *old_ss);
#endif
