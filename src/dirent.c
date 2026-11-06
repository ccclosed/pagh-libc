#include <pagh.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

struct PaghDir {
    int fd;
    int done;
    size_t pos;
    size_t len;
    unsigned char buf[4096];
};

DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 0;
    DIR *d = malloc(sizeof *d);
    if (!d) { close(fd); return 0; }
    d->fd = fd; d->done = 0; d->pos = 0; d->len = 0;
    return d;
}

static unsigned align8(unsigned n) { return (n + 7) & ~7u; }

struct pagh_dirent *readdir(DIR *d) {
    for (;;) {
        if (d->done) return 0;
        if (d->pos >= d->len) {
            long n = __pagh_syscall3(SYS_GETDENTS64, d->fd, (long)d->buf, sizeof d->buf);
            if (n < 0) { errno = (int)-n; d->done = 1; return 0; }
            if (n == 0) { d->done = 1; return 0; }
            d->pos = 0; d->len = (size_t)n;
        }
        if (d->len - d->pos < 12) { d->done = 1; return 0; }
        unsigned char *p = d->buf + d->pos;
        unsigned name_len = p[9];
        if (!name_len) { d->done = 1; return 0; }
        size_t rec = align8(12 + name_len);
        if (d->pos + rec > d->len) { d->done = 1; return 0; }
        /* The kernel record is 8-byte ino, kind byte at 8, name_len at 9,
           padding at 10..11, name at 12.  Present the same shape. */
        struct pagh_dirent *out = (struct pagh_dirent *)p;
        d->pos += rec;
        return out;
    }
}

int closedir(DIR *d) {
    if (!d) return -1;
    close(d->fd);
    free(d);
    return 0;
}
