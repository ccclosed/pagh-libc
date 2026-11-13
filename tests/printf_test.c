/* Differential test for the pagh C library's printf against the host's libc.
 *
 * The library's vsnprintf is renamed at compile time (see tools/host-tests.sh),
 * so both implementations can be called in one process and compared byte for
 * byte. This is how the float formatter is verified: not "it printed something
 * plausible" but "it agrees with glibc on random bit patterns, every precision
 * and the awkward edge cases". */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

int pagh_snprintf(char *buf, unsigned long size, const char *fmt, ...);
int pagh_sprintf(char *buf, const char *fmt, ...);
int pagh_vsprintf(char *buf, const char *fmt, va_list ap);

/* The library's stdio only needs this for printf/puts, which this test never
 * calls through the renamed entry points; the linker wants the symbol. */
long write(int fd, const void *buf, unsigned long n) {
    (void)fd; (void)buf;
    return (long)n;
}

static int failures;
static int checks;

static void compare(const char *what, const char *fmt, const char *got, int ngot,
                    const char *want, int nwant) {
    checks++;
    if (ngot == nwant && strcmp(got, want) == 0) return;
    failures++;
    if (failures <= 40) {
        printf("MISMATCH %s fmt=%s\n  pagh: [%s] (%d)\n  libc: [%s] (%d)\n",
               what, fmt, got, ngot, want, nwant);
    }
}

#define CHECK(fmt, ...) do { \
    char a[600], b[600]; \
    int na = pagh_snprintf(a, sizeof a, fmt, __VA_ARGS__); \
    int nb = snprintf(b, sizeof b, fmt, __VA_ARGS__); \
    compare("value", fmt, a, na, b, nb); \
} while (0)

/* sprintf has no size argument; the pagh formatter must produce exactly what
 * snprintf (with a large buffer) produces, return value included. */
#define CHECK_SPRINTF(fmt, ...) do { \
    char a[600], b[600]; \
    int na = pagh_sprintf(a, fmt, __VA_ARGS__); \
    int nb = snprintf(b, sizeof b, fmt, __VA_ARGS__); \
    compare("sprintf", fmt, a, na, b, nb); \
} while (0)

static int call_vsprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = pagh_vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

#define CHECK_VSPRINTF(fmt, ...) do { \
    char a[600], b[600]; \
    int na = call_vsprintf(a, fmt, __VA_ARGS__); \
    int nb = snprintf(b, sizeof b, fmt, __VA_ARGS__); \
    compare("vsprintf", fmt, a, na, b, nb); \
} while (0)

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;
static uint64_t rng(void) {
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x;
    return x;
}

static double from_bits(uint64_t bits) {
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

static void check_double(double d) {
    static const int precisions[] = { 0, 1, 2, 3, 6, 9, 17, 20 };
    for (unsigned i = 0; i < sizeof precisions / sizeof precisions[0]; i++) {
        int p = precisions[i];
        CHECK("%.*f", p, d);
        CHECK("%.*e", p, d);
        CHECK("%.*g", p, d);
        CHECK("%#.*g", p, d);
        CHECK("%.*E", p, d);
        CHECK("%.*G", p, d);
        CHECK("%+.*f", p, d);
        CHECK("% .*f", p, d);
        CHECK("%*.*f", 24, p, d);
        CHECK("%-24.*f", p, d);
        CHECK("%024.*f", p, d);
        CHECK("%12.4f", d);
        CHECK("%F", d);
        CHECK("%.0f", d);
    }
}

static void fixed_cases(void) {
    static const uint64_t bits[] = {
        0x0000000000000000ULL, /* +0 */
        0x8000000000000000ULL, /* -0 */
        0x3fe0000000000000ULL, /* 0.5 */
        0x3ff8000000000000ULL, /* 1.5 */
        0x4004000000000000ULL, /* 2.5 */
        0x3ff0000000000000ULL, /* 1.0 */
        0x4059000000000000ULL, /* 100.0 */
        0x3fb999999999999aULL, /* 0.1 */
        0x3fd3333333333333ULL, /* 0.3 */
        0x3ff0000000000001ULL, /* 1 + 2^-52 */
        0x4340000000000000ULL, /* 2^53 */
        0x4340000000000001ULL, /* 2^53 + 2 */
        0x7fefffffffffffffULL, /* DBL_MAX */
        0x0010000000000000ULL, /* DBL_MIN */
        0x0000000000000001ULL, /* smallest denormal */
        0x000fffffffffffffULL, /* largest denormal */
        0x7ff0000000000000ULL, /* +inf */
        0xfff0000000000000ULL, /* -inf */
        0x7ff8000000000000ULL, /* nan */
        0xfff8000000000000ULL, /* -nan */
        0xc000000000000000ULL, /* -2.0 */
        0x4023ae147ae147aeULL, /* 9.86 */
        0x3feccccccccccccdULL, /* 0.9 */
        0x4023333333333333ULL, /* 9.6 */
        0x402e666666666666ULL, /* 15.2 */
        0x000000000000000fULL, /* denormal 2.1e-323 */
        0x7fe0000000000000ULL, /* 8.98e307 */
        0x3eb0c6f7a0b5ed8dULL, /* 1e-6 */
        0x3f847ae147ae147bULL, /* 0.01 */
        0x400921fb54442d18ULL, /* pi */
    };
    for (unsigned i = 0; i < sizeof bits / sizeof bits[0]; i++) check_double(from_bits(bits[i]));
    /* A deterministic sweep of random bit patterns across the whole exponent
     * range: normals, subnormals, huge and tiny. */
    for (int i = 0; i < 12000; i++) {
        uint64_t b = rng();
        if ((b & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL) b &= ~0x0008000000000000ULL;
        check_double(from_bits(b));
    }
}

static void integer_cases(void) {
    CHECK("%d", 0);
    CHECK("%d", -1);
    CHECK("%d", 2147483647);
    CHECK("%d", -2147483647 - 1);
    CHECK("%+d", 42);
    CHECK("% d", 42);
    CHECK("%-6d|", 42);
    CHECK("%06d", -42);
    CHECK("%.5d", 42);
    CHECK("%.0d", 0);
    CHECK("%8.3d|", 42);
    CHECK("%ld", 123456789012345L);
    CHECK("%lu", 18446744073709551615UL);
    CHECK("%llu", 18446744073709551615ULL);
    CHECK("%ld", -9223372036854775807L - 1);
    CHECK("%zu", (unsigned long)123456789);
    CHECK("%hhd", 300);
    CHECK("%hhu", 300);
    CHECK("%5.2s|", "abcdef");
    CHECK("%x", 0xdeadbeefu);
    CHECK("%X", 0xdeadbeefu);
    CHECK("%#x", 255u);
    CHECK("%#o", 8u);
    CHECK("%#x", 0u);
    CHECK("%08x", 0x1234u);
    CHECK("%o", 511u);
    CHECK("%-8x|", 0xabcdu);
    CHECK("%p", (void *)0x1234);
    CHECK("%s", "hello");
    CHECK("%10s|", "hi");
    CHECK("%-10s|", "hi");
    CHECK("%.3s", "hello");
    CHECK("%c", 'x');
    CHECK("%5c|", 'x');
    CHECK("%%", 0);
    CHECK("%5.2f%%", 1.0);
    CHECK("a%sb%dc%08.3f", "x", -7, 3.14159);
}

static void precision_cases(void) {
    /* F-rt-2 regression: the integer precision path used to memmove
     * `precision - digits` bytes inside `char text[32]`, so a large precision
     * overwrote the caller's stack.  These cases agree with glibc and cover
     * the 32-byte boundary and the 33..112 sweep from the audit, plus 128 and
     * a precision wider than the working buffer used by older builds. */
    static const int precisions[] = {
        0, 1, 2, 3, 10, 21, 22, 23, 30, 31, 32, 33, 34, 40, 48, 56, 64,
        80, 100, 112, 128, 200, 511,
    };
    for (unsigned i = 0; i < sizeof precisions / sizeof precisions[0]; i++) {
        int p = precisions[i];
        CHECK("%.*d", p, 42);
        CHECK("%.*d", p, 0);
        CHECK("%.*d", p, 2147483647);
        CHECK("%.*d", p, -2147483647 - 1);
        CHECK("%.*i", p, -7);
        CHECK("%.*u", p, 4294967295u);
        CHECK("%.*x", p, 0xdeadbeefu);
        CHECK("%.*X", p, 0xdeadbeefu);
        CHECK("%.*o", p, 511u);
        CHECK("%.*o", p, 0);
        CHECK("%#.*o", p, 0);
        CHECK("%#.*o", p, 8u);
        CHECK("%#.*x", p, 255u);
        CHECK("%.*ld", p, -9223372036854775807L - 1);
        CHECK("%.*lu", p, 18446744073709551615UL);
        CHECK("%.*hhd", p, 300);
        CHECK("%.*hhu", p, 300);
        CHECK("%+.*d", p, 42);
        CHECK("% .*d", p, 42);
        CHECK("%.*d|", p, -42);
        CHECK("%*.*d", 120, p, -42);
        CHECK("%-*.*d", 120, p, -42);
        CHECK("%0*.*d", 120, p, -42);
    }
    /* A negative precision means "omitted", in both implementations. */
    CHECK("%.*d", -1, 42);
    CHECK("%.*d", -123, -42);
    CHECK("%*.*d", 10, -3, 7);
    CHECK("%-*.*d", 10, -3, 7);
    CHECK("%.*u", -5, 7u);
    CHECK("%.*s", -1, "abc");
    /* Literal precision, including the one the audit used. */
    CHECK("%.128d", 42);
    CHECK("%.33d", 42);
    CHECK("%.112u", 42);
    CHECK("%.0d", 0);
    CHECK("%.0u", 0u);
}

int main(void) {
    fixed_cases();
    integer_cases();
    precision_cases();
    printf("printf diff: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
