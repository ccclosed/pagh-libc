#!/bin/sh
# Host tests: run the pure library code on the build host, against the host's
# libc (differential printf) and against hand-built DNS messages.
#
#   ./tools/host-tests.sh
#
# These are not a substitute for the live QEMU run; they are the inner loop that
# makes claims about formatting and parsing checkable in a second. Note the
# separate compilations below: the *library* sources are renamed on their own,
# so the test translation unit keeps calling the real host `snprintf`. Folding
# both into one command line would rename the host function too and turn the
# differential test into a self-comparison that always passes.
set -eu
cd "$(dirname "$0")/.."
CC=${CC:-cc}
mkdir -p build

echo "== printf differential test (host libc vs pagh vsnprintf) =="
$CC -O2 -Iinclude -c tests/printf_test.c -o build/host_printf_test.o
$CC -O2 -Iinclude -Dprintf=pagh_printf -Dsnprintf=pagh_snprintf \
    -Dvsnprintf=pagh_vsnprintf -Dputs=pagh_puts -Dputchar=pagh_putchar \
    -Dgetchar=pagh_getchar -c src/stdio.c -o build/host_stdio.o
$CC -O2 -Iinclude -c src/float.c -o build/host_float.o
$CC build/host_printf_test.o build/host_stdio.o build/host_float.o -o build/host_printf_test
./build/host_printf_test

echo "== DNS message test (build/parse, compression, negative cases) =="
$CC -O2 -Iinclude tests/dns_test.c src/dns.c -o build/host_dns_test
./build/host_dns_test

echo "== HTTP framing test (head parse, chunked decode, refusal cases) =="
$CC -O2 -Iinclude tests/http_test.c src/http.c -o build/host_http_test
./build/host_http_test

echo "host-tests: PASS"
