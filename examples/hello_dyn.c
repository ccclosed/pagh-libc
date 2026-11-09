#include <dlfcn.h>
#include <stdio.h>

extern int extra_answer(void);

int main(int argc, char **argv, char **envp) {
    (void)envp;
    printf("hello from a dynamically linked C program\n");
    printf("argc=%d argv0=%s extra=%d\n", argc, argc ? argv[0] : "", extra_answer());

    /* A missing object must fail without corrupting the loaded-object table. */
    if (dlopen("/mnt/no-such-plugin.so", RTLD_NOW) != 0 || !dlerror()) {
        printf("dlopen error path failed\n");
        return 4;
    }

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
    if (dlclose(h) != 0) return 6;
    return v == 42 ? 0 : 1;
}
