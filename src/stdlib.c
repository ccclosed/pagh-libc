#include <pagh.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void __run_atexit(void);

int atoi(const char *s) { return (int)atol(s); }
long atol(const char *s) {
    long v = 0; int neg = 0;
    while (*s == ' ') s++;
    if (*s == '-' || *s == '+') neg = (*s++ == '-');
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}
int abs(int x) { return x < 0 ? -x : x; }
void abort(void) { __run_atexit(); _exit(134); }
static void (*atexit_fns[16])(void);
static int atexit_n;
int atexit(void (*fn)(void)) { if (atexit_n < 16) atexit_fns[atexit_n++] = fn; return 0; }
void __run_atexit(void) { while (atexit_n) atexit_fns[--atexit_n](); }
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    unsigned char *b = base;
    for (size_t i = 1; i < n; i++) {
        for (size_t j = i; j > 0; j--) {
            unsigned char *a = b + (j - 1) * size, *c = b + j * size;
            if (cmp(a, c) <= 0) break;
            unsigned char tmp;
            for (size_t k = 0; k < size; k++) { tmp = a[k]; a[k] = c[k]; c[k] = tmp; }
        }
    }
}
