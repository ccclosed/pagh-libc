/* On-guest checks for the pthread stack-size validation (F-r4-rt-1).
 *
 * The round-4 runtime audit's victims set the stack size from untrusted input
 * and the pre-fix library rounded it unchecked: (size_t)-1 wrapped to a
 * zero-byte stack inside the heap.  This program pins the fix on the real
 * kernel and the real libpagh-c:
 *
 *  - PTHREAD_STACK_MIN (16 KiB) is accepted, page-rounded and works;
 *  - 0, 1, MIN-1, MAX+1 and SIZE_MAX are refused by
 *    pthread_attr_setstacksize() with EINVAL, and a create() with such an
 *    attribute (including one whose public stacksize field was written
 *    directly, the audit's untrusted-config shape) is refused with EINVAL
 *    before any thread exists;
 *  - the attribute records the rejected size, so dropping setstacksize()'s
 *    return value cannot silently fall back to the default stack;
 *  - pthread_attr_init() repairs the attribute, and a normal thread created
 *    with the default (NULL attr), the 64 KiB default and an explicit 32 KiB
 *    stack still runs and returns its value.
 *
 * tools/live-security.py requires "pthread_stack_bounds: ok" and exit 0.
 */
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>

static int checks;
static int failures;
static int worker_ran;

static void check(const char *what, int condition) {
    checks++;
    if (!condition) {
        printf("pthread_stack_bounds: FAIL %s\n", what);
        failures++;
    }
}

static void *worker(void *arg) {
    volatile unsigned char buf[256];
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (unsigned char)i;
    worker_ran++;
    return (void *)((long)arg + 1);
}

static int refused(int rc) { return rc == -1 && errno == EINVAL; }

/* A size both setstacksize() and create() must reject. */
static void expect_refused_size(pthread_attr_t *attr, size_t size) {
    pthread_t thread;
    errno = 0;
    check("setstacksize refuses out-of-range size",
          refused(pthread_attr_setstacksize(attr, size)));
    check("attribute records the rejected size", attr->stacksize == size);
    errno = 0;
    check("create refuses the recorded size",
          refused(pthread_create(&thread, attr, worker, (void *)1)));
}

static void expect_working(pthread_attr_t *attr, long arg) {
    pthread_t thread;
    void *retval = 0;
    worker_ran = 0;
    errno = 0;
    check("create succeeds", pthread_create(&thread, attr, worker, (void *)arg) == 0);
    check("join succeeds", pthread_join(thread, &retval) == 0);
    check("worker ran once", worker_ran == 1);
    check("worker return value survives join", retval == (void *)(arg + 1));
}

int main(void) {
    pthread_attr_t attr;
    pthread_t thread;

    check("attr_init succeeds", pthread_attr_init(&attr) == 0);
    check("attr_init default stack is 64 KiB", attr.stacksize == 64 * 1024);

    /* The default and NULL-attr paths, then explicit small/valid sizes. */
    expect_working(&attr, 1);
    expect_working(0, 2);
    check("setstacksize(PTHREAD_STACK_MIN) succeeds",
          pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN) == 0);
    expect_working(&attr, 3);
    check("setstacksize(32768) succeeds",
          pthread_attr_setstacksize(&attr, 32768) == 0);
    expect_working(&attr, 4);

    /* The audit's size, its wrapping neighbourhood and both boundaries. */
    expect_refused_size(&attr, (size_t)-1);
    expect_refused_size(&attr, (size_t)-4096);
    expect_refused_size(&attr, 0);
    expect_refused_size(&attr, 1);
    expect_refused_size(&attr, PTHREAD_STACK_MIN - 1);
    expect_refused_size(&attr, PTHREAD_STACK_MAX + 1);

    /* The public attribute struct can be written directly. */
    pthread_attr_t raw;
    raw.detachstate = 0;
    raw.stacksize = (size_t)-1;
    errno = 0;
    check("create refuses a directly written SIZE_MAX",
          refused(pthread_create(&thread, &raw, worker, 0)));
    raw.stacksize = 0;
    errno = 0;
    check("create refuses a directly written 0",
          refused(pthread_create(&thread, &raw, worker, 0)));
    raw.stacksize = PTHREAD_STACK_MIN - 1;
    errno = 0;
    check("create refuses a directly written sub-minimum size",
          refused(pthread_create(&thread, &raw, worker, 0)));

    /* The audit's failure mode: the thread stack lands between live heap
     * blocks.  A refused create must not touch them at all. */
    unsigned char *victim1 = malloc(64);
    unsigned char *victim2 = malloc(64);
    if (victim1 && victim2) {
        for (int i = 0; i < 64; i++) {
            victim1[i] = 0x5A;
            victim2[i] = 0x5A;
        }
        pthread_attr_t bad;
        check("attr_init succeeds", pthread_attr_init(&bad) == 0);
        errno = 0;
        check("setstacksize(SIZE_MAX) refuses", refused(pthread_attr_setstacksize(&bad, (size_t)-1)));
        worker_ran = 0;
        errno = 0;
        check("create next to live blocks is refused",
              refused(pthread_create(&thread, &bad, worker, 0)));
        check("refused create ran no worker", worker_ran == 0);
        int intact = 1;
        for (int i = 0; i < 64; i++) {
            if (victim1[i] != 0x5A || victim2[i] != 0x5A) intact = 0;
        }
        check("canaries intact after the refused create", intact);
    } else {
        check("canary blocks allocated", 0);
    }

    /* A caller that ignores the rejection error cannot create a thread
     * until the attribute is reset. */
    check("attr_init repairs the attribute", pthread_attr_init(&attr) == 0);
    errno = 0;
    check("setstacksize(SIZE_MAX) refuses without a thread",
          refused(pthread_attr_setstacksize(&attr, (size_t)-1)));
    errno = 0;
    check("create after the ignored rejection still refuses",
          refused(pthread_create(&thread, &attr, worker, 0)));
    check("attr_init after the rejection succeeds", pthread_attr_init(&attr) == 0);
    expect_working(&attr, 5);

    if (failures) {
        printf("pthread_stack_bounds: %d/%d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("pthread_stack_bounds: ok (%d checks)\n", checks);
    return 0;
}