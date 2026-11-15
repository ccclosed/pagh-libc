/* Host-side allocator hardening test (F-r3-rt-2, round-3 runtime audit).
 *
 * src/malloc.c is compiled for the host with the malloc family renamed
 * (pagh_malloc/...) and a fake brk from tests/host_brk_shim.c; this file
 * drives the contracts the audit's calloc overflow broke:
 *
 *  - calloc(n, size) refuses a product that overflows size_t with NULL and
 *    ENOMEM, before allocating or zeroing anything (the audit's
 *    n = 0x1000000000000001, size = 16 wrapped total to 16);
 *  - malloc(size) refuses sizes whose 16-byte rounding or block header would
 *    wrap (the pre-fix code returned a non-NULL pointer to a zero-byte block
 *    for malloc(SIZE_MAX));
 *  - realloc(ptr, larger) copies only the old block's bytes, not the new size
 *    from a pointer whose old size it does not know (the code-evident OOB
 *    read): the bytes after the copied prefix must still be fresh (zero), and
 *    the neighbouring allocation must be untouched;
 *  - the audit's count=4 control and the alignment contract still work.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void *pagh_malloc(size_t size);
void *pagh_calloc(size_t n, size_t size);
void *pagh_realloc(void *ptr, size_t size);
void pagh_free(void *ptr);
int *host_errno_slot(void);

#define HOST_ENOMEM 12

static int checks;
static int failures;

static void ok(const char *what, int cond) {
    checks++;
    if (!cond) {
        printf("malloc_test: FAIL %s\n", what);
        failures++;
    }
}

static void expect_refused(const char *what, void *p) {
    ok(what, p == 0);
    ok("    errno == ENOMEM", p == 0 && *host_errno_slot() == HOST_ENOMEM);
}

static int all_zero(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (p[i]) return 0;
    }
    return 1;
}

int main(void) {
    printf("malloc_test: %u-bit size_t, %u-bit pointer\n",
           (unsigned)(sizeof(size_t) * 8), (unsigned)(sizeof(void *) * 8));

    /* F-r3-rt-2: the audit's exact product.  0x1000000000000001 records of
     * 16 bytes need 2^64 + 16 bytes, which truncates to 16. */
    if (sizeof(size_t) >= 8) {
        *host_errno_slot() = 0;
        void *p = pagh_calloc((size_t)0x1000000000000001ULL, 16);
        expect_refused("calloc(0x1000000000000001, 16) is refused", p);
    }

    size_t max = (size_t)-1;
    *host_errno_slot() = 0;
    expect_refused("calloc(SIZE_MAX, 2) is refused", pagh_calloc(max, 2));
    *host_errno_slot() = 0;
    expect_refused("calloc(SIZE_MAX/2, 3) is refused", pagh_calloc(max / 2, 3));
    *host_errno_slot() = 0;
    expect_refused("calloc(SIZE_MAX/16 + 1, 16) is refused",
                   pagh_calloc(max / 16 + 1, 16));

    /* A product that fits but cannot be backed by the heap fails at malloc,
     * not by wrapping: SIZE_MAX/16 * 16 == SIZE_MAX-15. */
    *host_errno_slot() = 0;
    expect_refused("calloc(SIZE_MAX/16, 16) is refused by the heap",
                   pagh_calloc(max / 16, 16));

    /* Sibling C2: rounding SIZE_MAX used to wrap to zero and hand back a
     * "successful" zero-byte block. */
    *host_errno_slot() = 0;
    expect_refused("malloc(SIZE_MAX) is refused by rounding",
                   pagh_malloc(max));
    *host_errno_slot() = 0;
    expect_refused("malloc(SIZE_MAX - 10) is refused by rounding",
                   pagh_malloc(max - 10));
    *host_errno_slot() = 0;
    expect_refused("malloc(SIZE_MAX - 20) is refused by the block header",
                   pagh_malloc(max - 20));

    /* The audit's control: count = 4 gets 64 zeroed bytes and can fill them. */
    *host_errno_slot() = 0;
    unsigned char *r = pagh_calloc(4, 16);
    ok("calloc(4, 16) succeeds", r != 0);
    if (r) {
        ok("calloc(4, 16) is 16-byte aligned", ((uintptr_t)r % 16) == 0);
        ok("calloc(4, 16) zeroes all 64 bytes", all_zero(r, 64));
        for (size_t i = 0; i < 64; i++) r[i] = (unsigned char)(i + 1);
        ok("calloc(4, 16) block holds 64 written bytes", r[63] == 64);
    }

    /* Alignment contract over the common sizes (header must not break it). */
    int aligned = 1;
    for (size_t n = 1; n <= 64; n++) {
        unsigned char *p = pagh_malloc(n);
        if (!p || ((uintptr_t)p % 16) != 0) {
            aligned = 0;
        } else {
            memset(p, 0xA5, n);
        }
    }
    ok("malloc returns 16-byte aligned blocks for sizes 1..64", aligned);
    void *z = pagh_malloc(0);
    ok("malloc(0) returns a pointer that is aligned when non-NULL",
       z == 0 || ((uintptr_t)z % 16) == 0);

    /* Sibling C1: realloc must copy the old size only.  `a` is 16 bytes, `b`
     * is the next 16-byte block and holds a 0xCD pattern.  Growing `a` to 64
     * bytes must copy exactly `a`'s 16 bytes, leaving the rest fresh/zero,
     * and must not disturb `b`. */
    unsigned char *a = pagh_malloc(16);
    unsigned char *b = pagh_malloc(16);
    ok("two malloc(16) blocks", a != 0 && b != 0);
    if (a && b) {
        memset(a, 0xAB, 16);
        memset(b, 0xCD, 16);
        unsigned char *c = pagh_realloc(a, 64);
        ok("realloc(16 -> 64) succeeds", c != 0);
        if (c) {
            ok("realloc preserves the old 16 bytes", memcmp(c, a, 16) == 0);
            ok("realloc leaves the grown tail fresh (no OOB copy)",
               all_zero(c + 16, 48));
            ok("realloc does not disturb the next block",
               b[0] == 0xCD && b[15] == 0xCD);
            unsigned char *d = pagh_realloc(c, 8);
            ok("realloc(64 -> 8) succeeds", d != 0);
            ok("realloc shrink preserves the prefix",
               d != 0 && memcmp(d, a, 8) == 0);
        }
    }
    unsigned char *n = pagh_realloc(0, 32);
    ok("realloc(NULL, 32) behaves like malloc", n != 0);
    if (n) {
        memset(n, 0x5A, 32);
        ok("realloc(NULL, 32) block holds 32 bytes", n[31] == 0x5A);
    }

    pagh_free(r);
    pagh_free(n);
    pagh_free(b);

    if (failures) {
        printf("malloc_test: FAIL (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("malloc_test: ok (%d checks)\n", checks);
    return 0;
}