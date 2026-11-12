/* Live check for getline(3) and getdelim(3) on a real kernel filesystem.
 *
 * The host test (tests/getline_test.c) drives the code from a synthetic
 * stream; this program proves the same calls against a file on the ext2 image
 * mounted at /mnt: it writes a file with a short line, a line with a space, an
 * empty line, a 300-byte line (forcing the buffer to grow past its initial
 * 120 bytes) and a last line without a newline, then reads it back with
 * getline and compares every length and byte. getdelim is checked with ';' as
 * the delimiter. It prints one marker per step and ends with "getline: ok";
 * the exit code is non-zero at the first failure.
 *
 * Run it in the guest shell:
 *
 *   run /mnt/getline
 *
 * Exit codes: 0 success, 1 write, 2 getline, 3 EOF handling, 4 getdelim. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH "/mnt/getline.txt"
#define DELIM_PATH "/mnt/getdelim.txt"

static int write_fixture(void) {
    FILE *f = fopen(PATH, "w");
    if (!f) return -1;
    fputs("alpha\n", f);
    fputs("beta gamma\n", f);
    fputs("\n", f);
    for (int i = 0; i < 300; i++) fputc('x', f);
    fputc('\n', f);
    fputs("last-without-newline", f);
    if (fclose(f) != 0) return -1;
    return 0;
}

int main(void) {
    if (write_fixture() != 0) {
        printf("getline: cannot write %s\n", PATH);
        return 1;
    }
    printf("getline: wrote %s (339 bytes)\n", PATH);

    FILE *f = fopen(PATH, "r");
    if (!f) {
        printf("getline: cannot open %s\n", PATH);
        return 1;
    }

    static const size_t want[] = { 6, 11, 1, 301, 20 };
    static const int want_count = (int)(sizeof want / sizeof want[0]);
    char *line = 0;
    size_t cap = 0;
    int count = 0;
    long n;
    while ((n = getline(&line, &cap, f)) != -1) {
        count++;
        if (count > want_count || (size_t)n != want[count - 1]) {
            printf("getline: line %d has %ld bytes, expected %d\n",
                   count, n, count <= want_count ? (int)want[count - 1] : -1);
            free(line);
            fclose(f);
            return 2;
        }
        printf("getline: line %d: %ld bytes\n", count, n);
        if (count == 1 && strcmp(line, "alpha\n") != 0) return 2;
        if (count == 2 && strcmp(line, "beta gamma\n") != 0) return 2;
        if (count == 3 && strcmp(line, "\n") != 0) return 2;
        if (count == 3) printf("getline: empty line was stored as \"\\n\"\n");
        if (count == 4) {
            int all_x = 1;
            for (int i = 0; i < 300; i++) if (line[i] != 'x') all_x = 0;
            if (!all_x || line[300] != '\n' || line[301] != 0) {
                printf("getline: 300-byte line content mismatch\n");
                return 2;
            }
            printf("getline: 300-byte line grew the buffer to %lu bytes\n",
                   (unsigned long)cap);
        }
        if (count == 5 && strcmp(line, "last-without-newline") != 0) return 2;
    }
    if (count != want_count) {
        printf("getline: read %d lines, expected %d\n", count, want_count);
        free(line);
        fclose(f);
        return 2;
    }
    if (!feof(f)) {
        printf("getline: EOF flag not set after the last line\n");
        free(line);
        fclose(f);
        return 3;
    }
    free(line);
    fclose(f);
    printf("getline: eof after %d lines (feof set)\n", count);

    /* getdelim with a non-newline delimiter. */
    f = fopen(DELIM_PATH, "w");
    if (!f) {
        printf("getline: cannot write %s\n", DELIM_PATH);
        return 4;
    }
    fputs("one;two;three", f);
    fclose(f);
    f = fopen(DELIM_PATH, "r");
    if (!f) {
        printf("getline: cannot open %s\n", DELIM_PATH);
        return 4;
    }
    line = 0;
    cap = 0;
    static const char *const tokens[] = { "one;", "two;", "three" };
    int token = 0;
    while ((n = getdelim(&line, &cap, ';', f)) != -1) {
        if (token >= 3 || strcmp(line, tokens[token]) != 0) {
            printf("getline: getdelim token %d mismatch\n", token + 1);
            free(line);
            fclose(f);
            return 4;
        }
        printf("getdelim: token %d: %ld bytes [%s]\n", token + 1, n, line);
        token++;
    }
    free(line);
    fclose(f);
    if (token != 3) {
        printf("getline: getdelim returned %d tokens, expected 3\n", token);
        return 4;
    }

    printf("getline: ok (5 getline lines, 3 getdelim tokens)\n");
    return 0;
}
