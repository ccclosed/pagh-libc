#ifndef PAGH_DLFCN_H
#define PAGH_DLFCN_H

/* Runtime dynamic loading for the native pagh-riscv ABI.
 *
 * A program must have been started by the userland loader `ld-pagh`: that is
 * what publishes the loaded-object table (`__pagh_dl_state`) into the shared
 * libc.  `dlopen` then maps one additional ET_DYN object, follows its
 * `DT_NEEDED` entries recursively (reusing objects already in the table),
 * relocates the whole group against the process and runs its constructors.
 *
 * Scopes:
 *
 *   - `RTLD_GLOBAL` puts the loaded object *and everything its DT_NEEDED
 *     pulled in* into the global scope: later `dlsym(RTLD_DEFAULT, ...)` calls
 *     and later relocations can resolve against those symbols.
 *   - `RTLD_LOCAL` (the default) keeps the group out of the global scope: its
 *     symbols are found through the handle returned by `dlopen` (and by the
 *     relocations of the group itself) but not by later lookups.
 *   - `dlsym(RTLD_DEFAULT, name)` searches the global scope in load order;
 *     `dlsym(RTLD_NEXT, name)` searches the scope its caller would be
 *     relocated against, starting after the caller's own object.  The caller
 *     is identified from the return address, so it must live in one of the
 *     loaded objects.
 *
 * Supported: RTLD_LAZY/RTLD_NOW (both bind eagerly - there is no PLT
 * trampoline and no lazy resolver), RTLD_GLOBAL/RTLD_LOCAL, recursive
 * DT_NEEDED, dlsym(RTLD_DEFAULT)/dlsym(RTLD_NEXT).
 * Not supported: symbol versioning, TLS, `dlclose` unloading, `dladdr`,
 * `dlinfo`, `RTLD_NOLOAD`, thread safety.  Of the flag bits, only RTLD_GLOBAL
 * changes behaviour; the others are accepted and ignored.
 */

#define RTLD_LAZY   0x1
#define RTLD_NOW    0x2
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL  0

/* glibc-compatible pseudo-handles. */
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT    ((void *)-1L)

/* Load `path` (an ET_DYN shared object) and its DT_NEEDED tree, and return an
 * opaque handle, or NULL.  On failure `dlerror()` describes the reason and the
 * objects that call did map are unmapped.  A second dlopen of the same path
 * returns the existing handle without running the constructors again; passing
 * RTLD_GLOBAL then still promotes that already-loaded group. */
void *dlopen(const char *path, int flags);

/* Look `name` up in the dynamic symbol tables reachable from `handle`:
 * the object itself and the dependencies loaded with it.  `handle` may also be
 * RTLD_DEFAULT or RTLD_NEXT.  Returns the symbol address, or NULL (with
 * dlerror set) when it is absent. */
void *dlsym(void *handle, const char *name);

/* Present for source compatibility: unloading is a documented no-op because
 * the process has one address space and libc keeps no per-object reference
 * counts.  Always returns 0. */
int dlclose(void *handle);

/* The most recent dlopen/dlsym error, or NULL.  The string is valid until the
 * next dlopen/dlsym call. */
char *dlerror(void);

#endif
