#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
int main(void) {
    printf("parent pid=%d ppid=%d\n", getpid(), getppid());
    pid_t child = fork();
    if (child < 0) { printf("fork failed\n"); return 1; }
    if (child == 0) {
        char *args[] = { "/mnt/hello", 0 };
        execv("/mnt/hello", args);
        printf("exec failed\n");
        _exit(127);
    }
    int status = 0;
    pid_t got = waitpid(child, &status, 0);
    printf("child=%d got=%d status=%d code=%d\n", child, got, status, status >> 8);
    return got == child && ((status >> 8) & 0xff) == 0 ? 0 : 1;
}
