#include <pthread.h>
#include <stdio.h>
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
static int ready, seen;
static void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&m);
    ready = 1;
    pthread_cond_signal(&c);
    pthread_mutex_unlock(&m);
    return 0;
}
int main(void) {
    pthread_t t;
    pthread_create(&t, 0, worker, 0);
    pthread_mutex_lock(&m);
    while (!ready) pthread_cond_wait(&c, &m);
    seen = 1;
    pthread_mutex_unlock(&m);
    pthread_join(t, 0);
    printf("ready=%d seen=%d\n", ready, seen);
    return ready && seen ? 0 : 1;
}
