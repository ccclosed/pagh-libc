/* Dynamic test program: exercises the runtime loader's dlopen/dlsym/dlclose/
 * dlerror API, recursive DT_NEEDED loading and the RTLD_GLOBAL/RTLD_LOCAL
 * scopes.  Run as `run /mnt/ld-pagh /mnt/hello_dyn`; it returns 0 only when
 * every check passed, and each printed line is checked by
 * tools/live-dlopen.sh. */
#include <dlfcn.h>
#include <stdio.h>

extern int extra_answer(void);

/* Defined here and in libglobal.so: dlsym(RTLD_DEFAULT) finds this copy
 * first, dlsym(RTLD_NEXT) from this executable skips it. */
int interpose_value(void) { return 1; }

int main(int argc, char **argv, char **envp) {
    (void)envp;
    printf("hello from a dynamically linked C program\n");
    printf("argc=%d argv0=%s extra=%d\n", argc, argc ? argv[0] : "", extra_answer());

    /* A missing object must fail without corrupting the loaded-object table. */
    if (dlopen("/mnt/no-such-plugin.so", RTLD_NOW) != 0 || !dlerror()) {
        printf("dlopen error path failed\n");
        return 4;
    }

    /* libplug.so's DT_NEEDED names libplugdep.so, which nothing has loaded
     * yet: the whole group must arrive recursively.  No RTLD_GLOBAL, so the
     * group is RTLD_LOCAL. */
    void *h = dlopen("/mnt/libplug.so", RTLD_NOW);
    if (!h) {
        printf("dlopen failed: %s\n", dlerror());
        return 2;
    }
    int (*plug_value)(void) = (int (*)(void))dlsym(h, "plug_value");
    if (!plug_value) {
        printf("dlsym failed: %s\n", dlerror());
        return 3;
    }
    if (dlsym(h, "no_such_symbol") != 0 || !dlerror()) {
        printf("dlsym error path failed\n");
        return 5;
    }
    int v = plug_value();
    printf("dlopen: plug_value=%d\n", v);

    /* A second dlopen of the same path reuses the object: same handle, and
     * the constructors must not run again (they count themselves). */
    int (*plug_init_count)(void) = (int (*)(void))dlsym(h, "plug_init_count");
    if (!plug_init_count || plug_init_count() != 2) {
        printf("libplug constructors did not run exactly twice\n");
        return 17;
    }
    if (dlopen("/mnt/libplug.so", RTLD_NOW) != h) {
        printf("second dlopen returned a different handle\n");
        return 18;
    }
    if (plug_init_count() != 2) {
        printf("second dlopen re-ran the constructors\n");
        return 19;
    }
    printf("dlopen: second dlopen reuses the handle, init count still 2\n");

    /* dlsym(handle) reaches the dependency loaded with the handle... */
    int (*dep_answer)(void) = (int (*)(void))dlsym(h, "dep_answer");
    if (!dep_answer) {
        printf("dlsym(dep_answer) failed: %s\n", dlerror());
        return 7;
    }
    printf("dlopen: dlsym(handle) dep_answer=%d\n", dep_answer());

    /* ...but RTLD_LOCAL keeps the group out of the global scope. */
    if (dlsym(RTLD_DEFAULT, "dep_answer") != 0 || !dlerror()) {
        printf("RTLD_LOCAL leaked into the global scope\n");
        return 8;
    }
    printf("dlopen: RTLD_LOCAL group hidden from RTLD_DEFAULT\n");

    /* RTLD_GLOBAL: this object goes into the global scope. */
    if (!dlopen("/mnt/libglobal.so", RTLD_NOW | RTLD_GLOBAL)) {
        printf("dlopen(libglobal) failed: %s\n", dlerror());
        return 9;
    }
    int (*global_answer)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "global_answer");
    if (!global_answer) {
        printf("RTLD_DEFAULT(global_answer) failed: %s\n", dlerror());
        return 10;
    }
    printf("dlopen: RTLD_GLOBAL global_answer=%d\n", global_answer());

    /* A later RTLD_LOCAL object with no DT_NEEDED resolves its undefined
     * symbol against that global scope. */
    if (!dlopen("/mnt/liblate.so", RTLD_NOW)) {
        printf("dlopen(liblate) failed: %s\n", dlerror());
        return 11;
    }
    printf("dlopen: later plugin resolved the RTLD_GLOBAL symbol\n");

    /* The same rule rejects a later object that needs the RTLD_LOCAL group:
     * the error must name the symbol and the failed load must be rolled
     * back. */
    if (dlopen("/mnt/liblatebad.so", RTLD_NOW) != 0) {
        printf("RTLD_LOCAL group was visible to a later object\n");
        return 12;
    }
    printf("dlopen: RTLD_LOCAL-only symbol rejected: %s\n", dlerror());

    /* A relocation type the loader does not implement must be reported, not
     * applied as garbage. */
    if (dlopen("/mnt/libbadreloc.so", RTLD_NOW) != 0) {
        printf("unsupported relocation was accepted\n");
        return 16;
    }
    printf("dlopen: unsupported relocation rejected: %s\n", dlerror());

    /* RTLD_DEFAULT starts at the executable; RTLD_NEXT skips it. */
    int (*dflt)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "interpose_value");
    int (*next)(void) = (int (*)(void))dlsym(RTLD_NEXT, "interpose_value");
    if (!dflt || dflt() != 1) {
        printf("RTLD_DEFAULT interpose_value failed\n");
        return 13;
    }
    if (!next || next() != 2) {
        printf("RTLD_NEXT interpose_value failed: %s\n", dlerror());
        return 14;
    }
    printf("dlopen: RTLD_DEFAULT interpose_value=%d, RTLD_NEXT interpose_value=%d\n",
           dflt(), next());

    /* RTLD_NEXT obeys the scope: the RTLD_LOCAL group is not in it. */
    if (dlsym(RTLD_NEXT, "dep_answer") != 0 || !dlerror()) {
        printf("RTLD_NEXT reached into an RTLD_LOCAL group\n");
        return 15;
    }
    printf("dlopen: RTLD_NEXT skips RTLD_LOCAL-only symbols\n");

    /* An already-loaded group can still be promoted into the global scope. */
    if (dlopen("/mnt/libplug.so", RTLD_NOW | RTLD_GLOBAL) != h) {
        printf("RTLD_GLOBAL promotion returned a different handle\n");
        return 20;
    }
    if (!dlsym(RTLD_DEFAULT, "dep_answer")) {
        printf("RTLD_GLOBAL promotion did not add the group to the global scope\n");
        return 21;
    }
    printf("dlopen: RTLD_GLOBAL promotes an already-loaded group\n");

    if (dlclose(h) != 0) return 6;
    return v == 42 ? 0 : 1;
}
