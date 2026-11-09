/* Host test for the pure HTTP framing used by https_get (src/http.c).
 *
 * The cases are the ones a proxy or a hostile server would use to make a client
 * believe the wrong bytes: a second `Content-Length` that disagrees, a
 * `Transfer-Encoding` this client cannot decode, a chunked body whose terminal
 * chunk never arrives, a size line split across reads. Each is checked to be
 * refused, not guessed at. */
#include <pagh_internal.h>
#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void ok(int condition, const char *what) {
    checks++;
    if (condition) return;
    failures++;
    printf("FAIL %s\n", what);
}

static long head(const char *text, struct pagh_http_head *out) {
    return __pagh_http_parse_head(text, strlen(text), out);
}

static void head_cases(void) {
    struct pagh_http_head h;

    ok(head("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n", &h) == 1, "minimal head parses");
    ok(h.status == 200, "status 200");
    ok(h.framing == PAGH_HTTP_LENGTH, "content-length framing");
    ok(h.length == 5, "content-length value");
    ok(h.body_off == 38, "body offset");

    ok(head("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n", &h) == 0, "head without its end is incomplete");
    ok(head("HTTP/1.1 200 OK\r\n\r\n", &h) == 1, "head without headers parses");
    ok(h.framing == PAGH_HTTP_UNTIL_CLOSE, "no framing header -> until close");

    ok(head("HTTP/1.1 200 OK\r\n\r\nhello", &h) == 1, "body after the head is ignored");
    ok(h.body_off == 19, "body offset without headers");

    ok(head("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n", &h) == 1,
       "both framing headers parse");
    ok(h.framing == PAGH_HTTP_CHUNKED, "chunked wins over content-length");

    ok(head("HTTP/1.1 200 OK\r\nTransfer-Encoding: identity\r\n\r\n", &h) == -1,
       "an undecodable transfer coding is refused");
    ok(head("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n", &h) == 1,
       "a repeated, agreeing content-length parses");
    ok(head("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", &h) == -1,
       "a repeated, disagreeing content-length is refused");
    ok(head("HTTP/1.1 200 OK\r\nContent-Length: 5x\r\n\r\n", &h) == -1, "non-digit length");
    ok(head("HTTP/1.1 200 OK\r\nContent-Length: \r\n\r\n", &h) == -1, "empty length");
    ok(head("HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999\r\n\r\n", &h) == -1,
       "overflowing length");

    ok(head("HTTP/1.0 404 Not Found\r\n\r\n", &h) == 1, "HTTP/1.0 status line");
    ok(h.status == 404, "status 404");
    ok(head("HTTP/2 200 OK\r\n\r\n", &h) == -1, "HTTP/2 is not HTTP/1.x");
    ok(head("HTTP/1.1 20 OK\r\n\r\n", &h) == -1, "short status code");
    ok(head("HTTP/1.1 200OK\r\n\r\n", &h) == -1, "missing status separator");
    ok(head("\r\n\r\n", &h) == -1, "empty head");
    ok(head("GET / HTTP/1.1\r\n\r\n", &h) == -1, "a request is not a response");
    ok(head("HTTP/1.1 200 OK\r\nBadHeader\r\n\r\n", &h) == -1, "a header without a colon");
    ok(head("HTTP/1.1 200 OK\r\nX: a\r\n b\r\n\r\n", &h) == -1, "a folded header line");
    ok(head("HTTP/1.1 200 OK\r\nBad Name: a\r\n\r\n", &h) == -1, "whitespace inside a field name");
    ok(head("HTTP/1.1 200 OK\r\nX: a\r\nY: b\r\n\r\n", &h) == 1, "unknown headers are skipped");
    ok(head("HTTP/1.1 200 OK\n\n", &h) == 0, "a bare LF does not end a head");
    ok(head("HTTP/1.1 200 OK\r\nhost: example.com\r\n\r\n", &h) == 1, "a non-framing header");

    ok(head("HTTP/1.1 200 OK\r\nCONTENT-LENGTH: 7\r\n\r\n", &h) == 1, "header name case");
    ok(h.length == 7, "uppercase content-length value");
    ok(head("HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\n", &h) == 1, "value case");
    ok(h.framing == PAGH_HTTP_CHUNKED, "mixed-case chunked");
    ok(head("HTTP/1.1 200 OK\r\nContent-Length:  12  \r\n\r\n", &h) == 1, "value whitespace");
    ok(h.length == 12, "trimmed length");
    ok(head("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", &h) == 1, "zero length");
    ok(h.length == 0, "zero-length value");
}

/* Decode `text` feeding `step` bytes at a time; returns the feed's verdict and
 * the decoded body in `out`. */
static int decode(const char *text, size_t step, char *out, size_t cap, size_t *out_len) {
    struct pagh_http_chunk state;
    __pagh_http_chunk_init(&state);
    *out_len = 0;
    size_t len = strlen(text);
    for (size_t at = 0; at < len; at += step) {
        size_t run = len - at < step ? len - at : step;
        int rc = __pagh_http_chunk_feed(&state, text + at, run, out, cap, out_len);
        if (rc != 0) return rc;
    }
    return 0;
}

static void chunk_cases(void) {
    static const char BODY[] = "5\r\nhello\r\n0\r\n\r\n";
    char out[64];
    size_t used;

    ok(decode(BODY, sizeof BODY - 1, out, sizeof out, &used) == 1, "one-byte-at-a-time chunk");
    ok(used == 5 && memcmp(out, "hello", 5) == 0, "one-byte-at-a-time body");

    /* Every read boundary must decode the same body. */
    int all = 1;
    for (size_t step = 1; step <= strlen(BODY); step++) {
        if (decode(BODY, step, out, sizeof out, &used) != 1 || used != 5 ||
            memcmp(out, "hello", 5) != 0)
            all = 0;
    }
    ok(all, "every split of the stream decodes to the same body");

    static const char TWO[] = "3\r\nabc\r\n4\r\ndefg\r\n0\r\n\r\n";
    ok(decode(TWO, 1, out, sizeof out, &used) == 1 && used == 7 && memcmp(out, "abcdefg", 7) == 0,
       "two chunks");

    static const char EXT[] = "5;name=value\r\nhello\r\n0\r\n\r\n";
    ok(decode(EXT, 1, out, sizeof out, &used) == 1 && used == 5 && memcmp(out, "hello", 5) == 0,
       "chunk extension skipped");

    static const char UPPER[] = "A\r\n0123456789\r\n0\r\n\r\n";
    ok(decode(UPPER, 3, out, sizeof out, &used) == 1 && used == 10 &&
           memcmp(out, "0123456789", 10) == 0,
       "uppercase hex size");

    static const char TRAILER[] = "5\r\nhello\r\n0\r\nX-Trailer: 1\r\n\r\n";
    ok(decode(TRAILER, 1, out, sizeof out, &used) == 1 && used == 5, "a trailer after the terminal chunk");

    static const char EMPTY[] = "0\r\n\r\n";
    ok(decode(EMPTY, 1, out, sizeof out, &used) == 1 && used == 0, "an empty body");

    /* Refusals. */
    ok(decode("zz\r\nhello\r\n0\r\n\r\n", 8, out, sizeof out, &used) == -1, "non-hex chunk size");
    ok(decode("\r\nhello\r\n0\r\n\r\n", 8, out, sizeof out, &used) == -1, "empty chunk size");
    ok(decode("5\r\nhelloXX0\r\n\r\n", 16, out, sizeof out, &used) == -1, "no CRLF after the data");
    ok(decode("5\r\nhello\r\n", 16, out, sizeof out, &used) == 0, "no terminal chunk yet");
    ok(decode("5\r\nhel", 16, out, sizeof out, &used) == 0, "a chunk cut short");
    ok(decode("5 hello\r\nx\r\n0\r\n\r\n", 32, out, sizeof out, &used) == -1, "whitespace in the size line");
    ok(decode("5\r\nhello\r\n0\r\n\r\n", 1, out, 3, &used) == -2, "a body larger than the buffer");

    struct pagh_http_chunk state;
    __pagh_http_chunk_init(&state);
    used = 0;
    ok(__pagh_http_chunk_feed(&state, "5\r\nhello\r\n0\r\n\r\n", 15, out, 5, &used) == 1 && used == 5,
       "a body that exactly fills the buffer");
}

int main(void) {
    head_cases();
    chunk_cases();
    if (failures) {
        printf("http_test: %d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("http_test: %d checks passed\n", checks);
    return 0;
}
