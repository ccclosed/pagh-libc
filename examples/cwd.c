#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
int main(void) {
    char buf[256];
    if (chdir("/mnt") < 0) { printf("chdir failed\n"); return 1; }
    if (!getcwd(buf, sizeof buf)) return 1;
    printf("cwd=%s\n", buf);
    int fd = open("hello.txt", O_RDONLY);
    if (fd < 0) { printf("relative open failed\n"); return 1; }
    char data[32]; long n = read(fd, data, sizeof data - 1); close(fd);
    if (n < 0) n = 0; data[n] = 0;
    printf("relative read=%s", data);
    return 0;
}
