#ifndef PAGH_PTHREAD_H
#define PAGH_PTHREAD_H
#include <stddef.h>
typedef int pthread_t;
typedef struct { int detachstate; size_t stacksize; } pthread_attr_t;
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
void pthread_exit(void *retval) __attribute__((noreturn));
pthread_t pthread_self(void);
int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size);
int pthread_attr_destroy(pthread_attr_t *attr);
#endif
