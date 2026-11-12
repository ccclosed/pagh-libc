/* Host test for getline(3)/getdelim(3) (src/getline.c).
 *
 * The implementation is intentionally built on the public stream interface
 * (fgetc + ferror), so this test can substitute a synthetic stream for the
 * FILE and drive every case from known bytes: line boundaries, a last line
 * without its delimiter, embedded NUL bytes, a custom delimiter, buffer
 * reuse and growth, end of file, and a read error in the middle of a line.
 * The host's own getline cannot be compared in-process (its FILE and this
 * libc's FILE are different types), so the expectations are written out
 * explicitly; the live QEMU run in tools/live-getline.py covers the same code
 * on a real file.
 *
 *   ./build/host_getline_test
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A complete-enough FILE for the stub stream below; src/getline.c only sees
 * the opaque `struct PaghFile` and passes a pointer back to fgetc/ferror. */
struct PaghFile {
    int unused;
};

static struct PaghFile fake_stream;
static FILE *stream = &fake_stream;

static const unsigned char *feed;
static size_t feed_len;
static size_t feed_pos;
static size_t feed_error_at; /* SIZE_MAX: no error */
static int feed_error;

int errno;

int fgetc(FILE *f) {
    (void)f;
    if (feed_error) return EOF;
    if (feed_pos == feed_error_at) {
        feed_error = 1;
        return EOF;
    }
    if (feed_pos >= feed_len) return EOF;
    return feed[feed_pos++];
}

int ferror(FILE *f) {
    (void)f;
    return feed_error;
}

static void feed_bytes(const void *data, size_t n) {
    feed = data;
    feed_len = n;
    feed_pos = 0;
    feed_error_at = (size_t)-1;
    feed_error = 0;
}

static void feed_error_after(size_t at) {
    feed_error_at = at;
}

static int checks;
static int failures;

static void ok(int condition, const char *what, const char *detail) {
    checks++;
    if (condition) return;
    failures++;
    if (failures <= 40) printf("FAIL %s: %s\n", what, detail);
}

static void test_lines(void) {
    static const char data[] = "one\ntwo\nthree";
    char *line = NULL;
    size_t cap = 0;
    feed_bytes(data, sizeof data - 1);

    ssize_t n = getline(&line, &cap, stream);
    ok(n == 4 && memcmp(line, "one\n", 4) == 0, "line 1", "length/content");
    ok(cap >= 5, "line 1", "capacity must cover the NUL");
    n = getline(&line, &cap, stream);
    ok(n == 4 && memcmp(line, "two\n", 4) == 0, "line 2", "length/content");
    n = getline(&line, &cap, stream);
    ok(n == 5 && memcmp(line, "three", 5) == 0, "last line", "no trailing newline");
    n = getline(&line, &cap, stream);
    ok(n == -1, "eof", "must return -1");
    free(line);
}

static void test_empty_file(void) {
    char *line = NULL;
    size_t cap = 0;
    feed_bytes("", 0);
    errno = 0;
    ssize_t n = getline(&line, &cap, stream);
    ok(n == -1, "empty eof", "-1 on immediate EOF");
    ok(cap == 120 && line != NULL, "empty eof", "buffer is allocated like glibc");
    ok(errno == 0, "empty eof", "EOF is not an error");
    free(line);
}

static void test_reuse_and_growth(void) {
    char *line = malloc(4);
    size_t cap = 4;
    line[0] = 'x';
    line[1] = 'y';
    line[2] = 'z';
    line[3] = 0;

    feed_bytes("a\n", 2);
    ssize_t n = getline(&line, &cap, stream);
    ok(n == 2 && strcmp(line, "a\n") == 0, "small reuse", "existing tiny buffer");
    ok(cap >= 3, "small reuse", "capacity must cover the NUL");

    /* 5000 'x' and a newline: forces several doublings. */
    static unsigned char big[5001];
    memset(big, 'x', 5000);
    big[5000] = '\n';
    feed_bytes(big, sizeof big);
    n = getline(&line, &cap, stream);
    ok(n == 5001, "growth", "length");
    ok(cap >= 5002, "growth", "capacity must cover length + NUL");
    ok(strlen(line) == 5001 && line[5000] == '\n' && line[5001] == 0, "growth",
       "content/NUL");
    free(line);
}

static void test_embedded_nul(void) {
    static const unsigned char data[] = {'a', 0, 'b', '\n'};
    char *line = NULL;
    size_t cap = 0;
    feed_bytes(data, sizeof data);
    ssize_t n = getline(&line, &cap, stream);
    ok(n == 4, "embedded NUL", "length counts the NUL byte");
    ok(memcmp(line, data, 4) == 0 && line[4] == 0, "embedded NUL", "bytes preserved");
    ok(strlen(line) == 1, "embedded NUL", "C string view stops at the NUL");
    free(line);
}

static void test_delimiter(void) {
    static const char data[] = "a;b;c";
    char *line = NULL;
    size_t cap = 0;
    feed_bytes(data, sizeof data - 1);
    ssize_t n = getdelim(&line, &cap, ';', stream);
    ok(n == 2 && strcmp(line, "a;") == 0, "getdelim 1", "delimiter stored");
    n = getdelim(&line, &cap, ';', stream);
    ok(n == 2 && strcmp(line, "b;") == 0, "getdelim 2", "delimiter stored");
    n = getdelim(&line, &cap, ';', stream);
    ok(n == 1 && strcmp(line, "c") == 0, "getdelim 3", "last token");
    n = getdelim(&line, &cap, ';', stream);
    ok(n == -1, "getdelim eof", "-1 after the last token");
    free(line);

    /* A NUL delimiter is a byte like any other. */
    static const unsigned char zero_data[] = {'a', 'b', 0, 'c', 'd'};
    line = NULL;
    cap = 0;
    feed_bytes(zero_data, sizeof zero_data);
    n = getdelim(&line, &cap, 0, stream);
    ok(n == 3 && line[0] == 'a' && line[1] == 'b' && line[2] == 0 && line[3] == 0,
       "getdelim NUL", "NUL delimiter");
    free(line);
}

static void test_errors(void) {
    char *line = NULL;
    size_t cap = 0;
    feed_bytes("", 0);

    errno = 0;
    ok(getline(NULL, &cap, stream) == -1 && errno == EINVAL, "EINVAL lineptr",
       "NULL lineptr must be EINVAL");
    errno = 0;
    ok(getline(&line, NULL, stream) == -1 && errno == EINVAL, "EINVAL n",
       "NULL n must be EINVAL");
    errno = 0;
    ok(getline(&line, &cap, NULL) == -1 && errno == EINVAL, "EINVAL stream",
       "NULL stream must be EINVAL");
    free(line);

    /* An error before the first byte is an error, not EOF. */
    char *err_line = NULL;
    size_t err_cap = 0;
    feed_bytes("", 0);
    feed_error_after(0);
    ok(getline(&err_line, &err_cap, stream) == -1, "read error", "-1 on error");
    ok(ferror(stream), "read error", "stream error flag set");
    free(err_line);

    /* ... and so is an error after a partial line: POSIX does not report a
     * half line as if it had been read. */
    static const char partial[] = "abc";
    err_line = NULL;
    err_cap = 0;
    feed_bytes(partial, sizeof partial - 1);
    feed_error_after(2);
    ok(getline(&err_line, &err_cap, stream) == -1, "mid-line error", "-1, not 3");
    ok(ferror(stream), "mid-line error", "stream error flag set");
    free(err_line);
}

static void test_no_trailing_newline_after_previous(void) {
    static const char data[] = "first\nsecond";
    char *line = NULL;
    size_t cap = 0;
    feed_bytes(data, sizeof data - 1);
    ok(getline(&line, &cap, stream) == 6 && strcmp(line, "first\n") == 0,
       "two lines 1", "content");
    ok(getline(&line, &cap, stream) == 6 && strcmp(line, "second") == 0,
       "two lines 2", "content");
    ok(getline(&line, &cap, stream) == -1, "two lines eof", "-1");
    free(line);
}

int main(void) {
    test_lines();
    test_empty_file();
    test_reuse_and_growth();
    test_embedded_nul();
    test_delimiter();
    test_errors();
    test_no_trailing_newline_after_previous();
    if (failures) {
        printf("getline: %d checks, %d FAILURES\n", checks, failures);
        return 1;
    }
    printf("getline: %d checks, 0 failures\n", checks);
    return 0;
}