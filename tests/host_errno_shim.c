/* Host differential-test shim: the pagh sources define/read a plain
 * `int errno`, while glibc's errno is thread-local. Providing this one
 * definition in its own translation unit (no headers, so `errno` is not a
 * macro) lets the library objects link against the host libc. */
int errno;
