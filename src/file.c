#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

struct PaghFile {
    int fd;
    int readable;
    int writable;
    int eof;
    int error;
    unsigned char buf[4096];
    size_t pos;
    size_t len;
};

static struct PaghFile _stdin = { .fd = 0, .readable = 1, .buf = {0} };
static struct PaghFile _stdout = { .fd = 1, .writable = 1, .buf = {0} };
static struct PaghFile _stderr = { .fd = 2, .writable = 1, .buf = {0} };
FILE *stdin = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

FILE *fopen(const char *path, const char *mode) {
    int flags = 0;
    int readable = 0, writable = 0;
    if (mode[0] == 'r') { flags = O_RDONLY; readable = 1; }
    else if (mode[0] == 'w') { flags = O_WRONLY | O_CREAT | O_TRUNC; writable = 1; }
    else if (mode[0] == 'a') { flags = O_WRONLY | O_CREAT | O_APPEND; writable = 1; }
    else { errno = EINVAL; return 0; }
    if (strchr(mode, '+')) { flags = (flags & ~3) | O_RDWR; readable = writable = 1; }
    int fd = open(path, flags, 0666);
    if (fd < 0) return 0;
    FILE *f = malloc(sizeof *f);
    if (!f) { close(fd); return 0; }
    memset(f, 0, sizeof *f);
    f->fd = fd; f->readable = readable; f->writable = writable;
    return f;
}

int fflush(FILE *f) {
    if (!f) {
        fflush(stdout); fflush(stderr);
        return 0;
    }
    if (f->writable && f->pos) {
        size_t off = 0;
        while (off < f->pos) {
            long n = write(f->fd, f->buf + off, f->pos - off);
            if (n <= 0) { f->error = 1; errno = EIO; return EOF; }
            off += (size_t)n;
        }
        f->pos = 0;
    }
    return 0;
}

int fclose(FILE *f) {
    if (!f) return EOF;
    int rc = fflush(f);
    if (f->fd > 2) close(f->fd);
    if (f != stdin && f != stdout && f != stderr) free(f);
    return rc;
}

size_t fread(void *buf, size_t size, size_t n, FILE *f) {
    if (!f || !f->readable || !size || !n) return 0;
    size_t want = size * n;
    unsigned char *out = buf;
    size_t got = 0;
    while (got < want) {
        if (f->pos >= f->len) {
            long r = read(f->fd, f->buf, sizeof f->buf);
            if (r < 0) { f->error = 1; errno = EIO; break; }
            if (r == 0) { f->eof = 1; break; }
            f->pos = 0; f->len = (size_t)r;
        }
        size_t take = f->len - f->pos;
        if (take > want - got) take = want - got;
        memcpy(out + got, f->buf + f->pos, take);
        got += take; f->pos += take;
    }
    return got / size;
}

size_t fwrite(const void *buf, size_t size, size_t n, FILE *f) {
    if (!f || !f->writable || !size || !n) return 0;
    size_t want = size * n;
    const unsigned char *in = buf;
    size_t done = 0;
    while (done < want) {
        size_t space = sizeof f->buf - f->pos;
        if (!space) { if (fflush(f) == EOF) break; space = sizeof f->buf; }
        size_t take = want - done < space ? want - done : space;
        memcpy(f->buf + f->pos, in + done, take);
        f->pos += take; done += take;
        if (f == stdout) { for (size_t i = 0; i < f->pos; i++) if (f->buf[i] == '\n') { fflush(f); break; } }
    }
    return done / size;
}

char *fgets(char *buf, int size, FILE *f) {
    if (!f || !buf || size <= 0) return 0;
    int i = 0;
    while (i < size - 1) {
        unsigned char c;
        if (fread(&c, 1, 1, f) != 1) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (!i) return 0;
    buf[i] = 0;
    return buf;
}

int fputc(int c, FILE *f) {
    unsigned char ch = (unsigned char)c;
    return fwrite(&ch, 1, 1, f) == 1 ? ch : EOF;
}
int fgetc(FILE *f) {
    unsigned char ch;
    return fread(&ch, 1, 1, f) == 1 ? ch : EOF;
}
int feof(FILE *f) { return f ? f->eof : 0; }
int ferror(FILE *f) { return f ? f->error : 0; }
void clearerr(FILE *f) { if (f) { f->eof = 0; f->error = 0; } }
int fputs(const char *s, FILE *f) {
    size_t n = strlen(s);
    return fwrite(s, 1, n, f) == n ? 0 : EOF;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0) fwrite(buf, 1, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1, f);
    return n;
}
int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap); va_end(ap); return n;
}
int fdprintf(int fd, const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (n > 0) write(fd, buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
    return n;
}
