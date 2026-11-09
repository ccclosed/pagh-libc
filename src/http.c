/* Pure HTTP/1.1 response framing for https_get (src/tls.c): the head parser and
 * the `Transfer-Encoding: chunked` decoder.
 *
 * No sockets and no syscalls here, so tests/http_test.c runs exactly the code
 * the guest runs — the same split src/dns.c uses for the DNS wire format. The
 * security-relevant decisions (which framing wins, whether a length is
 * believable, whether a truncation is detectable) are therefore testable on the
 * host instead of only in a live transcript.
 *
 * Deliberately strict, because every leniency in HTTP framing is a smuggling
 * vector in somebody's proxy:
 *   * a head must end with CRLF CRLF; a bare LF is not a line terminator;
 *   * `Content-Length` must be all digits and, if repeated, must agree with
 *     itself — `Content-Length: 5` twice with different values is malformed;
 *   * a `Transfer-Encoding` this code cannot decode is malformed rather than
 *     silently treated as close-delimited;
 *   * `Transfer-Encoding: chunked` wins over `Content-Length` (RFC 9112 §6.3);
 *   * chunk-size lines carry hex digits and an optional `;extension` and
 *     nothing else — no whitespace, no sign. */
#include <stddef.h>
#include <string.h>
#include <pagh_internal.h>

static int ascii_lower(int byte) {
    return byte >= 'A' && byte <= 'Z' ? byte + 32 : byte;
}

/* `text[0..len)` equals `word` (which is NUL-terminated), ASCII-insensitively. */
static int eq_ignore_case(const char *text, size_t len, const char *word) {
    size_t i = 0;
    while (i < len && word[i]) {
        if (ascii_lower((unsigned char)text[i]) != word[i]) return 0;
        i++;
    }
    return i == len && word[i] == 0;
}

/* `word` occurs in `text[0..len)`, ASCII-insensitively. */
static int contains_ignore_case(const char *text, size_t len, const char *word) {
    size_t n = strlen(word);
    if (n == 0 || len < n) return 0;
    for (size_t start = 0; start + n <= len; start++) {
        size_t i = 0;
        while (i < n && ascii_lower((unsigned char)text[start + i]) == word[i]) i++;
        if (i == n) return 1;
    }
    return 0;
}

/* All-digits decimal, with an overflow check. Empty input is not a number. */
static int parse_decimal(const char *text, size_t len, unsigned long *out) {
    if (len == 0 || len > 20) return 0;
    unsigned long value = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] < '0' || text[i] > '9') return 0;
        unsigned long digit = (unsigned long)(text[i] - '0');
        if (value > (~0UL - digit) / 10UL) return 0;
        value = value * 10 + digit;
    }
    *out = value;
    return 1;
}

/* Hex digits, at most 16 of them. Empty input is not a size. */
static int parse_hex(const char *text, size_t len, unsigned long *out) {
    if (len == 0 || len > 16) return 0;
    unsigned long value = 0;
    for (size_t i = 0; i < len; i++) {
        char byte = text[i];
        unsigned long digit;
        if (byte >= '0' && byte <= '9') digit = (unsigned long)(byte - '0');
        else if (byte >= 'a' && byte <= 'f') digit = (unsigned long)(byte - 'a' + 10);
        else if (byte >= 'A' && byte <= 'F') digit = (unsigned long)(byte - 'A' + 10);
        else return 0;
        value = (value << 4) | digit;
    }
    *out = value;
    return 1;
}

int __pagh_http_parse_head(const char *data, size_t len, struct pagh_http_head *head) {
    size_t end = 0;
    int complete = 0;
    for (size_t i = 0; i + 4 <= len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n') {
            end = i;
            complete = 1;
            break;
        }
    }
    if (!complete) return 0; /* the head is still growing */

    int status = -1;
    unsigned long content_length = 0;
    int have_length = 0;
    int chunked = 0;

    size_t pos = 0;
    int first = 1;
    while (pos < end) {
        size_t cr = pos;
        while (cr + 1 < len && !(data[cr] == '\r' && data[cr + 1] == '\n')) cr++;
        if (cr + 1 >= len) return -1; /* no CRLF: unreachable inside a parsed head */
        const char *line = data + pos;
        size_t line_len = cr - pos;

        if (first) {
            /* `HTTP/1.x SP status SP reason`; the reason phrase is free-form. */
            first = 0;
            if (line_len < 12) return -1;
            if (memcmp(line, "HTTP/1.", 7) != 0) return -1;
            if (line[7] != '0' && line[7] != '1') return -1;
            if (line[8] != ' ') return -1;
            for (int i = 9; i <= 11; i++)
                if (line[i] < '0' || line[i] > '9') return -1;
            /* `SP reason-phrase`: the reason may be empty, the space may not. */
            if (line_len > 12 && line[12] != ' ') return -1;
            status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
        } else {
            size_t colon = 0;
            while (colon < line_len && line[colon] != ':') colon++;
            if (colon == 0 || colon == line_len) return -1;
            const char *name = line;
            size_t name_len = colon;
            const char *value = line + colon + 1;
            size_t value_len = line_len - colon - 1;
            /* No whitespace in the field name (RFC 9112 §5.1), which also
             * rejects the obsolete folded-line continuation. */
            for (size_t i = 0; i < name_len; i++)
                if (name[i] == ' ' || name[i] == '\t') return -1;
            while (value_len && (*value == ' ' || *value == '\t')) {
                value++;
                value_len--;
            }
            while (value_len && (value[value_len - 1] == ' ' || value[value_len - 1] == '\t'))
                value_len--;

            if (eq_ignore_case(name, name_len, "content-length")) {
                unsigned long number;
                if (!parse_decimal(value, value_len, &number)) return -1;
                if (have_length && content_length != number) return -1;
                content_length = number;
                have_length = 1;
            } else if (eq_ignore_case(name, name_len, "transfer-encoding")) {
                /* Only `chunked` is decodable here; `identity`, a coding chain
                 * or a typo is refused instead of guessed at. */
                if (!contains_ignore_case(value, value_len, "chunked")) return -1;
                chunked = 1;
            }
        }
        pos = cr + 2;
    }
    if (first) return -1; /* an empty head: no status line at all */
    if (status < 0) return -1;

    head->status = status;
    head->body_off = end + 4;
    if (chunked) {
        head->framing = PAGH_HTTP_CHUNKED;
        head->length = 0;
    } else if (have_length) {
        head->framing = PAGH_HTTP_LENGTH;
        head->length = content_length;
    } else {
        head->framing = PAGH_HTTP_UNTIL_CLOSE;
        head->length = 0;
    }
    return 1;
}

void __pagh_http_chunk_init(struct pagh_http_chunk *state) {
    state->state = PAGH_CHUNK_SIZE;
    state->remaining = 0;
    state->line_len = 0;
    state->crlf = 0;
}

int __pagh_http_chunk_feed(struct pagh_http_chunk *state, const char *data, size_t len,
                           char *out, size_t cap, size_t *out_len) {
    size_t at = 0;
    while (at < len) {
        if (state->state == PAGH_CHUNK_SIZE) {
            while (at < len && data[at] != '\n') {
                if (state->line_len == sizeof state->line) return -1;
                state->line[state->line_len++] = data[at++];
            }
            if (at == len) return 0; /* the size line is still growing */
            at++;                    /* the LF */
            size_t line_len = state->line_len;
            if (line_len && state->line[line_len - 1] == '\r') line_len--;
            size_t digits = 0;
            while (digits < line_len && state->line[digits] != ';') digits++;
            unsigned long size;
            if (!parse_hex(state->line, digits, &size)) return -1;
            state->line_len = 0;
            if (size == 0) return 1; /* the terminal chunk: the body is complete */
            state->remaining = size;
            state->state = PAGH_CHUNK_DATA;
        } else if (state->state == PAGH_CHUNK_DATA) {
            size_t take = len - at;
            if (take > (size_t)state->remaining) take = (size_t)state->remaining;
            /* *out_len <= cap is the caller's invariant; it only ever grows by
             * `take` below, and only after this check. */
            if (take > cap - *out_len) return -2;
            memcpy(out + *out_len, data + at, take);
            *out_len += take;
            at += take;
            state->remaining -= take;
            if (state->remaining == 0) {
                state->state = PAGH_CHUNK_CRLF;
                state->crlf = 0;
            }
        } else { /* PAGH_CHUNK_CRLF: the CRLF that closes a chunk's data */
            while (at < len && state->crlf < 2) {
                char want = state->crlf == 0 ? '\r' : '\n';
                if (data[at++] != want) return -1;
                state->crlf++;
            }
            if (state->crlf == 2) {
                state->crlf = 0;
                state->state = PAGH_CHUNK_SIZE;
            }
        }
    }
    return 0;
}
