/* A second plugin, reached only through libplug.so's DT_NEEDED entry.
 *
 * Neither ld-pagh nor hello_dyn ever names this file: it appears in the
 * process only because dlopen("/mnt/libplug.so") followed libplug's DT_NEEDED
 * recursively.  Its constructor must run (dep_is_ready) and its data symbol
 * `dep_global` must be relocated into libplug's GOT for plug_value() to
 * return 42.
 */
#include <stdio.h>

int dep_global = 40;   /* imported by libplug; build.sh re-tags the GOT
                        * relocation to R_RISCV_32 */
static int dep_ready;

__attribute__((constructor)) static void dep_init(void) {
    dep_ready = 1;
    printf("libplugdep: DT_INIT_ARRAY dep_global=%d\n", dep_global);
}

int dep_is_ready(void) { return dep_ready; }
int dep_answer(void) { return dep_ready ? 42 : -1; }
