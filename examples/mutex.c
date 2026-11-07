#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int counter;
static void *worker(void *arg) {
    int n = (int)(long)arg;
    for (int i = 0; i < n; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    return 0;
}
int main(void) {
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], 0, worker, (void *)250);
    for (int i = 0; i < 4; i++) pthread_join(t[i], 0);
    printf("counter=%d\n", counter);
    return counter == 1000 ? 0 : 1;
}
