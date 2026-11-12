/* Differential test for the pagh C library's formatted input: sscanf,
 * vsscanf and fscanf against the host libc.
 *
 * src/scanf.c is compiled with its public symbols renamed (tools/host-tests.sh)
 * and its only stream hook, fgetc, redirected to a synthetic byte source, so
 * both scanners run in the same process:
 *
 *  - sscanf/vsscanf are compared against glibc's sscanf over hand-written edge
 *    cases (signs, bases, width, suppression, scansets, %n, EOF/matching
 *    failure) and deterministic fuzz for integers, strings and correctly
 *    rounded decimal doubles;
 *  - fscanf is compared against glibc's fscanf over the same byte stream
 *    (glibc gets an fmemopen() stream, the pagh scanner the stub feed), which
 *    exercises the FILE path end to end;
 *  - vsscanf is additionally checked through a small variadic wrapper.
 *
 *   ./build/host_scanf_test
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Renamed pagh implementation (compiled from src/scanf.c). */
extern int pagh_sscanf(const char *str, const char *format, ...);
extern int pagh_vsscanf(const char *str, const char *format, va_list ap);
extern int pagh_fscanf(void *stream, const char *format, ...);
extern int pagh_vfscanf(void *stream, const char *format, va_list ap);

/* Synthetic stream: src/scanf.c reads through fgetc, renamed to this. */
static unsigned char feed[1024];
static size_t feed_len;
static size_t feed_pos;

int pagh_scan_fgetc(void *stream) {
    (void)stream;
    if (feed_pos >= feed_len) return -1;
    return feed[feed_pos++];
}

static void feed_bytes(const char *data) {
    feed_len = strlen(data);
    if (feed_len > sizeof feed) feed_len = sizeof feed;
    memcpy(feed, data, feed_len);
    feed_pos = 0;
}

static int checks;
static int failures;

static void ok(int condition, const char *what, const char *detail) {
    checks++;
    if (condition) return;
    failures++;
    if (failures <= 40) printf("FAIL %s: %s\n", what, detail);
}

static void compare_counts(const char *what, int pagh, int host) {
    checks++;
    if (pagh == host) return;
    failures++;
    if (failures <= 40) printf("FAIL %s: return %d, host %d\n", what, pagh, host);
}

/* ---- integer scans ------------------------------------------------------ */

static void check_int2(const char *input, const char *format) {
    int a1 = 111, b1 = 222, n1 = -1;
    int a2 = 111, b2 = 222, n2 = -1;
    int r1 = pagh_sscanf(input, format, &a1, &b1, &n1);
    int r2 = sscanf(input, format, &a2, &b2, &n2);
    compare_counts("int2", r1, r2);
    ok(a1 == a2 && b1 == b2 && n1 == n2, "int2 values", "stored value mismatch");
}

static void check_ll2(const char *input, const char *format) {
    long long a1 = 111, b1 = 222, n1 = -1;
    long long a2 = 111, b2 = 222, n2 = -1;
    int r1 = pagh_sscanf(input, format, &a1, &b1, &n1);
    int r2 = sscanf(input, format, &a2, &b2, &n2);
    compare_counts("ll2", r1, r2);
    ok(a1 == a2 && b1 == b2 && n1 == n2, "ll2 values", "stored value mismatch");
}

static void test_integers(void) {
    static const char *const cases[][2] = {
        { "42", "%d" },
        { "  42", "%d" },
        { "\t\n-17", "%d" },
        { "+9", "%d" },
        { "12 34", "%d %d" },
        { "12,34", "%d,%d" },
        { "12 , 34", "%d , %d" },
        { "123456", "%3d" },
        { "  -0042rest", "%d" },
        { "abc", "%d" },
        { "", "%d" },
        { "12", "%d %d" },
        { "12 34 56", "%d %*d %d" },
        { "12 34 56", "%d%n %d" },
        { "12 34", "%i %i" },
        { "0x1f 010 99", "%i %i %i" },
        { "0x1f 0X20", "%x %X" },
        { "777", "%o" },
        { "4294967295", "%u" },
        { "-42 42", "%d %u" },
        { "12abc34", "%dabc%d" },
        { "12abc34", "%dabd%d" },
        { "%", "%%" },
        { "  %  ", " % % " },
        { "12", "%2d" },
        { " 12", "%4d" },
        { "12 34", "%2d %2d" },
        { "0", "%x" },
        { "0x", "%x" },
        { "0xg", "%x" },
        { "0x2a", "%i" },
        { "08", "%i" },
        { "-0x10", "%i" },
        { "+42", "%d" },
        { "18446744073709551615", "%llu" },
        { "12", "%1d" },
        { "abc42", "%d" },
        { "42abc", "%d%n" },
        { "0000", "%d" },
        { "-0", "%d" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        check_int2(cases[i][0], cases[i][1]);

    check_ll2("9223372036854775807 -42", "%lld %lld");
    check_ll2("123456789012 7", "%12lld %lld");
    /* short/long modifiers: compared through widened temporaries. */
    {
        short s1 = 1, s2 = 1;
        long l1 = 1, l2 = 1;
        int r1 = pagh_sscanf("-42 123456", "%hd %ld", &s1, &l1);
        int r2 = sscanf("-42 123456", "%hd %ld", &s2, &l2);
        compare_counts("hd/ld", r1, r2);
        ok(s1 == s2 && l1 == l2, "hd/ld values", "stored value mismatch");
    }
    {
        signed char c1 = 1, c2 = 1;
        unsigned char u1 = 1, u2 = 1;
        int r1 = pagh_sscanf("-5 250", "%hhd %hhu", &c1, &u1);
        int r2 = sscanf("-5 250", "%hhd %hhu", &c2, &u2);
        compare_counts("hhd/hhu", r1, r2);
        ok(c1 == c2 && u1 == u2, "hhd/hhu values", "stored value mismatch");
    }
    {
        void *p1 = (void *)1, *p2 = (void *)1;
        int r1 = pagh_sscanf("0x1234", "%p", &p1);
        int r2 = sscanf("0x1234", "%p", &p2);
        compare_counts("p", r1, r2);
        ok(p1 == p2, "p value", "stored pointer mismatch");
    }
    {
        size_t z1 = 1, z2 = 1;
        int r1 = pagh_sscanf("4096", "%zu", &z1);
        int r2 = sscanf("4096", "%zu", &z2);
        compare_counts("zu", r1, r2);
        ok(z1 == z2, "zu value", "stored value mismatch");
    }
}

/* ---- string/character scans -------------------------------------------- */

static void check_str2(const char *input, const char *format) {
    char s1[64] = "pagh", s2[64] = "pagh";
    int n1 = -1, n2 = -1;
    int r1 = pagh_sscanf(input, format, s1, &n1);
    int r2 = sscanf(input, format, s2, &n2);
    compare_counts("str2", r1, r2);
    ok(strcmp(s1, s2) == 0 && n1 == n2, "str2 values", "stored string/n mismatch");
}

static void check_chars(const char *input, const char *format) {
    char a1[16], a2[16];
    memset(a1, '?', sizeof a1);
    memset(a2, '?', sizeof a2);
    int r1 = pagh_sscanf(input, format, a1);
    int r2 = sscanf(input, format, a2);
    compare_counts("chars", r1, r2);
    ok(memcmp(a1, a2, sizeof a1) == 0, "chars values", "stored characters mismatch");
}

static void test_strings(void) {
    static const char *const str_cases[][2] = {
        { "hello", "%s" },
        { "  hello world", "%s" },
        { "hello", "%3s" },
        { "hello", "%10s" },
        { "  hello", "%*s %s" },
        { "abc123", "%[a-z]%n" },
        { "abc123", "%[^0-9]%n" },
        { "abc-def", "%[a-c-]%n" },
        { "]", "%[]]" },
        { "xyz", "%[^abc]%n" },
        { "  xyz", "%s%n" },
        { "abc def", "%s %s" },
        { "abcdef", "%*3s%s" },
        { "a b", "%[^ ] %s" },
        { "-abc", "%[-a-z]%n" },
        { "123abc", "%[^a-z]%n" },
        { "x", "%[a-z]" },
        { "", "%[a-z]" },
        { "hello, world!", "%[^,], %s" },
        { "", "%s" },
        { "   ", "%s" },
    };
    for (size_t i = 0; i < sizeof str_cases / sizeof str_cases[0]; i++)
        check_str2(str_cases[i][0], str_cases[i][1]);

    check_chars("abc", "%c");
    check_chars("abc", "%3c");
    check_chars("abc", "%2c");
    check_chars(" a", "%c");
    check_chars("", "%c");
    check_chars("ab", "%c");
}

/* ---- floating point ----------------------------------------------------- */

static void check_double2(const char *input, const char *format) {
    double a1 = 1.5, b1 = 2.5;
    double a2 = 1.5, b2 = 2.5;
    int n1 = -1, n2 = -1;
    int r1 = pagh_sscanf(input, format, &a1, &b1, &n1);
    int r2 = sscanf(input, format, &a2, &b2, &n2);
    compare_counts("double2", r1, r2);
    ok(memcmp(&a1, &a2, sizeof a1) == 0 && memcmp(&b1, &b2, sizeof b1) == 0 &&
           n1 == n2,
       "double2 values", "bit mismatch");
}

static void check_float1(const char *input, const char *format) {
    float a1 = 1.5f, a2 = 1.5f;
    int r1 = pagh_sscanf(input, format, &a1);
    int r2 = sscanf(input, format, &a2);
    compare_counts("float1", r1, r2);
    ok(memcmp(&a1, &a2, sizeof a1) == 0, "float1 value", "bit mismatch");
}

static void test_floats(void) {
    static const char *const cases[][2] = {
        { "3.14159", "%lf" },
        { " -0.5 ", "%lf" },
        { "1e3", "%lf" },
        { "1E-3", "%lf" },
        { "123.456e+2", "%lf" },
        { "0.0001", "%lf" },
        { "abc", "%lf" },
        { "", "%lf" },
        { "1.5 2.5", "%lf %lf" },
        { "1.5x", "%lf%n" },
        { "6.25", "%f" },
        { "2.5e2", "%f" },
        { "1.25", "%3lf" },
        { "1e", "%lf" },
        { "1e+", "%lf" },
        { "12.5%%", "%lf%%" },
        { "+3.5", "%lf" },
        { ".5", "%lf" },
        { "5.", "%lf" },
        { "1e-22", "%lf" },
        { "123456789012345e5", "%lf" },
        { "-0.0", "%lf" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        check_double2(cases[i][0], cases[i][1]);
    check_float1("6.25", "%f");
    check_float1("-0.125", "%f");

    /* Deterministic fuzz in the correctly-rounded range: at most 15
     * significant digits and a decimal exponent in +-22. */
    unsigned long long state = 0x9e3779b97f4a7c15ULL;
    for (int round = 0; round < 500; round++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        unsigned long long mantissa = state % 1000000000000000ULL; /* 15 digits */
        int exponent = (int)((state >> 32) % 45) - 22;
        char input[64];
        snprintf(input, sizeof input, "%llue%d", mantissa, exponent);
        check_double2(input, "%lf");
    }

    /* Integer fuzz with arbitrary whitespace/signs. */
    for (int round = 0; round < 300; round++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        int a = (int)(state % 2000001) - 1000000;
        int b = (int)((state >> 20) % 2000001) - 1000000;
        const char *seps[] = { " ", "\t", ",", " , ", "\n" };
        const char *sep = seps[state % 5];
        char input[64];
        snprintf(input, sizeof input, "%d%s%d", a, sep, b);
        char fmt[32];
        if (strcmp(sep, ",") == 0) snprintf(fmt, sizeof fmt, "%%d,%%d");
        else if (strcmp(sep, " , ") == 0) snprintf(fmt, sizeof fmt, "%%d , %%d");
        else snprintf(fmt, sizeof fmt, "%%d %%d");
        check_int2(input, fmt);
    }
}

/* ---- FILE path and vsscanf --------------------------------------------- */

/* The FILE-path checks use matching destination types: fscanf is variadic,
 * so a wrong destination type would corrupt the stack, not test the scanner. */
static FILE *host_mem_stream(const char *data, char **copy_out) {
    char *copy = malloc(strlen(data) + 1);
    if (!copy) return NULL;
    strcpy(copy, data);
    FILE *stream = fmemopen(copy, strlen(copy), "r");
    if (!stream) {
        free(copy);
        return NULL;
    }
    *copy_out = copy;
    return stream;
}

static void check_fscanf(const char *data, const char *format) {
    feed_bytes(data);
    int a1 = 111, b1 = 222, n1 = -1;
    int a2 = 111, b2 = 222, n2 = -1;
    char *copy = NULL;
    FILE *host_stream = host_mem_stream(data, &copy);
    if (!host_stream) return;
    int r2 = fscanf(host_stream, format, &a2, &b2, &n2);
    fclose(host_stream);
    free(copy);
    int r1 = pagh_fscanf((void *)1, format, &a1, &b1, &n1);
    compare_counts("fscanf", r1, r2);
    ok(a1 == a2 && b1 == b2 && n1 == n2, "fscanf values", "stored value mismatch");
}

static void check_fscanf_str(const char *data, const char *format) {
    feed_bytes(data);
    char a1[64] = "pagh", b1[64] = "pagh";
    char a2[64] = "pagh", b2[64] = "pagh";
    int n1 = -1, n2 = -1;
    char *copy = NULL;
    FILE *host_stream = host_mem_stream(data, &copy);
    if (!host_stream) return;
    int r2 = fscanf(host_stream, format, a2, b2, &n2);
    fclose(host_stream);
    free(copy);
    int r1 = pagh_fscanf((void *)1, format, a1, b1, &n1);
    compare_counts("fscanf str", r1, r2);
    ok(strcmp(a1, a2) == 0 && strcmp(b1, b2) == 0 && n1 == n2,
       "fscanf str values", "stored value mismatch");
}

static void check_fscanf_double(const char *data, const char *format) {
    feed_bytes(data);
    double a1 = 1.5, b1 = 2.5, a2 = 1.5, b2 = 2.5;
    int n1 = -1, n2 = -1;
    char *copy = NULL;
    FILE *host_stream = host_mem_stream(data, &copy);
    if (!host_stream) return;
    int r2 = fscanf(host_stream, format, &a2, &b2, &n2);
    fclose(host_stream);
    free(copy);
    int r1 = pagh_fscanf((void *)1, format, &a1, &b1, &n1);
    compare_counts("fscanf double", r1, r2);
    ok(memcmp(&a1, &a2, sizeof a1) == 0 && memcmp(&b1, &b2, sizeof b1) == 0 &&
           n1 == n2,
       "fscanf double values", "bit mismatch");
}

static int call_vsscanf(const char *input, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int result = pagh_vsscanf(input, format, ap);
    va_end(ap);
    return result;
}

static void test_stream_and_vsscanf(void) {
    check_fscanf("12 34", "%d %d%n");
    check_fscanf("  -7 0x10", "%d %x%n");
    check_fscanf_str("alpha beta", "%s %s%n");
    check_fscanf_double("3.5 4.5", "%lf %lf%n");
    check_fscanf("abc", "%d");
    check_fscanf("", "%d");

    int a = 0, b = 0, n = -1;
    int r = call_vsscanf("8 9 rest", "%d %d%n", &a, &b, &n);
    compare_counts("vsscanf", r, 2);
    ok(a == 8 && b == 9 && n == 3, "vsscanf values", "stored values");
}

int main(void) {
    test_integers();
    test_strings();
    test_floats();
    test_stream_and_vsscanf();
    if (failures) {
        printf("scanf: %d checks, %d FAILURES\n", checks, failures);
        return 1;
    }
    printf("scanf: %d checks, 0 failures\n", checks);
    return 0;
}