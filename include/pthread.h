#ifndef PAGH_PTHREAD_H
#define PAGH_PTHREAD_H
#include <stddef.h>
typedef int pthread_t;
typedef struct { int locked; } pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER { 0 }
int pthread_mutex_init(pthread_mutex_t *m, const void *attr);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
typedef struct { int seq; } pthread_cond_t;
#define PTHREAD_COND_INITIALIZER { 0 }
int pthread_cond_init(pthread_cond_t *c, const void *attr);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
typedef struct { int state; } pthread_rwlock_t;
#define PTHREAD_RWLOCK_INITIALIZER { 0 }
int pthread_rwlock_init(pthread_rwlock_t *l, const void *attr);
int pthread_rwlock_rdlock(pthread_rwlock_t *l);
int pthread_rwlock_wrlock(pthread_rwlock_t *l);
int pthread_rwlock_unlock(pthread_rwlock_t *l);
typedef struct { int detachstate; size_t stacksize; } pthread_attr_t;

/* Thread stack bounds.  The stack comes out of the process heap, whose window
 * in the pagh ABI is 1 MiB, so the library refuses a requested size outside
 * [PTHREAD_STACK_MIN, PTHREAD_STACK_MAX] instead of letting the page-rounding
 * arithmetic wrap (round-4 runtime audit F-r4-rt-1).  PTHREAD_STACK_MIN is the
 * POSIX/Linux floor; a NULL attr means the 64 KiB default, and the real limit
 * is what the heap has left: a valid-but-large size fails with ENOMEM. */
#define PTHREAD_STACK_MIN 16384
#define PTHREAD_STACK_MAX (1024 * 1024)

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
void pthread_exit(void *retval) __attribute__((noreturn));
pthread_t pthread_self(void);
int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size);
int pthread_attr_destroy(pthread_attr_t *attr);
#endif
