/* One HTTPS GET through the convenience call: `https_get(host, path, out, cap)`
 * resolves the name, opens TCP port 443, hands the socket to the kernel for the
 * fail-closed TLS 1.3 handshake, sends the request and copies the response body
 * into the caller's buffer.
 *
 *   run /mnt/https_get example.com
 *   run /mnt/https_get example.com /index.html
 *   run /mnt/https_get example.com / 100          (a cap too small: ENOSPC)
 *   run /mnt/https_get no-such-host.pagh.invalid  (fails closed: no body)
 *
 * The point of the example is the error reporting: every refusal is -1 with the
 * kernel's or the resolver's errno, never a body that might be truncated or
 * unauthenticated. `errno=129` (EKEYREJECTED) is a certificate the kernel
 * rejected, `errno=2` (ENOENT) a name that did not resolve, `errno=5` (EIO) a
 * response that was not 200, `errno=28` (ENOSPC) a body larger than the buffer —
 * refused, not truncated.
 *
 * Exit codes: 0 success, 1 the fetch failed (errno says why), 2 usage,
 * 3 out of memory. */
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pagh_tls.h>

#define BODY_CAP 65536

int main(int argc, char **argv) {
    if (argc < 2 || argc > 4) {
        printf("usage: https_get host [path] [cap]\n");
        return 2;
    }
    const char *host = argv[1];
    const char *path = argc > 2 ? argv[2] : "/";
    long cap = argc > 3 ? atol(argv[3]) : BODY_CAP;
    if (cap <= 0 || cap > 16L * 1024 * 1024) {
        printf("https_get: cap %ld is not between 1 and 16777216\n", cap);
        return 2;
    }

    char *body = malloc((size_t)cap);
    if (!body) {
        printf("https_get: out of memory (%ld bytes)\n", cap);
        return 3;
    }

    printf("https_get: GET %s%s (cap %ld)\n", host, path, cap);
    ssize_t length = https_get(host, path, body, (size_t)cap);
    if (length < 0) {
        printf("https_get: FAILED errno=%d %s (h_errno=%d %s)\n", errno, strerror(errno),
               h_errno, hstrerror(h_errno));
        printf("https_get: no body was returned\n");
        free(body);
        return 1;
    }

    printf("https_get: body %ld bytes\n", (long)length);
    printf("https_get: ----- body -----\n");
    if (length && write(1, body, (size_t)length) != length) {
        printf("\nhttps_get: body write failed\n");
        free(body);
        return 1;
    }
    printf("\nhttps_get: ----- end body -----\n");
    printf("https_get: ok\n");
    free(body);
    return 0;
}
