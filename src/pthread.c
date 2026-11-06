#include <pagh.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define MAX_THREADS 16
struct thread_start {
    void *(*fn)(void *);
    void *arg;
    void *retval;
    unsigned char *stack;
    int done;
};
static struct thread_start *registry[MAX_THREADS];
static pthread_t registry_tid[MAX_THREADS];

static long futex(int *addr, long op, long val) {
    return __pagh_syscall6(SYS_FUTEX, (long)addr, op | FUTEX_PRIVATE_FLAG, val, 0, 0, 0);
}

static void __attribute__((noreturn)) thread_trampoline(unsigned long raw) {
    struct thread_start *s = (struct thread_start *)raw;
    s->retval = s->fn(s->arg);
    s->done = 1;
    futex(&s->done, FUTEX_WAKE, 1);
    __pagh_syscall3(SYS_EXIT, 0, 0, 0);
    for (;;) {}
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    size_t stack_size = attr && attr->stacksize ? attr->stacksize : (64 * 1024);
    stack_size = (stack_size + 4095) & ~(size_t)4095;
    unsigned char *stack = malloc(stack_size);
    struct thread_start *s = malloc(sizeof *s);
    if (!stack || !s) { free(stack); free(s); errno = ENOMEM; return -1; }
    s->fn = start_routine; s->arg = arg; s->retval = 0; s->stack = stack; s->done = 0;
    unsigned long top = ((unsigned long)stack + stack_size) & ~15UL;
    long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD;
    long tid = __pagh_syscall6(SYS_CLONE, flags, (long)top,
                               (long)thread_trampoline, (long)s, 0, 0);
    if (tid < 0) { free(stack); free(s); errno = (int)-tid; return -1; }
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!registry[i]) { registry[i] = s; registry_tid[i] = (pthread_t)tid; break; }
    }
    *thread = (pthread_t)tid;
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (registry_tid[i] == thread && registry[i]) {
            struct thread_start *s = registry[i];
            while (!s->done) futex(&s->done, FUTEX_WAIT, 0);
            if (retval) *retval = s->retval;
            free(s->stack);
            free(s);
            registry[i] = 0;
            registry_tid[i] = 0;
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}

void pthread_exit(void *retval) {
    (void)retval;
    __pagh_syscall3(SYS_EXIT, 0, 0, 0);
    for (;;) {}
}
pthread_t pthread_self(void) { return (pthread_t)__pagh_syscall3(SYS_GETTID, 0, 0, 0); }
int pthread_attr_init(pthread_attr_t *attr) {
    if (!attr) return -1;
    attr->detachstate = 0; attr->stacksize = 64 * 1024; return 0;
}
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size) {
    if (!attr) return -1; attr->stacksize = size; return 0;
}
int pthread_attr_destroy(pthread_attr_t *attr) { (void)attr; return 0; }
