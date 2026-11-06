#include <signal.h>
#include <stdio.h>
static volatile int hits;
static void handler(int sig) { (void)sig; hits++; }
int main(void) {
    if (signal(SIGUSR1, handler) == SIG_ERR) { printf("signal failed\n"); return 1; }
    if (raise(SIGUSR1) < 0) { printf("raise failed\n"); return 1; }
    printf("hits=%d\n", hits);
    return hits == 1 ? 0 : 1;
}
