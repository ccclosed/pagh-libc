/* RFC 1035 message helpers: pure buffer in, pure buffer out, no sockets. The
 * I/O lives in src/netdb.c so this half can be differentially tested on the
 * host against captured and hand-built responses (tests/dns_test.c).
 *
 * Scope, stated rather than implied: one question, one A record, name
 * compression in the answer (following pointers, bounded), no EDNS0, no
 * truncation/TCP fallback, no caching, no CNAME chasing — a CNAME answer is
 * reported as "no A record" and the caller retries the target itself or fails.
 * The name comparison is case-insensitive because DNS names are. */
#include <pagh_internal.h>
#include <string.h>
#include <stdint.h>

#define DNS_HEADER 12
#define DNS_TYPE_A 1
#define DNS_CLASS_IN 1
#define DNS_MAX_NAME 255
#define DNS_MAX_JUMPS 32

static unsigned char lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

int __pagh_dns_encode_name(const char *name, unsigned char *out, int cap) {
    if (!name || !*name) return 0;
    int n = 0;
    const char *p = name;
    /* A fully-qualified name may carry one trailing dot; it encodes the same
     * as the relative form. */
    size_t total = strlen(name);
    while (total > 1 && name[total - 1] == '.') total--;
    while ((size_t)(p - name) < total) {
        const char *dot = p;
        while ((size_t)(dot - name) < total && *dot != '.') dot++;
        int label = (int)(dot - p);
        if (label == 0 || label > 63) return 0;
        if (n + 1 + label + 1 > cap) return 0;
        out[n++] = (unsigned char)label;
        for (int i = 0; i < label; i++) out[n++] = (unsigned char)p[i];
        if ((size_t)(dot - name) >= total) break;
        p = dot + 1;
    }
    if (n + 1 > cap) return 0;
    out[n++] = 0;
    return n;
}

int __pagh_dns_build_query(const char *name, uint16_t id, unsigned char *out, int cap) {
    if (cap < DNS_HEADER + 5) return 0;
    memset(out, 0, DNS_HEADER);
    out[0] = (unsigned char)(id >> 8);
    out[1] = (unsigned char)(id & 0xff);
    out[2] = 0x01; /* recursion desired */
    out[5] = 0x01; /* QDCOUNT = 1 */
    int length = __pagh_dns_encode_name(name, out + DNS_HEADER, cap - DNS_HEADER - 4);
    if (!length) return 0;
    int n = DNS_HEADER + length;
    out[n++] = 0x00; out[n++] = DNS_TYPE_A;
    out[n++] = 0x00; out[n++] = DNS_CLASS_IN;
    return n;
}

/* Decode the (possibly compressed) name at `offset` into `out` as a lowercase
 * dotted string. Returns the offset just past the name in the *outer* walk
 * (i.e. past the first pointer when one was followed), or -1 on any malformed
 * input, over-long name or pointer loop. */
static int read_name(const unsigned char *msg, int len, int offset,
                     char *out, int cap) {
    int n = 0, jumps = 0, cursor = offset, resume = -1;
    for (;;) {
        if (cursor < 0 || cursor >= len) return -1;
        unsigned char c = msg[cursor];
        if ((c & 0xc0) == 0xc0) {
            if (cursor + 1 >= len) return -1;
            int target = ((c & 0x3f) << 8) | msg[cursor + 1];
            if (resume < 0) resume = cursor + 2;
            if (++jumps > DNS_MAX_JUMPS || target >= len) return -1;
            cursor = target;
            continue;
        }
        if (c & 0xc0) return -1; /* 0x40/0x80 are not label types */
        if (c == 0) {
            if (resume < 0) resume = cursor + 1;
            if (n >= cap) return -1;
            out[n] = 0;
            return resume;
        }
        if (cursor + 1 + c > len) return -1;
        if (n + (int)c + 2 > cap || n + (int)c + 1 > DNS_MAX_NAME) return -1;
        if (n) out[n++] = '.';
        for (int i = 0; i < c; i++) out[n++] = (char)lower(msg[cursor + 1 + i]);
        cursor += 1 + c;
    }
}

static int read_u16(const unsigned char *p) {
    return (p[0] << 8) | p[1];
}

int __pagh_dns_parse_a(const unsigned char *msg, int len, uint16_t id,
                       const char *name, unsigned char addr[4], int *rcode) {
    if (rcode) *rcode = 0;
    if (!msg || len < DNS_HEADER) return PAGH_DNS_BAD;
    if (read_u16(msg) != (int)id) return PAGH_DNS_BAD;
    int flags = read_u16(msg + 2);
    if (!(flags & 0x8000)) return PAGH_DNS_BAD;  /* not a response */
    if ((flags >> 11) & 0x0f) return PAGH_DNS_BAD; /* opcode != QUERY */
    if (flags & 0x0200) return PAGH_DNS_BAD;     /* truncated: no TCP fallback */
    int code = flags & 0x0f;
    if (rcode) *rcode = code;

    int questions = read_u16(msg + 4);
    int answers = read_u16(msg + 6);
    if (questions < 1) return PAGH_DNS_BAD;

    char qname[DNS_MAX_NAME + 1];
    int cursor = read_name(msg, len, DNS_HEADER, qname, (int)sizeof qname);
    if (cursor < 0 || cursor + 4 > len) return PAGH_DNS_BAD;
    int qtype = read_u16(msg + cursor);
    cursor += 4; /* QTYPE + QCLASS */
    if (qtype != DNS_TYPE_A) return PAGH_DNS_NO_ANSWER;
    if (code != 0) return PAGH_DNS_NAME_ERROR;

    /* The caller's spelling may differ in case or in one trailing dot; compare
     * normalised, lowercase, dot-free-of-trailing-dot forms. */
    char wanted[DNS_MAX_NAME + 1];
    int w = 0;
    for (const char *p = name; p && *p && w < DNS_MAX_NAME; p++) wanted[w++] = (char)lower((unsigned char)*p);
    while (w > 1 && wanted[w - 1] == '.') w--;
    wanted[w] = 0;
    if (strcmp(wanted, qname) != 0) return PAGH_DNS_BAD; /* the server echoed another question */

    for (int i = 0; i < answers; i++) {
        char aname[DNS_MAX_NAME + 1];
        cursor = read_name(msg, len, cursor, aname, (int)sizeof aname);
        if (cursor < 0 || cursor + 10 > len) return PAGH_DNS_BAD;
        int type = read_u16(msg + cursor);
        int klass = read_u16(msg + cursor + 2);
        int rdlength = read_u16(msg + cursor + 8);
        cursor += 10;
        if (cursor + rdlength > len) return PAGH_DNS_BAD;
        if (type == DNS_TYPE_A && klass == DNS_CLASS_IN && rdlength == 4 &&
            strcmp(aname, wanted) == 0) {
            memcpy(addr, msg + cursor, 4);
            return PAGH_DNS_OK;
        }
        cursor += rdlength;
    }
    return PAGH_DNS_NO_ANSWER;
}
