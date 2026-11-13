/* Bounds test for the pagh formatter's integer width/precision path.
 *
 * The library's vsnprintf is renamed at compile time (see tools/host-tests.sh)
 * and this file is compiled with AddressSanitizer, so it sees exactly the bug
 * from the runtime audit as a stack-buffer-overflow instead of silently
 * corrupting its caller: F-rt-2 was `memmove(text + precision - n, ...)` into
 * `char text[32]` for forms like snprintf(buf, n, "%.*d", 128, 42), which
 * overwrote the saved return address.
 *
 * The test checks the contract rather than just survival: snprintf still
 * returns the untruncated length, the buffer stays terminated, bytes past the
 * terminator are untouched, and an attacker-sized precision or width is
 * formatted in constant time (the zero extension is a fill, not a per-byte
 * stack write or a per-byte emit loop). */
#include <stdio.h>
#include <string.h>

int pagh_snprintf(char *buf, unsigned long size, const char *fmt, ...);

/* The library's stdio only needs this for printf/puts, which this test never
 * calls through the renamed entry points; the linker wants the symbol. */
long write(int fd, const void *buf, unsigned long n) {
    (void)fd; (void)buf;
    return (long)n;
}

#define CANARY 0x5A
static int failures;
static int checks;

static void fail(const char *what, int detail) {
    failures++;
    if (failures <= 40) printf("MISMATCH %s (%d)\n", what, detail);
}

static void check_len(const char *what, int got, int want) {
    checks++;
    if (got != want) fail(what, got - want);
}

/* Every byte from `from` to sizeof buf-1 must still hold the canary. */
static void check_canary(const char *what, const unsigned char *buf, unsigned long from,
                         unsigned long size) {
    checks++;
    for (unsigned long i = from; i < size; i++)
        if (buf[i] != CANARY) { fail(what, (int)i); return; }
}

static void fill(unsigned char *buf, unsigned long size) {
    memset(buf, CANARY, size);
}

/* "%.*d", p, 42 == p zeros + "42", and snprintf(n) == max(p, 2). */
static void precision_sweep(void) {
    static const int precisions[] = {
        0, 1, 2, 3, 10, 21, 22, 23, 30, 31, 32, 33, 34, 40, 48, 56, 64,
        80, 100, 112, 128, 200, 511, 1000,
    };
    unsigned char buf[320];
    for (unsigned i = 0; i < sizeof precisions / sizeof precisions[0]; i++) {
        int p = precisions[i];
        int want = p > 2 ? p : 2;
        fill(buf, sizeof buf);
        int n = pagh_snprintf((char *)buf, 300, "%.*d", p, 42);
        check_len("%.*d return", n, want);
        checks++;
        if (n < 300) {
            if (buf[n] != 0) { fail("%.*d terminator", n); continue; }
            for (int j = 0; j < n - 2; j++)
                if (buf[j] != '0') { fail("%.*d zero extension", j); break; }
            if (buf[n - 2] != '4' || buf[n - 1] != '2') fail("%.*d digits", p);
            check_canary("%.*d canary", buf, (unsigned long)n + 1, sizeof buf);
        } else {
            checks++;
            if (buf[299] != 0) fail("%.*d truncated terminator", p);
            check_canary("%.*d truncated canary", buf, 300, sizeof buf);
        }
    }
    /* The unsigned and hex paths shared the same bug. */
    static const int few[] = {33, 64, 100, 112, 128};
    for (unsigned i = 0; i < sizeof few / sizeof few[0]; i++) {
        int p = few[i];
        fill(buf, sizeof buf);
        int n = pagh_snprintf((char *)buf, 300, "%.*x", p, 0x2au);
        check_len("%.*x return", n, p > 2 ? p : 2);
        checks++;
        if (n < 300) {
            if (buf[n] != 0) fail("%.*x terminator", n);
            if (buf[n - 2] != '2' || buf[n - 1] != 'a') fail("%.*x digits", p);
            check_canary("%.*x canary", buf, (unsigned long)n + 1, sizeof buf);
        }
    }
}

/* Width still pads, and width + precision together stay bounded. */
static void width_and_precision(void) {
    unsigned char buf[320];
    fill(buf, sizeof buf);
    int n = pagh_snprintf((char *)buf, 300, "%*.*u", 120, 40, 7u);
    check_len("%*.*u return", n, 120);
    checks++;
    if (buf[0] != ' ' || buf[79] != ' ' || buf[80] != '0' ||
        buf[118] != '0' || buf[119] != '7' || buf[120] != 0)
        fail("%*.*u padding", 0);
    check_canary("%*.*u canary", buf, 121, sizeof buf);

    fill(buf, sizeof buf);
    n = pagh_snprintf((char *)buf, 300, "%.*d", -1, 42);
    check_len("negative precision", n, 2);
    checks++;
    if (buf[0] != '4' || buf[1] != '2' || buf[2] != 0) fail("negative precision digits", 0);
    check_canary("negative precision canary", buf, 3, sizeof buf);

    fill(buf, sizeof buf);
    n = pagh_snprintf((char *)buf, 300, "%*.*d", 10, -3, 7);
    check_len("%*.*d negative precision", n, 10);
    checks++;
    if (buf[9] != '7' || buf[10] != 0) fail("%*.*d negative precision digits", 0);
}

/* A precision/width larger than the destination must not write past size-1 and
 * must not cost one iteration per output byte.  The old code walked this many
 * bytes past a 32-byte stack array and died long before finishing. */
static void huge_fields(void) {
    unsigned char small[8];
    fill(small, sizeof small);
    int n = pagh_snprintf((char *)small, sizeof small, "%.*d", 100000, 42);
    check_len("huge precision return", n, 100000);
    checks++;
    if (small[7] != 0) fail("huge precision terminator", 0);
    if (small[0] != '0' || small[6] != '0') fail("huge precision prefix", 0);

    fill(small, sizeof small);
    n = pagh_snprintf((char *)small, sizeof small, "%*d", 100000, 42);
    check_len("huge width return", n, 100000);
    checks++;
    if (small[7] != 0 || small[0] != ' ') fail("huge width padding", 0);

    /* Constant-time fill: a per-byte loop over 10^9 zeros cannot pass the
     * host-tests gate in a sane time. */
    n = pagh_snprintf((char *)small, sizeof small, "%.*d", 1000000000, 42);
    check_len("1e9 precision return", n, 1000000000);
    n = pagh_snprintf((char *)small, sizeof small, "%*d", 1000000000, 42);
    check_len("1e9 width return", n, 1000000000);

    /* size 1 keeps only the terminator; size 0 writes nothing at all. */
    unsigned char one[4];
    fill(one, sizeof one);
    n = pagh_snprintf((char *)one, 1, "%.*d", 128, 42);
    check_len("size=1 return", n, 128);
    checks++;
    if (one[0] != 0) fail("size=1 terminator", 0);
    check_canary("size=1 canary", one, 1, sizeof one);

    fill(one, sizeof one);
    n = pagh_snprintf((char *)one, 0, "%.*d", 128, 42);
    check_len("size=0 return", n, 128);
    check_canary("size=0 nothing written", one, 0, sizeof one);
}

int main(void) {
    precision_sweep();
    width_and_precision();
    huge_fields();
    printf("printf safety: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}