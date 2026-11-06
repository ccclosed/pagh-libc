#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
int main(int argc, char **argv) {
    char buf[4096];
    if (argc < 2) {
        long n;
        while ((n = read(0, buf, sizeof buf)) > 0) write(1, buf, n);
        return 0;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) { fdprintf(2, "cat: %s: cannot open\n", argv[i]); rc = 1; continue; }
        long n;
        while ((n = read(fd, buf, sizeof buf)) > 0) write(1, buf, n);
        close(fd);
    }
    return rc;
}
