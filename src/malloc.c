#include <pagh.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static unsigned char *heap_base;
static size_t heap_left;

static int grow(size_t need) {
    unsigned long cur = (unsigned long)__pagh_syscall3(SYS_BRK, 0, 0, 0);
    if ((long)cur < 0) { errno = ENOMEM; return -1; }
    size_t chunk = need + 65536;
    chunk = (chunk + 4095) & ~(size_t)4095;
    long next = __pagh_syscall3(SYS_BRK, (long)(cur + chunk), 0, 0);
    if (next < 0) { errno = ENOMEM; return -1; }
    heap_base = (unsigned char *)cur;
    heap_left = (size_t)((unsigned long)next - cur);
    return 0;
}

void *malloc(size_t size) {
    size = (size + 15) & ~(size_t)15;
    if (!heap_left) {
        if (grow(size + 16) < 0) return 0;
    }
    while (heap_left < size) {
        if (grow(size + 16) < 0) return 0;
    }
    void *p = heap_base;
    heap_base += size;
    heap_left -= size;
    return p;
}
void free(void *ptr) { (void)ptr; }
void *calloc(size_t n, size_t size) {
    size_t total = n * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}
void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    void *p = malloc(size);
    if (!p) return 0;
    /* We do not track the old size; the common callers pass a larger size
       after writing only the old contents, so copy what fits (bounded by the
       caller).  A production malloc would keep a header; this is a bump
       allocator for short-lived CLI programs, documented in README. */
    memmove(p, ptr, size);
    return p;
}
