/* getdelim(3) and getline(3).
 *
 * The stream interface they need is exactly the public fgetc/ferror pair, so
 * this file does not depend on the FILE layout and the host test in
 * tests/getline_test.c can drive it from a synthetic stream. The buffer
 * starts at the same 120 bytes glibc uses (large enough for any sane line
 * without a second allocation) and doubles when a longer one appears.
 *
 * `free` is a no-op in this libc (bump allocator), so a line that outgrows its
 * buffer leaves the old one allocated until exit; that is the documented
 * allocator compromise, not a per-line leak in the usual case. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_INITIAL 120

ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream) {
    if (!lineptr || !n || !stream) {
        errno = EINVAL;
        return -1;
    }
    if (!*lineptr || *n == 0) {
        char *buf = malloc(LINE_INITIAL);
        if (!buf) {
            errno = ENOMEM;
            return -1;
        }
        *lineptr = buf;
        *n = LINE_INITIAL;
    }

    size_t len = 0;
    for (;;) {
        int c = fgetc(stream);
        if (c == EOF) {
            if (ferror(stream)) return -1;
            if (len == 0) return -1;
            break;
        }
        if (len + 1 >= *n) {
            size_t cap = *n <= (size_t)-1 / 2 ? *n * 2 : (size_t)-1;
            if (cap <= len + 1) {
                errno = ENOMEM;
                return -1;
            }
            char *buf = malloc(cap);
            if (!buf) {
                errno = ENOMEM;
                return -1;
            }
            memcpy(buf, *lineptr, len);
            free(*lineptr);
            *lineptr = buf;
            *n = cap;
        }
        (*lineptr)[len++] = (char)c;
        if (c == delim) break;
    }
    (*lineptr)[len] = 0;
    return (ssize_t)len;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    return getdelim(lineptr, n, '\n', stream);
}