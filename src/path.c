#include <pagh_internal.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

char __pagh_cwd[256] = "/";

static unsigned long slen(const char *s, unsigned long max) {
    unsigned long n = 0;
    while (n < max && s[n]) n++;
    return n;
}

const char *__pagh_abs(const char *path, char *buf, unsigned long size) {
    if (!path) { buf[0] = 0; return buf; }
    if (path[0] == '/') {
        unsigned long n = slen(path, size - 1);
        memcpy(buf, path, n); buf[n] = 0;
        return buf;
    }
    unsigned long c = slen(__pagh_cwd, sizeof __pagh_cwd - 1);
    unsigned long p = slen(path, size - 1);
    if (c + 1 + p + 1 > size) p = size - c - 2;
    memcpy(buf, __pagh_cwd, c);
    buf[c] = '/';
    memcpy(buf + c + 1, path, p);
    buf[c + 1 + p] = 0;
    return buf;
}

int chdir(const char *path) {
    char buf[512];
    const char *abs = __pagh_abs(path, buf, sizeof buf);
    struct pagh_stat st;
    if (stat(abs, &st) < 0) return -1;
    if (st.kind != 1) { errno = ENOTDIR; return -1; }
    unsigned long n = slen(abs, sizeof __pagh_cwd - 1);
    if (n > 255) { errno = ENAMETOOLONG; return -1; }
    memcpy(__pagh_cwd, abs, n); __pagh_cwd[n] = 0;
    return 0;
}

char *getcwd(char *buf, unsigned long size) {
    unsigned long n = slen(__pagh_cwd, sizeof __pagh_cwd - 1);
    if (size < n + 1) { errno = ERANGE; return 0; }
    memcpy(buf, __pagh_cwd, n); buf[n] = 0;
    return buf;
}
