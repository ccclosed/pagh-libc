/* Plugin loaded at runtime by hello_dyn through dlopen("/mnt/libplug.so").
 *
 * It deliberately has no DT_NEEDED entry: `extra_global` and `printf` are
 * undefined here and the loader must resolve both against objects that are
 * already in the process (libextra.so and libpagh-c.so).  DT_INIT and the
 * DT_INIT_ARRAY constructor prove both constructor paths ran; plug_value
 * returns 42 only when both did.
 */
#include <stdio.h>

extern int extra_global;

static int plug_ready;
static int plug_dt_ready;

/* Exported through DT_INIT by the -init link option in build.sh. */
void plug_dt_init(void) {
    plug_dt_ready = 1;
    printf("libplug: DT_INIT\n");
}

__attribute__((constructor)) static void plug_init(void) {
    plug_ready = 1;
    printf("libplug: DT_INIT_ARRAY extra_global=%d\n", extra_global);
}

int plug_value(void) {
    return (plug_ready && plug_dt_ready) ? extra_global + 2 : -1;
}
