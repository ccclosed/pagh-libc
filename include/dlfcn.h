#ifndef PAGH_DLFCN_H
#define PAGH_DLFCN_H

/* Runtime dynamic loading for the native pagh-riscv ABI.
 *
 * A program must have been started by the userland loader `ld-pagh`: that is
 * what publishes the loaded-object table (`__pagh_dl_state`) into the shared
 * libc.  `dlopen` then maps one additional ET_DYN shared object, relocates it
 * against the objects already in the process and runs its constructors.
 *
 * Supported: RTLD_LAZY/RTLD_NOW (both bind eagerly), RTLD_GLOBAL/RTLD_LOCAL
 * (accepted; every object is globally visible to later relocations).
 * Not supported: symbol versioning, TLS, `dlclose` unloading, `dlinfo`,
 * `dladdr`, dependency loading for the new object, thread safety.
 */

#define RTLD_LAZY   0x1
#define RTLD_NOW    0x2
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL  0

/* Load `path` (an ET_DYN shared object) and return an opaque handle, or NULL.
 * On failure `dlerror()` describes the reason.  A second dlopen of the same
 * path returns the existing handle without running the constructors again. */
void *dlopen(const char *path, int flags);

/* Look `name` up in the dynamic symbol table of the object behind `handle`.
 * Returns the symbol address, or NULL (with dlerror set) when it is absent. */
void *dlsym(void *handle, const char *name);

/* Present for source compatibility: unloading is a documented no-op because
 * the process has one address space and libc keeps no per-object reference
 * counts.  Always returns 0. */
int dlclose(void *handle);

/* The most recent dlopen/dlsym error, or NULL.  The string is valid until the
 * next dlopen/dlsym call. */
char *dlerror(void);

#endif
