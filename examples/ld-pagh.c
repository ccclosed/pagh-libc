/* ld-pagh: the static entry point for pagh-libc's userland dynamic loader.
 *
 * The loader itself lives in libc (src/dynload.c) so a dynamically linked
 * program can call dlopen() at runtime and see the same loaded-object table
 * that ld-pagh built.  Usage is unchanged:
 *
 *     ld-pagh /mnt/program [args...]
 */
#include <pagh.h>
#include <pagh_internal.h>
#include <stdio.h>

int main(int argc, char **argv, char **envp) {
    if (argc < 2) {
        printf("usage: ld-pagh <program> [args...]\n");
        return 2;
    }
    return __pagh_dl_run(argc - 1, argv + 1, envp);
}
