/* Process-exit contract (0.12.0 fix): with a worker thread still alive and an
 * atexit handler registered, `exit()` must run the handlers (LIFO, POSIX) and
 * end the *whole process* (SYS_EXIT_GROUP 94), not just the calling thread
 * (SYS_EXIT 93), or the shell's `run` would wait for the worker forever.
 *
 * Regression shape: the live harness (tools/live-sockets.py) runs this through
 * `run /mnt/exit_semantics` and requires "exited with code 0" plus the atexit
 * marker. A return to SYS_EXIT leaves the run alive until the harness timeout;
 * dropping the handlers loses the marker. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void handler(void) { printf("exit_semantics: atexit handler ran\n"); }

static void *worker(void *arg) {
    (void)arg;
    for (;;) usleep(100000);
    return 0;
}

int main(void) {
    pthread_t thread;
    if (pthread_create(&thread, 0, worker, 0) != 0) {
        printf("exit_semantics: pthread_create failed\n");
        return 1;
    }
    if (atexit(handler) != 0) {
        printf("exit_semantics: atexit failed\n");
        return 1;
    }
    printf("exit_semantics: worker alive, calling exit(0)\n");
    exit(0);
}