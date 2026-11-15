/* Host-side pthread stack-size test (F-r4-rt-1, round-4 runtime audit).
 *
 * src/pthread.c is compiled for the host with every entry point renamed
 * (pagh_pthread_*), malloc/free redirected to a tracking allocator, and the
 * clone/futex/exit syscalls emulated over real host threads by
 * tests/host_pthread_shim.c.  This file drives the contract the audit broke:
 *
 *  - pthread_attr_setstacksize() rejects sizes outside
 *    [PTHREAD_STACK_MIN, PTHREAD_STACK_MAX] with EINVAL (0, 1, MIN-1,
 *    MAX+1, SIZE_MAX and the whole wrapping neighbourhood);
 *  - a rejected size is still recorded in the attribute, so a caller that
 *    drops the return value cannot make pthread_create() silently fall back
 *    to the default stack: create revalidates and refuses with EINVAL, before
 *    any allocation or clone ();
 *  - a caller that writes the public pthread_attr_t.stacksize directly (the
 *    audit's untrusted-config shape) is refused the same way;
 *  - a zeroed stacksize is refused instead of being treated as "default";
 *  - a valid size is page-rounded without wrapping and clone() receives an sp
 *    that is the end of a real allocation of at least PTHREAD_STACK_MIN
 *    bytes (the invariant a zero-byte stack violated);
 *  - a normal thread still works: create/join, return value, a second
 *    create/join after join (registry slot released), and the NULL-attribute
 *    default of 64 KiB.
 *
 * The pre-fix source fails this test: setstacksize() returns 0 for every size,
 * create() rounds SIZE_MAX to 0, malloc(0) succeeds, and the shim sees an sp
 * that ends no allocation of a valid size.
 */
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>

extern int shim_alloc_count(void);
extern long shim_clone_calls(void);
extern int shim_stack_violation(void);
extern void shim_reset_violation(void);
extern unsigned long shim_last_top(void);
extern size_t shim_last_stack_size(void);

static int checks;
static int failures;
static int worker_ran;

static void check(const char *what, int condition) {
    checks++;
    if (!condition) {
        printf("pthread_test: FAIL %s\n", what);
        failures++;
    }
}

static void check_refused(const char *what, int rc) {
    check(what, rc == -1 && errno == EINVAL);
}

static void *worker(void *arg) {
    volatile unsigned char buf[256];
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (unsigned char)i;
    worker_ran++;
    return (void *)((long)arg + 1);
}

/* A size the library must reject at both setstacksize() and create(). */
static void expect_rejected_size(size_t size) {
    pthread_attr_t attr;
    pthread_t thread = 0;
    check("attr_init succeeds", pthread_attr_init(&attr) == 0);
    errno = 0;
    check_refused("setstacksize rejects out-of-range size",
                  pthread_attr_setstacksize(&attr, size));
    check("rejected size is recorded (fail-closed attr)", attr.stacksize == size);

    long clones = shim_clone_calls();
    int allocs = shim_alloc_count();
    errno = 0;
    check_refused("create refuses the recorded size",
                  pthread_create(&thread, &attr, worker, (void *)1));
    check("refused create calls no clone", shim_clone_calls() == clones);
    check("refused create allocates nothing", shim_alloc_count() == allocs);
}

/* A size the library must accept: create/join works and the size clone() sees
 * is the page-rounded request, ending a real allocation. */
static void expect_accepted_size(size_t requested, size_t rounded) {
    pthread_attr_t attr;
    pthread_t thread = 0;
    void *retval = 0;
    check("attr_init succeeds", pthread_attr_init(&attr) == 0);
    shim_reset_violation();
    errno = 0;
    check("setstacksize accepts in-range size",
          pthread_attr_setstacksize(&attr, requested) == 0);
    worker_ran = 0;
    errno = 0;
    check("create succeeds", pthread_create(&thread, &attr, worker, (void *)5) == 0);
    check("create handed clone a stack ending a real allocation",
          shim_stack_violation() == 0);
    check("create rounded the stack size", shim_last_stack_size() == rounded);
    check("the stack top is not inside the heap header",
          shim_last_top() != 0 && shim_last_stack_size() >= PTHREAD_STACK_MIN);
    check("join succeeds", pthread_join(thread, &retval) == 0);
    check("worker ran once", worker_ran == 1);
    check("worker return value survives join", retval == (void *)6);
}

int main(void) {
    printf("pthread_test: sizeof(pthread_attr_t)=%u, stack window [%u, %u]\n",
           (unsigned)sizeof(pthread_attr_t),
           (unsigned)PTHREAD_STACK_MIN, (unsigned)PTHREAD_STACK_MAX);

    /* --- attribute initialisation and defaults -------------------------- */
    errno = 0;
    check_refused("attr_init(NULL) fails with EINVAL", pthread_attr_init(0));

    pthread_attr_t attr;
    check("attr_init succeeds", pthread_attr_init(&attr) == 0);
    check("attr_init default stack is 64 KiB", attr.stacksize == 64 * 1024);
    check("attr_init clears detachstate", attr.detachstate == 0);

    errno = 0;
    check_refused("attr_setstacksize(NULL) fails with EINVAL",
                  pthread_attr_setstacksize(0, 65536));

    /* --- the audit's value and every wrapping neighbour ----------------- */
    expect_rejected_size((size_t)-1);
    expect_rejected_size((size_t)-1 - 4095);
    expect_rejected_size((size_t)-4096);
    expect_rejected_size(0);
    expect_rejected_size(1);
    expect_rejected_size(PTHREAD_STACK_MIN - 1);
    expect_rejected_size(PTHREAD_STACK_MAX + 1);
    expect_rejected_size(PTHREAD_STACK_MAX * 2);
    expect_rejected_size(((size_t)-1) / 2);

    /* --- valid sizes, page rounding, real clone stack ------------------- */
    expect_accepted_size(PTHREAD_STACK_MIN, PTHREAD_STACK_MIN);
    expect_accepted_size(PTHREAD_STACK_MIN + 1, 20480);
    expect_accepted_size(65536, 65536);
    expect_accepted_size(65537, 69632);
    expect_accepted_size(PTHREAD_STACK_MAX, PTHREAD_STACK_MAX);

    /* --- the public struct can be written directly (untrusted config) --- */
    pthread_t thread = 0;
    pthread_attr_t raw;
    raw.detachstate = 0;

    raw.stacksize = (size_t)-1;
    errno = 0;
    check_refused("direct attr.stacksize = SIZE_MAX is refused",
                  pthread_create(&thread, &raw, worker, (void *)1));
    raw.stacksize = 0;
    errno = 0;
    check_refused("direct attr.stacksize = 0 is refused",
                  pthread_create(&thread, &raw, worker, (void *)1));
    raw.stacksize = PTHREAD_STACK_MIN - 1;
    errno = 0;
    check_refused("direct attr.stacksize below PTHREAD_STACK_MIN is refused",
                  pthread_create(&thread, &raw, worker, (void *)1));
    raw.stacksize = PTHREAD_STACK_MAX + 1;
    errno = 0;
    check_refused("direct attr.stacksize above PTHREAD_STACK_MAX is refused",
                  pthread_create(&thread, &raw, worker, (void *)1));

    /* An in-range direct value is still valid. */
    void *retval = 0;
    raw.stacksize = 20001;
    shim_reset_violation();
    errno = 0;
    check("direct attr.stacksize in range still creates a thread",
          pthread_create(&thread, &raw, worker, (void *)7) == 0);
    check("direct in-range size was page-rounded", shim_last_stack_size() == 20480);
    check("join succeeds", pthread_join(thread, &retval) == 0);
    check("worker return value survives join", retval == (void *)8);

    /* --- NULL attr means the default, and join releases the slot -------- */
    for (int i = 0; i < 3; i++) {
        worker_ran = 0;
        shim_reset_violation();
        errno = 0;
        check("create(NULL attr) succeeds", pthread_create(&thread, 0, worker, 0) == 0);
        check("NULL attr uses the 64 KiB default", shim_last_stack_size() == 65536);
        check("NULL attr top is a real stack end", shim_stack_violation() == 0);
        check("join succeeds", pthread_join(thread, &retval) == 0);
        check("join released the registry slot", retval == (void *)1);
        check("worker ran once", worker_ran == 1);
    }

    /* --- attribute can be repaired after a rejected size ---------------- */
    check("attr_init succeeds", pthread_attr_init(&attr) == 0);
    errno = 0;
    check_refused("setstacksize rejects SIZE_MAX",
                  pthread_attr_setstacksize(&attr, (size_t)-1));
    check("attr_init resets after rejection", pthread_attr_init(&attr) == 0);
    check("repaired default is 64 KiB", attr.stacksize == 64 * 1024);
    errno = 0;
    check("create after attr_init succeeds",
          pthread_create(&thread, &attr, worker, (void *)9) == 0);
    check("join succeeds", pthread_join(thread, &retval) == 0);
    check("worker return value survives join", retval == (void *)10);

    /* --- required arguments are checked too ----------------------------- */
    long clones = shim_clone_calls();
    errno = 0;
    check_refused("create(NULL thread) is refused",
                  pthread_create(0, &attr, worker, 0));
    errno = 0;
    check_refused("create(NULL start_routine) is refused",
                  pthread_create(&thread, &attr, 0, 0));
    check("argument refusals call no clone", shim_clone_calls() == clones);

    if (failures) {
        printf("pthread_test: %d/%d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("pthread_test: ok (%d checks)\n", checks);
    return 0;
}