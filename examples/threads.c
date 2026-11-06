#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
static volatile int counter;
static void *worker(void *arg) {
    int n = (int)(long)arg;
    for (int i = 0; i < n; i++) counter++;
    printf("worker %d done\n", n);
    return (void *)(long)(n * 10);
}
int main(void) {
    pthread_t a, b;
    void *ra = 0, *rb = 0;
    if (pthread_create(&a, 0, worker, (void *)5) || pthread_create(&b, 0, worker, (void *)3)) {
        printf("create failed\n"); return 1;
    }
    pthread_join(a, &ra); pthread_join(b, &rb);
    printf("counter=%d ret=%ld/%ld\n", counter, (long)ra, (long)rb);
    return counter == 8 ? 0 : 1;
}
