#!/bin/sh
# Host tests: run the pure library code on the build host, against the host's
# libc (differential printf/sprintf, getopt family, bsearch/tokenizers/memchr,
# time/strftime, sscanf/vsscanf/fscanf) and against hand-built DNS and HTTP
# messages, plus a synthetic-stream test for getline/getdelim and a table check
# that every errno the header defines has a strerror() text.
#
#   ./tools/host-tests.sh
#
# These are not a substitute for the live QEMU run; they are the inner loop that
# makes claims about formatting and parsing checkable in a second. The errno
# table check needs python3 (only that one; the rest is the host C compiler).
# Note the
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
    -Dvsnprintf=pagh_vsnprintf -Dsprintf=pagh_sprintf \
    -Dvsprintf=pagh_vsprintf -Dvprintf=pagh_vprintf -Dputs=pagh_puts \
    -Dputchar=pagh_putchar -Dgetchar=pagh_getchar \
    -c src/stdio.c -o build/host_stdio.o
$CC -O2 -Iinclude -c src/float.c -o build/host_float.o
$CC build/host_printf_test.o build/host_stdio.o build/host_float.o -o build/host_printf_test
./build/host_printf_test

echo "== DNS message test (build/parse, compression, negative cases) =="
$CC -O2 -Iinclude tests/dns_test.c src/dns.c -o build/host_dns_test
./build/host_dns_test

echo "== HTTP framing test (head parse, chunked decode, refusal cases) =="
$CC -O2 -Iinclude tests/http_test.c src/http.c -o build/host_http_test
./build/host_http_test

echo "== POSIX wrapper ABI test (pollfd/stack_t/sigaction layouts, syscall numbers) =="
$CC -O2 -Iinclude tests/posix_abi_test.c -o build/host_posix_abi_test
./build/host_posix_abi_test

echo "== getopt differential test (getopt/getopt_long/getopt_long_only vs host libc) =="
$CC -O2 -Dgetopt=pagh_getopt -Dgetopt_long=pagh_getopt_long \
    -Dgetopt_long_only=pagh_getopt_long_only -Doptarg=pagh_optarg \
    -Doptind=pagh_optind -Dopterr=pagh_opterr -Doptopt=pagh_optopt \
    -c src/getopt.c -o build/host_getopt.o
$CC -O2 tests/getopt_test.c build/host_getopt.o -o build/host_getopt_test
./build/host_getopt_test

echo "== libc extras differential test (bsearch, strtok_r/strsep, memchr, spans) =="
STRING_RENAMES="-Dstrlen=pagh_strlen -Dstrcmp=pagh_strcmp -Dstrncmp=pagh_strncmp \
    -Dstrcpy=pagh_strcpy -Dstrncpy=pagh_strncpy -Dstrcat=pagh_strcat \
    -Dstrchr=pagh_strchr -Dstrrchr=pagh_strrchr -Dstrstr=pagh_strstr \
    -Dstrdup=pagh_strdup -Dstrspn=pagh_strspn -Dstrcspn=pagh_strcspn \
    -Dstrtok_r=pagh_strtok_r -Dstrsep=pagh_strsep -Dmemcpy=pagh_memcpy \
    -Dmemmove=pagh_memmove -Dmemset=pagh_memset -Dmemcmp=pagh_memcmp \
    -Dmemchr=pagh_memchr"
$CC -O2 -Iinclude $STRING_RENAMES -c src/string.c -o build/host_string.o
$CC -O2 -Iinclude -Datoi=pagh_atoi -Datol=pagh_atol -Dabs=pagh_abs \
    -Dabort=pagh_abort -Datexit=pagh_atexit -Dqsort=pagh_qsort \
    -Dbsearch=pagh_bsearch -D__run_atexit=__pagh_host_atexit \
    -D_exit=pagh_host_exit -c src/stdlib.c -o build/host_stdlib.o
$CC -O2 tests/libc_extras_test.c build/host_string.o build/host_stdlib.o \
    -o build/host_libc_extras_test
./build/host_libc_extras_test

echo "== getline/getdelim test (synthetic stream: lines, growth, NUL, errors) =="
$CC -O2 -Iinclude -Dgetline=pagh_getline -Dgetdelim=pagh_getdelim \
    -c src/getline.c -o build/host_getline.o
$CC -O2 -Iinclude -Dgetline=pagh_getline -Dgetdelim=pagh_getdelim \
    tests/getline_test.c build/host_getline.o -o build/host_getline_test
./build/host_getline_test

echo "== scanf differential test (sscanf/vsscanf/fscanf vs host libc) =="
SCAN_RENAMES="-Dsscanf=pagh_sscanf -Dvsscanf=pagh_vsscanf -Dscanf=pagh_scanf \
    -Dvscanf=pagh_vscanf -Dfscanf=pagh_fscanf -Dvfscanf=pagh_vfscanf \
    -Dfgetc=pagh_scan_fgetc"
$CC -O2 -Iinclude $SCAN_RENAMES -c src/scanf.c -o build/host_scanf.o
$CC -O2 tests/scanf_test.c build/host_scanf.o -o build/host_scanf_test
./build/host_scanf_test

echo "== time differential test (clock/time/gettimeofday, gmtime_r/localtime_r, strftime) =="
TIME_RENAMES="-Dclock_gettime=pagh_clock_gettime -Dnanosleep=pagh_nanosleep \
    -Dtime=pagh_time -Dsleep=pagh_sleep -Dusleep=pagh_usleep \
    -Dgettimeofday=pagh_gettimeofday -Dgmtime_r=pagh_gmtime_r \
    -Dlocaltime_r=pagh_localtime_r -Dgmtime=pagh_gmtime \
    -Dlocaltime=pagh_localtime -Dstrftime=pagh_strftime"
$CC -O2 -Iinclude $TIME_RENAMES -c src/time.c -o build/host_time.o
# glibc's errno is thread-local; the library sources use a plain int. This
# one-definition shim is compiled without headers so `errno` is not a macro.
$CC -O2 -c tests/host_errno_shim.c -o build/host_errno_shim.o
$CC -O2 tests/time_test.c build/host_time.o build/host_errno_shim.o \
    -o build/host_time_test
./build/host_time_test

echo "== errno table check (include/errno.h vs src/strerror.c) =="
python3 tools/check_errno.py

echo "host-tests: PASS"
