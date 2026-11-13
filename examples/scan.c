/* Live check for the formatted-input surface added in 0.15.0:
 * sscanf/vsscanf/fscanf (and the sprintf/vsprintf wrappers over the formatter).
 *
 * The program writes a small text file on the ext2 image mounted at /mnt,
 * then parses it three ways:
 *
 *  1. getline-style `fgets` plus `sscanf` with a mixed format
 *     ("%d %d %lf %31s%n"), checking counts, sums, words and the %n offset;
 *  2. the same file through `fscanf` (the FILE path), record by record;
 *  3. an in-memory `sprintf` round-trip of the aggregate and a set of
 *     failure/edge semantics (EOF vs matching failure, %n, suppression,
 *     scansets) against hard-coded expectations.
 *
 * It prints one marker per step and ends with "scan: ok"; the exit code is
 * non-zero at the first failure.
 *
 *   run /mnt/scan
 *
 * Exit codes: 0 success, 1 write, 2 fgets+sscanf, 3 sprintf, 4 fscanf,
 * 5 edge semantics. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH "/mnt/scan-input.txt"

static const int EXPECTED_A[3] = { 42, 100, -5 };
static const int EXPECTED_B[3] = { -7, 200, 8 };
static const double EXPECTED_C[3] = { 3.5, 1.25, 0.5 };
static const char *const EXPECTED_WORD[3] = { "alpha", "beta", "gamma" };

static const char *const RECORDS[3] = {
    "42 -7 3.5 alpha\n",
    "100 200 1.25 beta\n",
    "  -5   8 0.5 gamma trailing\n",
};

static int write_input(void) {
    FILE *f = fopen(PATH, "w");
    if (!f) return -1;
    for (int i = 0; i < 3; i++) fputs(RECORDS[i], f);
    fputs("not a record\n", f);
    return fclose(f);
}

static int check_records_via_fgets(void) {
    FILE *f = fopen(PATH, "r");
    if (!f) {
        printf("scan: cannot open %s\n", PATH);
        return 2;
    }
    char line[256];
    int lines = 0;
    int parsed = 0;
    int sum_a = 0, sum_b = 0;
    double sum_c = 0.0;
    while (fgets(line, sizeof line, f)) {
        lines++;
        int a = 0, b = 0;
        double c = 0.0;
        char word[32];
        int consumed = -1;
        int n = sscanf(line, "%d %d %lf %31s%n", &a, &b, &c, word, &consumed);
        if (n == 4) {
            if (parsed >= 3 || a != EXPECTED_A[parsed] || b != EXPECTED_B[parsed] ||
                c != EXPECTED_C[parsed] || strcmp(word, EXPECTED_WORD[parsed]) != 0) {
                printf("scan: record %d mismatch: %d %d %g %s\n", parsed + 1, a, b, c, word);
                fclose(f);
                return 2;
            }
            printf("scan: record %d: %d %d %g %s (sscanf=%d, %%n=%d)\n",
                   parsed + 1, a, b, c, word, n, consumed);
            sum_a += a;
            sum_b += b;
            sum_c += c;
            parsed++;
        } else {
            printf("scan: line %d is not a record (sscanf=%d)\n", lines, n);
        }
    }
    fclose(f);
    if (lines != 4 || parsed != 3) {
        printf("scan: expected 4 lines / 3 records, got %d / %d\n", lines, parsed);
        return 2;
    }
    if (sum_a != 137 || sum_b != 201 || sum_c != 5.25) {
        printf("scan: sums %d %d %g, expected 137 201 5.25\n", sum_a, sum_b, sum_c);
        return 2;
    }
    printf("scan: fgets+sscanf ok (3 records, sums %d %d %g)\n", sum_a, sum_b, sum_c);
    return 0;
}

static int check_sprintf(void) {
    char out[128];
    int n = sprintf(out, "%d/%d/%.2f/%s", 137, 201, 5.25, "gamma");
    if (strcmp(out, "137/201/5.25/gamma") != 0 || n != (int)strlen(out)) {
        printf("scan: sprintf produced [%s] (n=%d)\n", out, n);
        return 3;
    }
    printf("scan: sprintf ok: %s (%d bytes)\n", out, n);
    return 0;
}

static int check_records_via_fscanf(void) {
    FILE *f = fopen(PATH, "r");
    if (!f) {
        printf("scan: cannot reopen %s\n", PATH);
        return 4;
    }
    int count = 0;
    for (;;) {
        int a = 0, b = 0;
        double c = 0.0;
        char word[32];
        int n = fscanf(f, "%d %d %lf %31s", &a, &b, &c, word);
        if (n == 4) {
            if (count >= 3 || a != EXPECTED_A[count] || b != EXPECTED_B[count] ||
                c != EXPECTED_C[count] || strcmp(word, EXPECTED_WORD[count]) != 0) {
                printf("scan: fscanf record %d mismatch\n", count + 1);
                fclose(f);
                return 4;
            }
            printf("scan: fscanf record %d: %d %d %g %s\n", count + 1, a, b, c, word);
            count++;
            continue;
        }
        printf("scan: fscanf stopped on the non-record line (return %d)\n", n);
        break;
    }
    fclose(f);
    if (count != 3) {
        printf("scan: fscanf read %d records, expected 3\n", count);
        return 4;
    }
    printf("scan: fscanf ok (3 records, FILE path)\n");
    return 0;
}

static int check_edges(void) {
    int value = -1, other = -1, position = -1;
    char first[32], second[32];

    if (sscanf("", "%d", &value) != EOF) {
        printf("scan: empty input must return EOF\n");
        return 5;
    }
    if (sscanf("abc", "%d", &value) != 0) {
        printf("scan: non-numeric input must be a matching failure\n");
        return 5;
    }
    if (sscanf(" 42abc", "%d%n", &value, &position) != 1 || value != 42 || position != 3) {
        printf("scan: %%d%%n on \" 42abc\" gave %d/%d/%d\n", value, position, value);
        return 5;
    }
    if (sscanf("12 34 56", "%d %*d %d", &value, &other) != 2 || value != 12 || other != 56) {
        printf("scan: suppression got %d %d\n", value, other);
        return 5;
    }
    if (sscanf("hello, world", "%[^,], %s", first, second) != 2 ||
        strcmp(first, "hello") != 0 || strcmp(second, "world") != 0) {
        printf("scan: scanset got [%s] [%s]\n", first, second);
        return 5;
    }
    if (sscanf("0x2a", "%i", &value) != 1 || value != 42) {
        printf("scan: %%i hex prefix got %d\n", value);
        return 5;
    }
    printf("scan: edge semantics ok (EOF/match failure/%%n/suppression/scanset/%%i)\n");
    return 0;
}

int main(void) {
    if (write_input() != 0) {
        printf("scan: cannot write %s\n", PATH);
        return 1;
    }
    printf("scan: wrote %s (4 lines, 3 records)\n", PATH);
    int rc = check_records_via_fgets();
    if (rc) return rc;
    rc = check_sprintf();
    if (rc) return rc;
    rc = check_records_via_fscanf();
    if (rc) return rc;
    rc = check_edges();
    if (rc) return rc;
    printf("scan: ok (fgets+sscanf, sprintf, fscanf, edge semantics)\n");
    return 0;
}