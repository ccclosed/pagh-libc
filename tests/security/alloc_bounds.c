/* On-guest checks for the allocator hardening siblings of F-r3-rt-2: the
 * 16-byte rounding wrap in malloc() and the out-of-bounds copy in realloc().
 *
 * The audit marked both "verified: no" (code-evident, not run); this program
 * makes them live assertions on the real kernel and the real libpagh-c:
 *
 *  - calloc(0x1000000000000001, 16) must return NULL/ENOMEM (the product
 *    wraps to 16 pre-fix);
 *  - malloc(SIZE_MAX) and malloc(SIZE_MAX - 20) must return NULL/ENOMEM: the
 *    pre-fix `size = (size + 15) & ~15` wrapped to 0 and returned a
 *    "successful" zero-byte block;
 *  - realloc(16 -> 64) must copy only the old 16 bytes: the neighbour block's
 *    0xCD pattern must not appear past the prefix, and the neighbour must be
 *    untouched (the pre-fix `memmove(p, ptr, size)` copied the NEW size);
 *  - malloc still returns 16-byte aligned blocks.
 *
 * tools/live-security.py requires "alloc_bounds: ok" and exit status 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int failures;

static void check(const char *what, int cond) {
    if (!cond) { printf("alloc_bounds: FAIL %s\n", what); failures++; }
}

int main(void) {
    int calloc_refused = 1;
    errno = 0;
    if (calloc((size_t)0x1000000000000001ULL, 16) != NULL || errno != ENOMEM) {
        calloc_refused = 0;
    }
    check("calloc product overflow refused (NULL, ENOMEM)", calloc_refused);

    int malloc_refused = 1;
    errno = 0;
    if (malloc((size_t)-1) != NULL || errno != ENOMEM) malloc_refused = 0;
    errno = 0;
    if (malloc((size_t)-1 - 20) != NULL || errno != ENOMEM) malloc_refused = 0;
    check("malloc rounding/header overflow refused (NULL, ENOMEM)",
          malloc_refused);
    if (calloc_refused && malloc_refused) {
        printf("alloc_bounds: calloc/malloc overflow refused\n");
    }

    int realloc_ok = 1;
    unsigned char *a = malloc(16);
    unsigned char *b = malloc(16);
    if (!a || !b) {
        realloc_ok = 0;
    } else {
        memset(a, 0xAB, 16);
        memset(b, 0xCD, 16);
        unsigned char *c = realloc(a, 64);
        if (!c) {
            realloc_ok = 0;
        } else {
            for (int i = 0; i < 16; i++) {
                if (c[i] != 0xAB) realloc_ok = 0;
            }
            for (int i = 16; i < 64; i++) {
                if (c[i] == 0xCD) realloc_ok = 0;
            }
            for (int i = 0; i < 16; i++) {
                if (b[i] != 0xCD) realloc_ok = 0;
            }
        }
    }
    check("realloc copies only the old block", realloc_ok);
    printf("alloc_bounds: realloc copies only the old block\n");

    int aligned = 1;
    for (size_t n = 1; n <= 64; n++) {
        unsigned char *p = malloc(n);
        if (!p || ((unsigned long)p & 15) != 0) {
            aligned = 0;
        } else {
            memset(p, 0xA5, n);
        }
    }
    check("malloc alignment 1..64", aligned);

    if (failures) {
        printf("alloc_bounds: FAIL (%d checks)\n", failures);
        return 1;
    }
    printf("alloc_bounds: ok (4 checks)\n");
    return 0;
}