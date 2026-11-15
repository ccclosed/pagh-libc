/* Host-side shim for the pthread stack-size test (F-r4-rt-1).
 *
 * src/pthread.c is compiled for the host with every pthread_* entry point
 * renamed (pagh_pthread_*), malloc/free renamed to pagh_thread_malloc/free,
 * and this file supplies the three pieces the library normally gets from the
 * kernel and the allocator:
 *
 *  - a tracking allocator on top of the host's malloc, so the test can assert
 *    that a successful create handed clone() an sp that is the end of a real
 *    block of at least PTHREAD_STACK_MIN bytes (the pre-fix code rounded
 *    SIZE_MAX to 0, malloc(0) succeeded, and sp pointed into live heap);
 *  - clone()/exit()/gettid()/futex() over real host threads, so a normal
 *    create/join is exercised for real; clone() records the sp it was given
 *    and flags it if no tracked allocation ends there;
 *  - a no-op-ish futex that yields instead of sleeping, because the test must
 *    not depend on timing.
 *
 * This translation unit is compiled *without* -Iinclude so <pthread.h> is the
 * host's: the pagh source's renamed entry points never collide with glibc's.
 */
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define FAKE_SYS_CLONE 220
#define FAKE_SYS_FUTEX 98
#define FAKE_SYS_GETTID 178
#define FAKE_SYS_EXIT 93

#define FAKE_FUTEX_WAIT 0
#define FAKE_FUTEX_WAKE 1
#define FAKE_FUTEX_PRIVATE 128

#define FAKE_STACK_MIN 16384
#define FAKE_STACK_MAX (1024 * 1024)

#define MAX_TRACKED 64

static struct {
    void *ptr;
    size_t size;
} tracked[MAX_TRACKED];
static int tracked_count;

static long clone_calls;
static long fake_tid = 100;
static int stack_violation;
static unsigned long last_top;
static size_t last_stack_size;

void *pagh_thread_malloc(size_t size) {
    /* malloc(0) must stay observable as a zero-byte block: that is exactly
     * what the pre-fix thread stack looked like. */
    void *p = malloc(size ? size : 1);
    if (!p) return 0;
    if (tracked_count < MAX_TRACKED) {
        tracked[tracked_count].ptr = p;
        tracked[tracked_count].size = size;
        tracked_count++;
    }
    return p;
}

void pagh_thread_free(void *ptr) {
    if (ptr) free(ptr);
}

int shim_alloc_count(void) { return tracked_count; }
long shim_clone_calls(void) { return clone_calls; }
int shim_stack_violation(void) { return stack_violation; }
void shim_reset_violation(void) { stack_violation = 0; }
unsigned long shim_last_top(void) { return last_top; }
size_t shim_last_stack_size(void) { return last_stack_size; }

struct clone_args {
    void (*trampoline)(unsigned long);
    unsigned long raw;
};

static void *host_thread(void *arg) {
    struct clone_args c = *(struct clone_args *)arg;
    free(arg);
    c.trampoline(c.raw);
    return 0;
}

long __pagh_syscall6(long nr, long a0, long a1, long a2, long a3,
                     long a4, long a5) {
    (void)a4;
    (void)a5;
    if (nr == FAKE_SYS_CLONE) {
        clone_calls++;
        last_top = (unsigned long)a1;
        if (a1 & 15) stack_violation |= 1;
        int found = 0;
        for (int i = tracked_count - 1; i >= 0; i--) {
            if ((unsigned long)tracked[i].ptr + tracked[i].size == last_top) {
                found = 1;
                last_stack_size = tracked[i].size;
                break;
            }
        }
        if (!found) {
            stack_violation |= 2;
        } else if (last_stack_size < FAKE_STACK_MIN ||
                   last_stack_size > FAKE_STACK_MAX) {
            stack_violation |= 4;
        }
        struct clone_args *args = malloc(sizeof *args);
        if (!args) return -11; /* -EAGAIN */
        args->trampoline = (void (*)(unsigned long))a2;
        args->raw = (unsigned long)a3;
        pthread_t thread;
        if (pthread_create(&thread, 0, host_thread, args) != 0) {
            free(args);
            return -11;
        }
        pthread_detach(thread);
        return (long)fake_tid++;
    }
    if (nr == FAKE_SYS_FUTEX) {
        int op = (int)a1 & ~FAKE_FUTEX_PRIVATE;
        if (op == FAKE_FUTEX_WAIT) {
            while (*(volatile int *)a0 == (int)a2) sched_yield();
        }
        /* FAKE_FUTEX_WAKE needs no work: the waiter re-reads the word. */
        return 0;
    }
    return -38; /* -ENOSYS */
}

long __pagh_syscall3(long nr, long a0, long a1, long a2) {
    (void)a0;
    (void)a1;
    (void)a2;
    if (nr == FAKE_SYS_EXIT) {
        pthread_exit(0);
    }
    if (nr == FAKE_SYS_GETTID) return 7;
    return -38; /* -ENOSYS */
}