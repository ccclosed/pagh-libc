/* Bounded integer width/precision: on-guest regression for the F-rt-2 stack
 * overflow in vsnprintf.
 *
 * The audit's PoC (attack/runtime/pocs/libc/precision_smash.c) formats
 * `snprintf(buf, sizeof buf, "%.*d", atoi(argv[1]), 42)`: the library's integer
 * path used to memmove `precision - digits` bytes inside `char text[32]`, so
 * precision 128 overwrote the saved return address with "00000042" and the
 * process died fetching instructions from 0x3234303030303030.
 *
 * This program drives the same shape over the boundary and checks the contract
 * instead of the absence of a crash alone: the returned length is the
 * unrelated-to-buffer untruncated length, the buffer is NUL-terminated, bytes
 * past the terminator keep their canary, and an attacker-sized precision does
 * not write past the destination.  tools/live-security.py requires these
 * markers in the serial transcript, so a regression fails the live gate, not
 * just this program.
 */
#include <stdio.h>
#include <string.h>

#define CANARY 0x5A
#define BIG 300

static int failures;
static int checks;

static void fail(const char *what, int v) {
    printf("printf_bounds: FAIL %s (%d)\n", what, v);
    failures++;
}

static void expect_int(const char *what, int got, int want) {
    checks++;
    if (got != want) fail(what, got);
}

/* printf("%.*d", precision, 42) == `precision` zeros + "42" (at least "42"),
 * truncated to the destination; the return value is never truncated. */
static void precision_case(int p) {
    char buf[BIG];
    int want = p > 2 ? p : 2;
    memset(buf, CANARY, sizeof buf);
    int n = snprintf(buf, 256, "%.*d", p, 42);
    if (n != want) fail("return value", n);
    if (n < 256) {
        if (buf[n] != 0) fail("NUL terminator", n);
        if (p > 2) {
            int bad = -1;
            for (int i = 0; i < n - 2; i++)
                if (buf[i] != '0') { bad = i; break; }
            if (bad >= 0) fail("zero extension", bad);
        }
        if (buf[n - 2] != '4' || buf[n - 1] != '2') fail("digits", p);
        for (int i = n + 1; i < (int)sizeof buf; i++)
            if ((unsigned char)buf[i] != CANARY) { fail("canary after output", i); break; }
    } else {
        if (buf[255] != 0) fail("truncated terminator", 255);
        if ((unsigned char)buf[256] != CANARY) fail("write past destination", 256);
    }
}

int main(void) {
    /* The audit's sweep: 33..112 silently corrupted the stack and 128 killed
     * the process.  Cover the 32-byte boundary on both sides as well. */
    for (int p = 3; p <= 112; p++) precision_case(p);
    precision_case(128);
    precision_case(200);
    precision_case(1000);
    printf("printf_bounds: precision 3..112, 128, 200, 1000 ok (%d cases)\n", 115);

    /* Unsigned/hex shared the same code; negative precision means "omitted". */
    {
        char buf[BIG];
        int n = snprintf(buf, sizeof buf, "%.*x", 128, 0x2au);
        expect_int("%.*x return", n, 128);
        checks++;
        if (buf[0] != '0' || buf[126] != '2' || buf[127] != 'a' || buf[128] != 0)
            fail("%.*x digits", n);
        n = snprintf(buf, sizeof buf, "%.*d", -1, 42);
        expect_int("negative precision return", n, 2);
        checks++;
        if (strcmp(buf, "42")) fail("negative precision output", n);
        n = snprintf(buf, sizeof buf, "%*.*d", 10, -3, 7);
        expect_int("%*.*d negative precision return", n, 10);
        checks++;
        if (strcmp(buf, "         7")) fail("%*.*d negative precision output", n);
        printf("printf_bounds: %%.*x/negative precision ok\n");
    }

    /* Width plus precision together, and a huge precision/width that would
     * need a per-byte loop to be a DoS.  Both stay inside the destination and
     * still report the untruncated length. */
    {
        char buf[380];
        memset(buf, CANARY, sizeof buf);
        int n = snprintf(buf, 320, "%*.*u", 120, 40, 7u);
        expect_int("%*.*u return", n, 120);
        checks++;
        if (buf[0] != ' ' || buf[80] != '0' || buf[119] != '7' || buf[120] != 0)
            fail("%*.*u layout", n);
        memset(buf, CANARY, sizeof buf);
        n = snprintf(buf, 320, "%.*d", 100000, 42);
        expect_int("huge precision return", n, 100000);
        checks++;
        if (buf[319] != 0) fail("huge precision terminator", 319);
        for (int i = 320; i < (int)sizeof buf; i++)
            if ((unsigned char)buf[i] != CANARY) { fail("huge precision wrote past destination", i); break; }
        n = snprintf(buf, 320, "%*d", 1000000000, 42);
        expect_int("huge width return", n, 1000000000);
        n = snprintf(buf, 320, "%.*d", 1000000000, 42);
        expect_int("1e9 precision return", n, 1000000000);
        printf("printf_bounds: width/precision bounds ok (no per-byte stack write)\n");
    }

    if (failures) {
        printf("printf_bounds: FAIL (%d failures, %d checks)\n", failures, checks);
        return 1;
    }
    printf("printf_bounds: ok (%d checks)\n", checks);
    return 0;
}