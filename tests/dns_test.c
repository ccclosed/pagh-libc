/* Host test for the pure DNS half (src/dns.c). The captured response is a real
 * answer for example.com, recorded from a system resolver; the malformed cases
 * are built by hand so the parser's refusals are checked, not assumed. */
#include <pagh_internal.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int failures;
static int checks;

static void ok(int condition, const char *what) {
    checks++;
    if (condition) return;
    failures++;
    printf("FAIL %s\n", what);
}

static int from_hex(const char *hex, unsigned char *out, int cap) {
    int n = 0;
    while (hex[0] && hex[1]) {
        int hi = hex[0] <= '9' ? hex[0] - '0' : (hex[0] | 32) - 'a' + 10;
        int lo = hex[1] <= '9' ? hex[1] - '0' : (hex[1] | 32) - 'a' + 10;
        if (n >= cap) return -1;
        out[n++] = (unsigned char)((hi << 4) | lo);
        hex += 2;
    }
    return n;
}

/* A real A answer for example.com: id 1234, flags 8180, one question, two
 * compressed A records (8.6.112.0 and 8.47.69.0). */
static const char *REAL = "123481800001000200000000076578616d706c6503636f6d0000010001"
                          "c00c000100010000009f000408067000"
                          "c00c000100010000009f0004082f4500";

static void query_cases(void) {
    unsigned char out[512];
    int n = __pagh_dns_build_query("example.com", 0x1234, out, sizeof out);
    ok(n == 12 + 13 + 4, "query length");
    static const unsigned char expected[] = {
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x03, 'c', 'o', 'm', 0x00,
        0x00, 0x01, 0x00, 0x01,
    };
    ok(n == (int)sizeof expected && memcmp(out, expected, sizeof expected) == 0, "query bytes");
    ok(__pagh_dns_build_query("", 1, out, sizeof out) == 0, "empty name refused");
    ok(__pagh_dns_build_query("example..com", 1, out, sizeof out) == 0, "empty label refused");
    /* A 64-byte label is over the limit. */
    char long_label[300];
    memset(long_label, 'a', 64);
    strcpy(long_label + 64, ".com");
    ok(__pagh_dns_build_query(long_label, 1, out, sizeof out) == 0, "long label refused");
    /* Trailing dot encodes the same bytes (fresh reference buffer: the refused
     * calls above are allowed to have scribbled on `out`). */
    unsigned char reference[512], out2[512];
    int n1 = __pagh_dns_build_query("example.com", 0x1234, reference, sizeof reference);
    int n2 = __pagh_dns_build_query("example.com.", 0x1234, out2, sizeof out2);
    ok(n2 == n1 && memcmp(reference, out2, (size_t)n1) == 0, "trailing dot");
}

static void parse_cases(void) {
    unsigned char msg[512];
    int len = from_hex(REAL, msg, sizeof msg);
    ok(len == 61, "captured response length");
    unsigned char addr[4];
    int rcode = -1;
    ok(__pagh_dns_parse_a(msg, len, 0x1234, "example.com", addr, &rcode) == PAGH_DNS_OK,
       "captured response parses");
    ok(addr[0] == 8 && addr[1] == 6 && addr[2] == 112 && addr[3] == 0,
       "first A record is 8.6.112.0");
    ok(__pagh_dns_parse_a(msg, len, 0x1234, "EXAMPLE.COM", addr, &rcode) == PAGH_DNS_OK,
       "name comparison is case-insensitive");
    ok(__pagh_dns_parse_a(msg, len, 0x1234, "example.com.", addr, &rcode) == PAGH_DNS_OK,
       "trailing dot accepted");
    ok(__pagh_dns_parse_a(msg, len, 0x1235, "example.com", addr, &rcode) == PAGH_DNS_BAD,
       "wrong transaction id refused");
    ok(__pagh_dns_parse_a(msg, len, 0x1234, "example.net", addr, &rcode) == PAGH_DNS_BAD,
       "wrong question refused");
    ok(__pagh_dns_parse_a(msg, len - 1, 0x1234, "example.com", addr, &rcode) == PAGH_DNS_OK,
       "a cut in the trailing record still answers");
    ok(__pagh_dns_parse_a(msg, 41, 0x1234, "example.com", addr, &rcode) == PAGH_DNS_BAD,
       "a cut inside the first record is refused");
    {   /* rdlength past the end of the message. */
        unsigned char over[512];
        memcpy(over, msg, (size_t)len);
        over[7] = 0x01;
        over[39] = 0xff; over[40] = 0xff;
        ok(__pagh_dns_parse_a(over, len, 0x1234, "example.com", addr, &rcode) == PAGH_DNS_BAD,
           "rdlength past the end is refused");
    }
    ok(__pagh_dns_parse_a(msg, 5, 0x1234, "example.com", addr, &rcode) == PAGH_DNS_BAD,
       "short header refused");

    /* NXDOMAIN: copy the real response, set RCODE=3 and drop the answers. */
    unsigned char nx[512];
    memcpy(nx, msg, (size_t)len);
    nx[3] = 0x83;
    nx[7] = 0x00;
    ok(__pagh_dns_parse_a(nx, len, 0x1234, "example.com", addr, &rcode) == PAGH_DNS_NAME_ERROR,
       "NXDOMAIN reported");
    ok(rcode == 3, "NXDOMAIN rcode is 3");

    /* TC (truncated) must be refused: there is no TCP fallback. */
    unsigned char tc[512];
    memcpy(tc, msg, (size_t)len);
    tc[2] |= 0x02;
    ok(__pagh_dns_parse_a(tc, len, 0x1234, "example.com", addr, &rcode) == PAGH_DNS_BAD,
       "truncated-flag response refused");

    /* A response flag missing (a query echoed back) is not an answer. */
    unsigned char q[512];
    memcpy(q, msg, (size_t)len);
    q[2] = 0x01;
    ok(__pagh_dns_parse_a(q, len, 0x1234, "example.com", addr, &rcode) == PAGH_DNS_BAD,
       "query echo refused");

    /* An answer whose name is a pointer to itself must not loop forever. */
    unsigned char loop[64];
    memset(loop, 0, sizeof loop);
    loop[0] = 0x12; loop[1] = 0x34; loop[2] = 0x81; loop[3] = 0x80;
    loop[5] = 1; loop[7] = 1;
    loop[12] = 0x01; loop[13] = 'a'; loop[14] = 0x00; loop[15] = 0x00;
    loop[16] = 0x01; loop[17] = 0x00; loop[18] = 0x01;
    loop[19] = 0xc0; loop[20] = 0x13; /* points at itself */
    int looplen = 21 + 10 + 4;
    memset(loop + 21, 0, (size_t)(looplen - 21));
    ok(__pagh_dns_parse_a(loop, looplen, 0x1234, "a", addr, &rcode) == PAGH_DNS_BAD,
       "compression loop refused");

    /* Only an AAAA record: well-formed, but not an A answer. The first answer
     * (name c00c at offset 29, type at 31) becomes AAAA and ANCOUNT drops to 1,
     * so the second, real A record is not reached. */
    unsigned char aaaa[512];
    memcpy(aaaa, msg, (size_t)len);
    aaaa[7] = 0x01;
    aaaa[31] = 0x00; aaaa[32] = 0x1c;
    ok(__pagh_dns_parse_a(aaaa, len, 0x1234, "example.com", addr, &rcode) == PAGH_DNS_NO_ANSWER,
       "AAAA is not an A record");

    /* An A record for another name (a CNAME chain) is not followed. */
    ok(__pagh_dns_parse_a(msg, len, 0x1234, "www.example.com", addr, &rcode) == PAGH_DNS_BAD,
       "answer for another name refused");
}

int main(void) {
    query_cases();
    parse_cases();
    printf("dns test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
