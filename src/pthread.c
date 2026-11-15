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

#define PAGE_SIZE 4096
#define PTHREAD_DEFAULT_STACK (64 * 1024)

/* A thread stack is carved out of the process heap, and clone() only checks
 * that the page at sp-16 is user RW — it cannot tell that the address is
 * inside live heap blocks.  So the size arithmetic must never wrap:
 * (SIZE_MAX + 4095) & ~4095 is 0, malloc(0) hands back a 16-byte header block,
 * and the new thread then pushes its frame through the allocations below it
 * (round-4 runtime audit F-r4-rt-1).  Reject anything outside the documented
 * window and do the rounding with an explicit overflow check. */
static int checked_stack_size(size_t requested, size_t *out) {
    if (requested < PTHREAD_STACK_MIN || requested > PTHREAD_STACK_MAX)
        return EINVAL;
    if (requested > (size_t)-1 - (PAGE_SIZE - 1))
        return EINVAL;
    size_t rounded = (requested + (PAGE_SIZE - 1)) & ~(size_t)(PAGE_SIZE - 1);
    if (rounded == 0 || rounded > PTHREAD_STACK_MAX)
        return EINVAL;
    *out = rounded;
    return 0;
}

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
    if (!thread || !start_routine) { errno = EINVAL; return -1; }
    size_t stack_size = PTHREAD_DEFAULT_STACK;
    if (attr) {
        int error = checked_stack_size(attr->stacksize, &stack_size);
        if (error) { errno = error; return -1; }
    }
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
    if (!attr) { errno = EINVAL; return -1; }
    attr->detachstate = 0; attr->stacksize = PTHREAD_DEFAULT_STACK; return 0;
}
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size) {
    if (!attr) { errno = EINVAL; return -1; }
    /* Record the requested size even when it is out of range and still report
     * EINVAL: pthread_create revalidates the attribute, so a caller that
     * ignores this error cannot make the library quietly substitute the
     * default 64 KiB stack behind its back (the audit's shape: the size comes
     * from untrusted input and the return value is dropped). */
    attr->stacksize = size;
    size_t rounded;
    int error = checked_stack_size(size, &rounded);
    if (error) { errno = error; return -1; }
    return 0;
}
int pthread_attr_destroy(pthread_attr_t *attr) { (void)attr; return 0; }

/* ---- mutexes (futex-backed) ------------------------------------------- */
int pthread_mutex_init(pthread_mutex_t *m, const void *attr) {
    (void)attr; if (!m) return -1; m->locked = 0; return 0;
}
int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_trylock(pthread_mutex_t *m) {
    int expected = 0;
    return __atomic_compare_exchange_n(&m->locked, &expected, 1, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 0 : 1;
}
int pthread_mutex_lock(pthread_mutex_t *m) {
    for (;;) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&m->locked, &expected, 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 0;
        futex(&m->locked, FUTEX_WAIT, 1);
    }
}
int pthread_mutex_unlock(pthread_mutex_t *m) {
    __atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE);
    futex(&m->locked, FUTEX_WAKE, 1);
    return 0;
}

/* ---- condition variables (futex sequence word) ------------------------ */
int pthread_cond_init(pthread_cond_t *c, const void *attr) { (void)attr; if (!c) return -1; c->seq = 0; return 0; }
int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    int seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
    pthread_mutex_unlock(m);
    futex(&c->seq, FUTEX_WAIT, seq);
    pthread_mutex_lock(m);
    return 0;
}
int pthread_cond_signal(pthread_cond_t *c) {
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
    futex(&c->seq, FUTEX_WAKE, 1);
    return 0;
}
int pthread_cond_broadcast(pthread_cond_t *c) {
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
    futex(&c->seq, FUTEX_WAKE, 0x7fffffff);
    return 0;
}

/* ---- rwlock (readers high, writer low bit) ---------------------------- */
#define RW_WRITER 1
int pthread_rwlock_init(pthread_rwlock_t *l, const void *attr) { (void)attr; l->state = 0; return 0; }
int pthread_rwlock_rdlock(pthread_rwlock_t *l) {
    for (;;) {
        int s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
        if (!(s & RW_WRITER) &&
            __atomic_compare_exchange_n(&l->state, &s, s + 2, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 0;
        futex(&l->state, FUTEX_WAIT, s);
    }
}
int pthread_rwlock_wrlock(pthread_rwlock_t *l) {
    for (;;) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&l->state, &expected, RW_WRITER, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 0;
        futex(&l->state, FUTEX_WAIT, expected);
    }
}
int pthread_rwlock_unlock(pthread_rwlock_t *l) {
    int s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
    if (s & RW_WRITER) __atomic_store_n(&l->state, 0, __ATOMIC_RELEASE);
    else __atomic_sub_fetch(&l->state, 2, __ATOMIC_RELEASE);
    futex(&l->state, FUTEX_WAKE, 0x7fffffff);
    return 0;
}
