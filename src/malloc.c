/* Bump allocator over brk.
 *
 * Every block starts with a 16-byte header that records the *requested* user
 * size; malloc() returns the aligned pointer just past it.  The header costs
 * 16 bytes per allocation and keeps the 16-byte alignment the native ABI
 * requires (RISC-V lp64d's long double), and it is what lets realloc() copy
 * exactly the old contents instead of guessing a length.  free() is still a
 * no-op: this allocator releases memory when the process exits, not earlier
 * (documented in README).
 *
 * Size arithmetic is checked, because the caller's length may be untrusted:
 * calloc() refuses a product that overflows size_t and malloc() refuses a
 * size whose 16-byte rounding or block-header addition would wrap.  An
 * undersized block must never reach a caller that believes it owns n * size
 * bytes (round-3 runtime audit F-r3-rt-2).
 */
#include <pagh.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define ALLOC_ALIGN 16
#define BLOCK_HEADER 16
#define HEAP_SLACK 65536
#define PAGE_SIZE 4096

struct block_header {
    size_t size;
    size_t reserved;
};
_Static_assert(sizeof(struct block_header) <= BLOCK_HEADER,
               "block header must fit in BLOCK_HEADER bytes");

static unsigned char *heap_base;
static size_t heap_left;

/* Make at least `need` bytes available at heap_base.  The old code resets the
 * bump region on growth; keep that shape but do every addition checked, so a
 * caller-controlled size can only ever fail with ENOMEM, never wrap into a
 * small "successful" chunk. */
static int grow(size_t need) {
    if (heap_left >= need) return 0;

    unsigned long cur = (unsigned long)__pagh_syscall3(SYS_BRK, 0, 0, 0);
    if ((long)cur < 0) { errno = ENOMEM; return -1; }

    size_t missing = need - heap_left;
    if (missing > (size_t)-1 - HEAP_SLACK) { errno = ENOMEM; return -1; }
    size_t chunk = missing + HEAP_SLACK;
    if (chunk > (size_t)-1 - (PAGE_SIZE - 1)) { errno = ENOMEM; return -1; }
    chunk = (chunk + (PAGE_SIZE - 1)) & ~(size_t)(PAGE_SIZE - 1);
    if (cur > (unsigned long)-1 - chunk) { errno = ENOMEM; return -1; }

    long next = __pagh_syscall3(SYS_BRK, (long)(cur + chunk), 0, 0);
    if (next < 0) { errno = ENOMEM; return -1; }
    /* A successful brk must grant at least what was asked; never trust a
     * shorter grant, because heap_left would then be below the block we are
     * about to hand out. */
    unsigned long granted = (unsigned long)next;
    if (granted < cur || granted - cur < need) { errno = ENOMEM; return -1; }

    heap_base = (unsigned char *)cur;
    heap_left = (size_t)(granted - cur);
    return 0;
}

void *malloc(size_t size) {
    if (size > (size_t)-1 - (ALLOC_ALIGN - 1)) { errno = ENOMEM; return 0; }
    size_t rounded = (size + (ALLOC_ALIGN - 1)) & ~(size_t)(ALLOC_ALIGN - 1);
    if (rounded > (size_t)-1 - BLOCK_HEADER) { errno = ENOMEM; return 0; }
    if (grow(rounded + BLOCK_HEADER) < 0) return 0;

    struct block_header *header = (struct block_header *)heap_base;
    header->size = size;
    header->reserved = 0;
    heap_base += BLOCK_HEADER;
    heap_left -= BLOCK_HEADER;

    void *p = heap_base;
    heap_base += rounded;
    heap_left -= rounded;
    return p;
}

void free(void *ptr) { (void)ptr; }

void *calloc(size_t n, size_t size) {
    if (n && size > (size_t)-1 / n) { errno = ENOMEM; return 0; }
    size_t total = n * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    struct block_header *header = (struct block_header *)ptr - 1;
    /* Shrinking keeps the block: free() is a no-op, so the trailing bytes are
     * already reserved for this allocation.  Growing copies min(old, new). */
    if (size <= header->size) return ptr;
    void *p = malloc(size);
    if (!p) return 0;
    memmove(p, ptr, header->size);
    return p;
}
