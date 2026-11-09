#ifndef PAGH_INTERNAL_H
#define PAGH_INTERNAL_H
/* Resolve a relative path against the process-local cwd (no kernel chdir). */
const char *__pagh_abs(const char *path, char *buf, unsigned long size);
extern char __pagh_cwd[256];

/* Load an ET_DYN program plus its DT_NEEDED libraries and jump to its entry.
 * argc/argv are already shifted: argv[0] is the program path and the rest are
 * the program's arguments (the same shape its main() expects). */
int __pagh_dl_run(int argc, char **argv, char **envp);

/* RFC 1035 message helpers (src/dns.c): pure buffers, no sockets, so the host
 * test in tests/dns_test.c exercises exactly the code the kernel does. */
#include <stdint.h>
#define PAGH_DNS_OK 1
#define PAGH_DNS_NO_ANSWER 0
#define PAGH_DNS_BAD (-1)
#define PAGH_DNS_NAME_ERROR (-2)
int __pagh_dns_encode_name(const char *name, unsigned char *out, int cap);
int __pagh_dns_build_query(const char *name, uint16_t id, unsigned char *out, int cap);
int __pagh_dns_parse_a(const unsigned char *msg, int len, uint16_t id,
                       const char *name, unsigned char addr[4], int *rcode);

/* Float formatting for vsnprintf (src/float.c). */
#include <stddef.h>
int __pagh_fmt_float(char *buf, size_t size, size_t *pos, double value,
                     int conv, int precision, int width, int zero, int left,
                     int plus, int space, int alt);

/* HTTP/1.1 response framing (src/http.c): pure buffers, no sockets, so the
 * host test in tests/http_test.c exercises exactly the code https_get runs. */
#define PAGH_HTTP_LENGTH 0      /* a Content-Length body */
#define PAGH_HTTP_CHUNKED 1     /* a Transfer-Encoding: chunked body */
#define PAGH_HTTP_UNTIL_CLOSE 2 /* no framing header: the body ends at close */

struct pagh_http_head {
    int status;              /* the 3-digit status code */
    int framing;             /* PAGH_HTTP_LENGTH / _CHUNKED / _UNTIL_CLOSE */
    size_t body_off;         /* first body byte, relative to the head buffer */
    unsigned long length;    /* Content-Length when framing == PAGH_HTTP_LENGTH */
};

/* Parse a response head. Returns 1 when the head is complete (`head` filled),
 * 0 while more bytes are needed, -1 when it is malformed. */
int __pagh_http_parse_head(const char *data, size_t len, struct pagh_http_head *head);

/* The chunked decoder's state: fed arbitrary byte runs, so a caller never has
 * to buffer a whole body or assume a chunk boundary lands on a read boundary. */
enum {
    PAGH_CHUNK_SIZE = 0, /* accumulating a chunk-size line */
    PAGH_CHUNK_DATA = 1, /* copying the current chunk's data */
    PAGH_CHUNK_CRLF = 2  /* expecting the CRLF that closes the data */
};

struct pagh_http_chunk {
    int state;
    unsigned long remaining; /* data bytes left in the current chunk */
    unsigned long crlf;      /* 0 or 1: how much of the closing CRLF was seen */
    char line[128];          /* the size line being accumulated */
    size_t line_len;
};

void __pagh_http_chunk_init(struct pagh_http_chunk *state);

/* Feed `len` bytes of a chunked body. Returns 1 when the terminal chunk has
 * been decoded (the body is complete), 0 when more input is needed, -1 on
 * malformed framing, -2 when the body does not fit in `cap` (nothing more is
 * copied). `*out_len` is the number of decoded bytes written so far. */
int __pagh_http_chunk_feed(struct pagh_http_chunk *state, const char *data, size_t len,
                           char *out, size_t cap, size_t *out_len);

#endif
