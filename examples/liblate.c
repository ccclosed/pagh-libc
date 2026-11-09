/* A later plugin with no DT_NEEDED and one undefined symbol, global_answer,
 * which lives in libglobal.so.  libglobal was loaded earlier with
 * RTLD_GLOBAL, so dlopen must resolve this relocation against the global
 * scope; the constructor then proves the address works at runtime. */
#include <stdio.h>

extern int global_answer(void);

__attribute__((constructor)) static void late_init(void) {
    printf("liblate: DT_INIT_ARRAY global_answer=%d\n", global_answer());
}
