/* Differential tests for the pure libc helpers added with the getopt work:
 * bsearch, the reentrant tokenizers (strtok_r/strsep), the span helpers they
 * use (strspn/strcspn) and memchr.
 *
 * src/string.c and src/stdlib.c are compiled with their public symbols renamed
 * (tools/host-tests.sh), so the host libc's versions stay available and every
 * input is run through both implementations. The tokenizers are compared by
 * token text *and* by the offset of the saved cursor inside the buffer, so a
 * different-but-plausible tokenization cannot pass by coincidence.
 *
 *   ./build/host_libc_extras_test
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Renamed pagh implementations (compiled from src/string.c and src/stdlib.c). */
extern void *pagh_memchr(const void *s, int c, size_t n);
extern size_t pagh_strspn(const char *s, const char *accept);
extern size_t pagh_strcspn(const char *s, const char *reject);
extern char *pagh_strtok_r(char *s, const char *delim, char **saveptr);
extern char *pagh_strsep(char **stringp, const char *delim);
extern void *pagh_bsearch(const void *key, const void *base, size_t n, size_t size,
                          int (*cmp)(const void *, const void *));

/* stdlib.c's abort() calls _exit; renamed so it cannot end this test even if a
 * bug made it reachable. */
void pagh_host_exit(int code) {
    (void)code;
    for (;;) {}
}

static int checks;
static int failures;

static void ok(int condition, const char *what, const char *detail) {
    checks++;
    if (condition) return;
    failures++;
    if (failures <= 40) printf("FAIL %s: %s\n", what, detail);
}

static unsigned long rng_state = 0x243f6a8885a308d3ULL;

static unsigned long rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (unsigned long)rng_state;
}

/* ---- bsearch ------------------------------------------------------------ */

static int cmp_int(const void *key, const void *element) {
    int a = *(const int *)key;
    int b = *(const int *)element;
    return (a > b) - (a < b);
}

struct record {
    int key;
    int payload;
};

static int cmp_record(const void *key, const void *element) {
    int a = *(const int *)key;
    int b = ((const struct record *)element)->key;
    return (a > b) - (a < b);
}

static void test_bsearch(void) {
    for (int round = 0; round < 200; round++) {
        int n = (int)(rng() % 40);
        int array[40];
        for (int i = 0; i < n; i++) array[i] = (int)(rng() % 200) - 100;
        /* Sort with the host qsort; both implementations then search the same
         * sorted array. */
        qsort(array, (size_t)n, sizeof array[0], cmp_int);
        for (int probe = 0; probe < 30; probe++) {
            int key = (int)(rng() % 240) - 120;
            void *p = pagh_bsearch(&key, array, (size_t)n, sizeof array[0], cmp_int);
            void *h = bsearch(&key, array, (size_t)n, sizeof array[0], cmp_int);
            int pi = p ? (int)((int *)p - array) : -1;
            int hi = h ? (int)((int *)h - array) : -1;
            ok(pi == hi, "bsearch int", "index mismatch");
        }
    }

    struct record records[32];
    for (int i = 0; i < 32; i++) {
        records[i].key = i * 3;
        records[i].payload = 1000 + i;
    }
    for (int key = -3; key < 100; key++) {
        struct record *p = pagh_bsearch(&key, records, 32, sizeof records[0], cmp_record);
        struct record *h = bsearch(&key, records, 32, sizeof records[0], cmp_record);
        int pi = p ? (int)(p - records) : -1;
        int hi = h ? (int)(h - records) : -1;
        ok(pi == hi, "bsearch struct", "key projection mismatch");
        if (pi >= 0) ok(p->key == key, "bsearch struct", "wrong element");
    }
    ok(pagh_bsearch(&(int){0}, records, 0, sizeof records[0], cmp_record) == NULL,
       "bsearch empty", "must return NULL");
}

/* ---- strtok_r / strsep -------------------------------------------------- */

static char *token_buffer(const char *s) {
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    memcpy(copy, s, n);
    return copy;
}

static void test_tokenizers(void) {
    static const char *const strings[] = {
        "", " ", "a", " a ", "a,b,c", ",,a,,b,", "a::b::c", "  leading",
        "trailing ", "only,delims,", "abracadabra", "a b\tc\nd", "::://",
    };
    static const char *const delims[] = {
        ",", ":", " ,\t\n", "", ",:", "ab", " ",
    };
    for (size_t si = 0; si < sizeof strings / sizeof strings[0]; si++) {
        for (size_t di = 0; di < sizeof delims / sizeof delims[0]; di++) {
            const char *s = strings[si];
            const char *d = delims[di];

            /* strtok_r lockstep, comparing token text and cursor offset. */
            char *pagh_buf = token_buffer(s);
            char *host_buf = token_buffer(s);
            char *pagh_save = NULL;
            char *host_save = NULL;
            for (int step = 0; step < 32; step++) {
                char *pt = pagh_strtok_r(step == 0 ? pagh_buf : NULL, d, &pagh_save);
                char *ht = strtok_r(step == 0 ? host_buf : NULL, d, &host_save);
                ok((pt == NULL) == (ht == NULL), "strtok_r", "NULL mismatch");
                if (!pt || !ht) break;
                ok(strcmp(pt, ht) == 0, "strtok_r", "token text mismatch");
                ok(pagh_save - pagh_buf == host_save - host_buf, "strtok_r",
                   "cursor offset mismatch");
            }
            /* Both cursors must agree at the end as well. */
            ok((pagh_save == NULL) == (host_save == NULL), "strtok_r end", "cursor NULL mismatch");
            free(pagh_buf);
            free(host_buf);

            /* strsep lockstep: adjacent delimiters give empty tokens, and the
             * cursor becomes NULL at the final token. */
            pagh_buf = token_buffer(s);
            host_buf = token_buffer(s);
            char *pagh_cursor = pagh_buf;
            char *host_cursor = host_buf;
            for (int step = 0; step < 32; step++) {
                char *pt = pagh_strsep(&pagh_cursor, d);
                char *ht = strsep(&host_cursor, d);
                ok((pt == NULL) == (ht == NULL), "strsep", "NULL mismatch");
                if (!pt || !ht) break;
                ok(strcmp(pt, ht) == 0, "strsep", "token text mismatch");
                ok((pagh_cursor == NULL) == (host_cursor == NULL), "strsep",
                   "cursor NULL mismatch");
                if (!pagh_cursor || !host_cursor) break;
                ok(pagh_cursor - pagh_buf == host_cursor - host_buf, "strsep",
                   "cursor offset mismatch");
            }
            free(pagh_buf);
            free(host_buf);
        }
    }

    /* A NULL start pointer is a no-op for strsep. */
    char *null_cursor = NULL;
    ok(pagh_strsep(&null_cursor, ",") == NULL && null_cursor == NULL,
       "strsep NULL start", "must stay NULL");
}

/* ---- spans and memchr --------------------------------------------------- */

static void test_spans_and_memchr(void) {
    static const char *const strings[] = {
        "", "a", "abc", "aaabbbccc", "hello, world", " \t\n", "abcdef",
    };
    static const char *const sets[] = {"abc", "", "xyz", " ", ",;", "fedcba"};
    for (size_t si = 0; si < sizeof strings / sizeof strings[0]; si++) {
        for (size_t di = 0; di < sizeof sets / sizeof sets[0]; di++) {
            size_t p = pagh_strspn(strings[si], sets[di]);
            size_t h = strspn(strings[si], sets[di]);
            ok(p == h, "strspn", "length mismatch");
            p = pagh_strcspn(strings[si], sets[di]);
            h = strcspn(strings[si], sets[di]);
            ok(p == h, "strcspn", "length mismatch");
        }
    }

    unsigned char bytes[64];
    for (int round = 0; round < 100; round++) {
        size_t n = rng() % sizeof bytes;
        for (size_t i = 0; i < n; i++) bytes[i] = (unsigned char)rng();
        for (int probe = 0; probe < 20; probe++) {
            int c = (int)(rng() % 260) - 2;
            size_t len = rng() % (n + 1);
            void *p = pagh_memchr(bytes, c, len);
            void *h = memchr(bytes, c, len);
            ok((p == NULL) == (h == NULL), "memchr", "NULL mismatch");
            if (p && h) ok((unsigned char *)p - bytes == (unsigned char *)h - bytes,
                           "memchr", "index mismatch");
        }
    }
    ok(pagh_memchr("hello", 'l', 2) == NULL, "memchr bound", "found past n");
}

int main(void) {
    test_bsearch();
    test_tokenizers();
    test_spans_and_memchr();
    if (failures) {
        printf("libc extras: %d checks, %d FAILURES\n", checks, failures);
        return 1;
    }
    printf("libc extras: %d checks, 0 failures\n", checks);
    return 0;
}