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

#endif
