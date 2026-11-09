/* Plugin loaded at runtime by hello_dyn through dlopen("/mnt/libplug.so").
 *
 * It deliberately has no link-time dependency on libextra.so: `extra_global`
 * and `printf` are undefined here and the loader must resolve both against
 * objects that are already in the process.  Its DT_NEEDED entry names
 * libplugdep.so, which nothing has loaded yet, so dlopen must follow it
 * recursively before the relocations below can be satisfied.
 *
 * DT_INIT and the DT_INIT_ARRAY constructor prove both constructor paths ran;
 * plug_value() returns 42 only when this object's constructors ran, the
 * dependency's constructor ran, and both imported data words arrived.
 */
#include <stdio.h>

extern int extra_global;          /* libextra.so, already in the process */
extern int dep_global;            /* libplugdep.so, via DT_NEEDED */
extern int dep_is_ready(void);

static int plug_ready;
static int plug_dt_ready;
static int plug_inits;   /* bumped by both constructors: proves they run once */

/* Exported through DT_INIT by the -init link option in build.sh. */
void plug_dt_init(void) {
    plug_dt_ready = 1;
    plug_inits++;
    printf("libplug: DT_INIT\n");
}

__attribute__((constructor)) static void plug_init(void) {
    plug_ready = 1;
    plug_inits++;
    printf("libplug: DT_INIT_ARRAY extra_global=%d\n", extra_global);
}

int plug_init_count(void) { return plug_inits; }

int plug_value(void) {
    if (!plug_ready || !plug_dt_ready || !dep_is_ready()) return -1;
    /* extra_global arrives through the legacy type-6 GLOB_DAT arm and
     * dep_global through R_RISCV_32 (build.sh re-tags both). */
    return extra_global + dep_global - 38;   /* 40 + 40 - 38 = 42 */
}
