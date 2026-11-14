/* Host-side fake brk for the allocator tests.
 *
 * Compiled without any headers on purpose: glibc's errno is a macro, while
 * the library sources read a plain `int errno` (as pagh-libc defines it), so
 * this translation unit provides the one definition and a getter, exactly
 * like tests/host_errno_shim.c does for the time tests.
 *
 * The arena is a plain static buffer, so a runaway write or read fails an
 * assertion (or ASAN's global-buffer check) instead of touching the host.
 */
#define FAKE_SYS_BRK 214
#define FAKE_ENOMEM 12
#define ARENA_BYTES (16u << 20)

static unsigned char arena[ARENA_BYTES];
static unsigned long brk_used;
int errno;

long __pagh_syscall3(long nr, long a0, long a1, long a2) {
    (void)a1;
    (void)a2;
    if (nr != FAKE_SYS_BRK) return -38; /* -ENOSYS */
    unsigned long base = (unsigned long)arena;
    if (a0 == 0) return (long)(base + brk_used);
    unsigned long want = (unsigned long)a0;
    if (want < base || want - base > ARENA_BYTES) return -FAKE_ENOMEM;
    brk_used = want - base;
    return (long)want;
}

int *host_errno_slot(void) { return &errno; }