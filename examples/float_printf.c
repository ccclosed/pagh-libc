/* Floating-point formatting and arithmetic live check.
 *
 *   run /mnt/float_printf
 *
 * Two claims at once: the C library's %f/%e/%g produce the exact strings the
 * host's glibc produces (the same table is compared on the host by
 * tests/printf_test.c over random bit patterns), and U-mode really executes
 * RISC-V floating-point instructions on this kernel -- if sstatus.FS were off,
 * the first arithmetic would trap as an illegal instruction and this program
 * would never print its verdict.
 *
 * Exit status 0 only if every row matches. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

static double from_bits(uint64_t bits) {
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

struct row {
    const char *fmt;
    double value;
    const char *expect;
};

static const struct row rows[] = {
    { "%.2f", 3.14159, "3.14" },
    { "%.0f", 0.5, "0" },
    { "%.0f", 1.5, "2" },
    { "%.0f", 2.5, "2" },
    { "%.20f", 0.1, "0.10000000000000000555" },
    { "%e", 12345.678, "1.234568e+04" },
    { "%.3e", 0.000123456, "1.235e-04" },
    { "%g", 0.0001234, "0.0001234" },
    { "%.3g", 999.9, "1e+03" },
    { "%10.3f", -2.5, "    -2.500" },
    { "%-10.3f", 2.5, "2.500     " },
    { "%+.1f", 0.0, "+0.0" },
    { "%g", 1e20, "1e+20" },
    { "%f", 1e20, "100000000000000000000.000000" },
    { "%e", 1e300, "1.000000e+300" },
    { "%.2f", -0.0, "-0.00" },
    { "%08.3f", 2.0 / 3.0, "0000.667" },
    { "%.15g", 1.0 / 3.0, "0.333333333333333" },
    { "%5.1f", 1.0 / 3.0, "  0.3" },
};

int main(void) {
    int failures = 0;

    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        char got[128];
        snprintf(got, sizeof got, rows[i].fmt, rows[i].value);
        int ok = strcmp(got, rows[i].expect) == 0;
        printf("float: %-8s %-14s -> %-24s %s\n", rows[i].fmt,
               rows[i].expect, got, ok ? "ok" : "MISMATCH");
        if (!ok) failures++;
    }

    /* NaN and the infinities: the bit patterns are built by hand because this
     * library has no <math.h>. */
    {
        char got[32];
        snprintf(got, sizeof got, "%f", from_bits(0x7ff8000000000000ULL));
        int ok = strcmp(got, "nan") == 0;
        printf("float: %%f       nan            -> %-24s %s\n", got, ok ? "ok" : "MISMATCH");
        if (!ok) failures++;
        snprintf(got, sizeof got, "%f", from_bits(0x7ff0000000000000ULL));
        ok = strcmp(got, "inf") == 0;
        printf("float: %%f       inf            -> %-24s %s\n", got, ok ? "ok" : "MISMATCH");
        if (!ok) failures++;
        snprintf(got, sizeof got, "%f", from_bits(0xfff0000000000000ULL));
        ok = strcmp(got, "-inf") == 0;
        printf("float: %%f       -inf           -> %-24s %s\n", got, ok ? "ok" : "MISMATCH");
        if (!ok) failures++;
    }

    /* Floating-point arithmetic in U-mode, then format the result: one live
     * proof that covers both the FPU and %f. */
    double product = 1.5 * 2.25;
    double sum = 0.0;
    for (int i = 1; i <= 100; i++) sum += (double)i;
    double ratio = 355.0 / 113.0;
    printf("float: 1.5*2.25 = %.3f\n", product);
    printf("float: sum(1..100) = %.1f\n", sum);
    printf("float: 355/113 = %.6f\n", ratio);
    if (product != 3.375 || sum != 5050.0) {
        printf("float: FPU arithmetic wrong\n");
        failures++;
    }

    /* Integer formatting with the flags the same parser now handles. */
    {
        char got[64];
        snprintf(got, sizeof got, "[%+d|%-6d|%06d|%#x|%.3d]", 42, 42, -42, 255, 7);
        int ok = strcmp(got, "[+42|42    |-00042|0xff|007]") == 0;
        printf("float: int flags                 -> %-24s %s\n", got, ok ? "ok" : "MISMATCH");
        if (!ok) failures++;
    }

    if (failures) {
        printf("float: %d check(s) failed\n", failures);
        return 1;
    }
    printf("float: ok (%u formatted values + FPU arithmetic)\n",
           (unsigned)(sizeof rows / sizeof rows[0]) + 3);
    return 0;
}
