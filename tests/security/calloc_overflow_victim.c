/* pagh-libc calloc() integer-overflow victim (F-r3-rt-2, round-3 runtime
 * audit).  This is the audit's PoC shape, kept as the live regression: the
 * record count is untrusted input (the first 8 bytes of a file), exactly the
 * way an image/record parser reads a length before allocating an array.
 *
 * Pre-fix src/malloc.c computed `size_t total = n * size` with no overflow
 * check, so calloc(0x1000000000000001, 16) returned a 16-byte block while the
 * caller believed it owned count*16 bytes, and the fill loop wrote 16-byte
 * records until it ran off the ~68 KiB heap:
 *
 *     [user] ... killed: store page fault (cause=15, stval=0x20011000, ...)
 *
 * Fixed: the product overflow is refused with NULL/ENOMEM, this program
 * prints CALLOC REFUSED and exits 3 without writing through the pointer;
 * the count=4 control still gets 64 zeroed bytes and exits 0.
 *
 * tools/live-security.py compiles this with the cross toolchain against
 * build/libpagh-c.a and requires both outcomes in the serial transcript.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

struct rec { unsigned long a, b; }; /* 16 bytes */

int main(int argc, char **argv) {
    unsigned long count = 0;
    const char *path = argc > 1 ? argv[1] : "/mnt/rec.bin";
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) { printf("OPEN FAILED\n"); return 2; }
    if (read(fd, &count, sizeof count) != (long)sizeof count) {
        printf("SHORT READ\n"); close(fd); return 2;
    }
    close(fd);
    printf("COUNT %lu\n", count);
    struct rec *r = calloc(count, sizeof *r);
    if (!r) { printf("CALLOC REFUSED\n"); return 3; }
    printf("ALLOCATED, count*sizeof = %lu mod 2^64\n", count * sizeof *r);
    for (unsigned long i = 0; i < count; i++) { r[i].a = i; r[i].b = ~i; }
    printf("FILLED OK\n");
    return 0;
}