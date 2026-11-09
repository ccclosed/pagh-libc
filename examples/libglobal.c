/* RTLD_GLOBAL plugin: hello_dyn loads this one with RTLD_GLOBAL, so later
 * dlsym(RTLD_DEFAULT, ...) calls and later objects' relocations must resolve
 * global_answer(), and dlsym(RTLD_NEXT) from the executable must find this
 * definition of interpose_value() instead of the executable's own. */
#include <stdio.h>

__attribute__((constructor)) static void global_init(void) {
    printf("libglobal: DT_INIT_ARRAY\n");
}

int global_answer(void) { return 77; }

/* Also defined by hello_dyn (which returns 1): RTLD_DEFAULT finds the
 * executable first, RTLD_NEXT skips it and lands here. */
int interpose_value(void) { return 2; }
