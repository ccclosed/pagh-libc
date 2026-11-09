/* Runtime dynamic loading for the native pagh-riscv ABI.
 *
 * This is the same userland loader `ld-pagh` used to keep in examples/: it
 * reads an ET_DYN object into a private buffer, maps its PT_LOAD segments from
 * anonymous pages, applies R_RISCV_RELATIVE/R_RISCV_32/R_RISCV_64/
 * R_RISCV_JUMP_SLOT (and the legacy type-6 GLOB_DAT arm described below), and
 * calls DT_INIT/DT_INIT_ARRAY.  It now lives in libc so the already-running
 * program can add an object at runtime through `dlopen`.
 *
 * The loader state is per-process and owned by the loader copy inside the
 * statically linked `ld-pagh`.  That copy publishes a pointer in the shared
 * libc's `__pagh_dl_state` global before any constructors run; `dlopen` and
 * `dlsym` in libpagh-c.so operate on that one table.  Pointers in the table
 * are ordinary process addresses, so a shared libc can use them directly.
 *
 * Scope model - what RTLD_GLOBAL/RTLD_LOCAL mean here:
 *
 *   - every object belongs to the *group* of the dlopen() that caused its
 *     load; the group is identified by the table index of that call's root
 *     object.  The program and its DT_NEEDED libraries are group 0;
 *   - a relocation in object X is resolved against the global scope (group 0
 *     plus every group promoted by RTLD_GLOBAL) first, then against X's own
 *     group: its root and everything that root's DT_NEEDED pulled in,
 *     recursively;
 *   - dlopen() follows the new object's DT_NEEDED recursively and skips an
 *     object that is already loaded, matched by absolute path;
 *   - dlsym(handle) searches the handle's group, dlsym(RTLD_DEFAULT) the
 *     global scope, and dlsym(RTLD_NEXT) the same scope the caller would be
 *     relocated against, starting after the caller's own object.
 *
 * No kernel support is involved: mapping is mmap(MAP_ANONYMOUS), protection is
 * mprotect, a failed dlopen tears its own mappings down with munmap, and the
 * loaded object is just bytes copied from the file.
 */

#include <pagh.h>
#include <pagh_internal.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define ET_DYN 3
#define EM_RISCV 243

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define SHT_RELA 4
#define SHT_DYNSYM 11
#define SHN_UNDEF 0
#define SHN_ABS 0xfff1
#define STT_TLS 6

#define R_RISCV_NONE 0
#define R_RISCV_32 1
#define R_RISCV_64 2
#define R_RISCV_RELATIVE 3
#define R_RISCV_JUMP_SLOT 5
/* RISC-V has no R_RISCV_GLOB_DAT: the psABI uses R_RISCV_64 for data GOT
 * slots, and number 6 is R_RISCV_TLS_DTPMOD32.  The pre-0.9 pagh loader
 * nevertheless called type 6 GLOB_DAT.  Accept type 6 as a legacy address
 * relocation, but only for a non-TLS symbol; a real STT_TLS type-6 relocation
 * is rejected rather than silently written as an address.  rust-lld never
 * emits it, so build.sh re-tags one libplug relocation only to test this
 * compatibility arm. */
#define R_RISCV_GLOB_DAT_COMPAT 6

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_STRTAB 5
#define DT_INIT 12
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27

#define DL_MAX_OBJECTS 16
#define DL_MAX_DEPS 8
#define DL_MAX_DEPTH 8
#define DL_MAX_PATH 512
#define DL_MAX_FILE (8UL << 20)
#define DL_MAX_IMAGE (64UL << 20)

struct ehdr {
    unsigned char ident[16];
    unsigned short type, machine;
    unsigned int version;
    unsigned long entry, phoff, shoff;
    unsigned int flags;
    unsigned short ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct phdr { unsigned int type, flags; unsigned long off, vaddr, paddr, filesz, memsz, align; };
struct shdr { unsigned int name, type; unsigned long flags, addr, off, size; unsigned int link, info; unsigned long addralign, entsize; };
struct sym { unsigned int name; unsigned char info, other; unsigned short shndx; unsigned long value, size; };
struct rela { unsigned long off, info; long addend; };

_Static_assert(sizeof(struct ehdr) == 64, "ELF64 header layout");
_Static_assert(sizeof(struct phdr) == 56, "ELF64 program header layout");
_Static_assert(sizeof(struct shdr) == 64, "ELF64 section header layout");
_Static_assert(sizeof(struct sym) == 24, "ELF64 symbol layout");
_Static_assert(sizeof(struct rela) == 24, "ELF64 Rela layout");

struct dl_object {
    char *path;
    unsigned char *data;
    unsigned long size;
    unsigned char *base;
    struct ehdr *eh;
    struct shdr *sh;
    unsigned long total;
    int group;   /* index of the dlopen() root that pulled this object in */
    int global;  /* in the global scope: group 0 or RTLD_GLOBAL */
};

struct dl_state {
    int count;
    struct dl_object objs[DL_MAX_OBJECTS];
};

struct dynsym_view {
    unsigned char *tab;
    const char *strings;
    unsigned long strsz;
    unsigned long count;
    unsigned long entsize;
};

/* The loader copy in ld-pagh points this at its own table from
 * __pagh_dl_run.  The copy inside libpagh-c.so starts NULL and is assigned by
 * ld-pagh through the dynamic symbol table, so no state is duplicated. */
struct dl_state *__pagh_dl_state __attribute__((used, visibility("default")));

static struct dl_state dl_state;

static char dl_error_buf[192];
static const char *dl_error;

static void dl_error_set(const char *msg) {
    snprintf(dl_error_buf, sizeof dl_error_buf, "%s", msg);
    dl_error = dl_error_buf;
}
static void dl_error_set1(const char *fmt, const char *arg) {
    snprintf(dl_error_buf, sizeof dl_error_buf, fmt, arg);
    dl_error = dl_error_buf;
}
static void dl_error_setu(const char *fmt, unsigned long arg) {
    snprintf(dl_error_buf, sizeof dl_error_buf, fmt, arg);
    dl_error = dl_error_buf;
}

static int fits(unsigned long off, unsigned long len, unsigned long size) {
    return off <= size && len <= size - off;
}

static long sys_mmap(unsigned long len, int prot) {
    return __pagh_syscall6(SYS_MMAP, 0, (long)len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}
static long sys_mprotect(unsigned long addr, unsigned long len, int prot) {
    return __pagh_syscall3(SYS_MPROTECT, (long)addr, (long)len, prot);
}
static long sys_munmap(unsigned long addr, unsigned long len) {
    /* No two-argument stub exists; the extra zero arguments are ignored. */
    return __pagh_syscall6(SYS_MUNMAP, (long)addr, (long)len, 0, 0, 0, 0);
}

static int read_file(struct dl_object *o, const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) { dl_error_set1("cannot open %s", path); return -1; }
    struct pagh_stat st;
    if (fstat(fd, &st) < 0 || st.size == 0 || st.size > DL_MAX_FILE) {
        close(fd);
        dl_error_set1("cannot stat %s (or file is empty/too large)", path);
        return -1;
    }
    unsigned char *buf = malloc((size_t)st.size);
    if (!buf) { close(fd); dl_error_set("out of memory reading object"); return -1; }
    long n = read(fd, buf, (size_t)st.size);
    close(fd);
    if (n != (long)st.size) {
        dl_error_set1("short read of %s", path);
        return -1;
    }
    struct ehdr *eh = (struct ehdr *)buf;
    if (memcmp(eh->ident, "\177ELF", 4) ||
        eh->ident[4] != 2 /* ELFCLASS64 */ || eh->ident[5] != 1 /* ELFDATA2LSB */ ||
        eh->machine != EM_RISCV || eh->type != ET_DYN) {
        dl_error_set1("not a 64-bit little-endian RISC-V ET_DYN object: %s", path);
        return -1;
    }
    if (eh->phentsize < sizeof(struct phdr) || eh->phoff > st.size ||
        eh->phnum > (st.size - eh->phoff) / eh->phentsize) {
        dl_error_set1("bad program-header table in %s", path);
        return -1;
    }
    if (eh->shnum) {
        if (eh->shentsize < sizeof(struct shdr) || eh->shoff > st.size ||
            eh->shnum > (st.size - eh->shoff) / eh->shentsize) {
            dl_error_set1("bad section-header table in %s", path);
            return -1;
        }
        o->sh = (struct shdr *)(buf + eh->shoff);
    }
    o->path = strdup(path);
    if (!o->path) { dl_error_set("out of memory storing object path"); return -1; }
    o->data = buf;
    o->size = (unsigned long)st.size;
    o->eh = eh;
    return 0;
}

static int load_object(struct dl_object *o) {
    unsigned long max = 0;
    for (int i = 0; i < o->eh->phnum; i++) {
        struct phdr *p = (struct phdr *)(o->data + o->eh->phoff + (unsigned long)i * o->eh->phentsize);
        if (p->type != PT_LOAD) continue;
        if (p->filesz > p->memsz || !fits(p->off, p->filesz, o->size)) {
            dl_error_set1("bad PT_LOAD in %s", o->path);
            return -1;
        }
        if (p->vaddr > DL_MAX_IMAGE || p->memsz > DL_MAX_IMAGE - p->vaddr) {
            dl_error_set1("PT_LOAD too large in %s", o->path);
            return -1;
        }
        unsigned long end = p->vaddr + p->memsz;
        if (end > max) max = end;
    }
    if (!max) { dl_error_set1("object has no loadable segments: %s", o->path); return -1; }
    max = (max + 4095) & ~4095UL;
    long base = sys_mmap(max, PROT_READ | PROT_WRITE);
    if (base < 0) { dl_error_set1("cannot map %s", o->path); return -1; }
    o->base = (unsigned char *)base;
    o->total = max;
    memset(o->base, 0, max);
    for (int i = 0; i < o->eh->phnum; i++) {
        struct phdr *p = (struct phdr *)(o->data + o->eh->phoff + (unsigned long)i * o->eh->phentsize);
        if (p->type != PT_LOAD || !p->filesz) continue;
        memcpy(o->base + p->vaddr, o->data + p->off, p->filesz);
    }
    return 0;
}

static int protect_object(struct dl_object *o) {
    for (unsigned long voff = 0; voff < o->total; voff += 4096) {
        int prot = PROT_READ | PROT_WRITE;
        int exec = 0;
        for (int i = 0; i < o->eh->phnum; i++) {
            struct phdr *p = (struct phdr *)(o->data + o->eh->phoff + (unsigned long)i * o->eh->phentsize);
            if (p->type != PT_LOAD) continue;
            unsigned long start = p->vaddr;
            unsigned long end = p->vaddr + p->memsz;
            if (voff + 4096 <= start || voff >= end) continue;
            if (p->flags & 1) exec = 1;
            if (!(p->flags & 2)) prot &= ~PROT_WRITE;
        }
        if (exec) prot = PROT_READ | PROT_EXEC;
        if (sys_mprotect((unsigned long)o->base + voff, 4096, prot) < 0) {
            dl_error_set1("mprotect failed for %s", o->path);
            return -1;
        }
    }
    return 0;
}

static struct shdr *find_section(struct dl_object *o, unsigned int type) {
    for (int i = 0; i < o->eh->shnum; i++)
        if (o->sh[i].type == type) return &o->sh[i];
    return 0;
}

static int dynsym_view(struct dl_object *o, struct dynsym_view *v) {
    struct shdr *sym = find_section(o, SHT_DYNSYM);
    if (!sym || sym->link >= o->eh->shnum) return -1;
    struct shdr *str = &o->sh[sym->link];
    if (!fits(sym->off, sym->size, o->size) || !fits(str->off, str->size, o->size)) return -1;
    if (sym->addr > o->total || sym->size > o->total - sym->addr) return -1;
    v->entsize = sym->entsize ? sym->entsize : sizeof(struct sym);
    if (v->entsize < sizeof(struct sym)) return -1;
    v->tab = o->base + sym->addr;
    v->strings = (const char *)(o->data + str->off);
    v->strsz = str->size;
    v->count = sym->size / v->entsize;
    return 0;
}

static const char *dynstr(const struct dynsym_view *v, unsigned long off) {
    if (off >= v->strsz) return 0;
    const char *s = v->strings + off;
    for (unsigned long i = 0; i < v->strsz - off; i++)
        if (!s[i]) return s;
    return 0;
}

static unsigned long resolve_in(struct dl_object *o, const char *name) {
    struct dynsym_view v;
    if (dynsym_view(o, &v) < 0) return 0;
    for (unsigned long i = 0; i < v.count; i++) {
        struct sym *s = (struct sym *)(v.tab + i * v.entsize);
        if (s->shndx == SHN_UNDEF || (s->info & 0xf) == STT_TLS) continue;
        const char *nm = dynstr(&v, s->name);
        if (!nm || strcmp(nm, name)) continue;
        if (s->shndx == SHN_ABS) return s->value;
        return (unsigned long)o->base + s->value;
    }
    return 0;
}

/* The global scope: the program's group and every RTLD_GLOBAL group. */
static unsigned long resolve_global(struct dl_state *st, const char *name) {
    for (int i = 0; i < st->count; i++)
        if (st->objs[i].global) {
            unsigned long addr = resolve_in(&st->objs[i], name);
            if (addr) return addr;
        }
    return 0;
}

/* What a relocation in objs[idx] resolves against: the global scope first,
 * then the object's own dlopen() group. */
static unsigned long resolve_for(struct dl_state *st, int idx, const char *name) {
    unsigned long addr = resolve_global(st, name);
    if (addr) return addr;
    int group = st->objs[idx].group;
    for (int i = 0; i < st->count; i++) {
        struct dl_object *o = &st->objs[i];
        if (o->global || o->group != group) continue;
        addr = resolve_in(o, name);
        if (addr) return addr;
    }
    return 0;
}

/* dlsym(RTLD_NEXT): the caller's scope, after the caller's own object. */
static unsigned long resolve_next(struct dl_state *st, int from, const char *name) {
    int group = st->objs[from].group;
    for (int i = from + 1; i < st->count; i++) {
        struct dl_object *o = &st->objs[i];
        if (!o->global && o->group != group) continue;
        unsigned long addr = resolve_in(o, name);
        if (addr) return addr;
    }
    return 0;
}

static void promote_group(struct dl_state *st, int group) {
    for (int i = 0; i < st->count; i++)
        if (st->objs[i].group == group) st->objs[i].global = 1;
}

static int relocate(struct dl_object *o, struct dl_state *st, int idx) {
    struct dynsym_view v;
    int have_syms = dynsym_view(o, &v) == 0;
    for (int i = 0; i < o->eh->shnum; i++) {
        struct shdr *r = &o->sh[i];
        if (r->type != SHT_RELA) continue;
        unsigned long entsize = r->entsize ? r->entsize : sizeof(struct rela);
        if (entsize < sizeof(struct rela) || !fits(r->off, r->size, o->size)) {
            dl_error_set1("bad relocation section in %s", o->path);
            return -1;
        }
        unsigned long n = r->size / entsize;
        for (unsigned long j = 0; j < n; j++) {
            struct rela *re = (struct rela *)(o->data + r->off + j * entsize);
            unsigned type = (unsigned)re->info & 0xffffffffu;
            unsigned si = (unsigned)(re->info >> 32);
            if (type == R_RISCV_NONE) continue;
            /* R_RISCV_32 writes a 32-bit word, everything else here an
             * address-sized one. */
            unsigned long width = type == R_RISCV_32 ? 4 : 8;
            if (re->off > o->total || width > o->total - re->off) {
                dl_error_set1("relocation offset outside the image in %s", o->path);
                return -1;
            }
            unsigned long where = (unsigned long)o->base + re->off;
            unsigned long val = 0;
            if (type == R_RISCV_RELATIVE) {
                val = (unsigned long)o->base + (unsigned long)re->addend;
            } else if (type == R_RISCV_32 || type == R_RISCV_64 ||
                       type == R_RISCV_JUMP_SLOT || type == R_RISCV_GLOB_DAT_COMPAT) {
                if (!have_syms || si >= v.count) {
                    dl_error_set1("relocation without a dynamic symbol in %s", o->path);
                    return -1;
                }
                struct sym *s = (struct sym *)(v.tab + si * v.entsize);
                if ((s->info & 0xf) == STT_TLS) {
                    dl_error_set1("TLS relocation is not supported in %s", o->path);
                    return -1;
                }
                const char *nm = dynstr(&v, s->name);
                if (!nm) { dl_error_set1("bad symbol name in %s", o->path); return -1; }
                unsigned long addr;
                if (s->shndx == SHN_ABS) addr = s->value;
                else if (s->shndx != SHN_UNDEF) addr = (unsigned long)o->base + s->value;
                else {
                    addr = resolve_for(st, idx, nm);
                    if (!addr) { dl_error_set1("unresolved symbol %s", nm); return -1; }
                }
                val = addr + (unsigned long)re->addend;
            } else {
                dl_error_setu("unsupported relocation type %lu", type);
                return -1;
            }
            if (type == R_RISCV_32) *(unsigned int *)where = (unsigned int)val;
            else *(unsigned long *)where = val;
        }
    }
    return 0;
}

static unsigned long dyn_value(struct dl_object *o, unsigned long tag) {
    for (int i = 0; i < o->eh->phnum; i++) {
        struct phdr *p = (struct phdr *)(o->data + o->eh->phoff + (unsigned long)i * o->eh->phentsize);
        if (p->type != PT_DYNAMIC) continue;
        unsigned long *d = (unsigned long *)(o->base + p->vaddr);
        for (unsigned long j = 0; j + 1 < p->memsz / 8; j += 2) {
            if (d[j] == tag) return d[j + 1];
            if (d[j] == DT_NULL) break;
        }
    }
    return 0;
}

static void call_inits(struct dl_object *o) {
    unsigned long init = dyn_value(o, DT_INIT);
    if (init && init < o->total) ((void (*)(void))((unsigned long)o->base + init))();
    unsigned long arr = dyn_value(o, DT_INIT_ARRAY);
    unsigned long sz = dyn_value(o, DT_INIT_ARRAYSZ);
    if (arr && sz && arr < o->total && sz <= o->total - arr) {
        void (**fns)(void) = (void (**)(void))((unsigned long)o->base + arr);
        for (unsigned long i = 0; i < sz / sizeof(void *); i++) fns[i]();
    }
}

static int file_dir(const char *path, char *out, unsigned long size) {
    const char *slash = strrchr(path, '/');
    if (!slash) { strncpy(out, ".", size); return 0; }
    unsigned long n = (unsigned long)(slash - path);
    if (n >= size) return -1;
    memcpy(out, path, n); out[n] = 0;
    return 0;
}

static int collect_needed(struct dl_object *o, unsigned long *offs, int max, int *count) {
    *count = 0;
    for (int i = 0; i < o->eh->phnum; i++) {
        struct phdr *p = (struct phdr *)(o->data + o->eh->phoff + (unsigned long)i * o->eh->phentsize);
        if (p->type != PT_DYNAMIC) continue;
        unsigned long *d = (unsigned long *)(o->base + p->vaddr);
        for (unsigned long j = 0; j + 1 < p->memsz / 8; j += 2) {
            if (d[j] == DT_NULL) break;
            if (d[j] != DT_NEEDED) continue;
            if (*count >= max) { dl_error_set("too many DT_NEEDED libraries"); return -1; }
            offs[(*count)++] = d[j + 1];
        }
    }
    return 0;
}

static const char *needed_name(struct dl_object *o, unsigned long strtab, unsigned long off) {
    if (!strtab || strtab >= o->total || off > o->total - strtab) return 0;
    const char *s = (const char *)(o->base + strtab + off);
    for (unsigned long i = 0; i < o->total - (strtab + off); i++)
        if (!s[i]) return s;
    return 0;
}

static int object_index(struct dl_state *st, void *handle) {
    unsigned long h = (unsigned long)handle;
    unsigned long base = (unsigned long)st->objs;
    unsigned long end = base + sizeof(st->objs);
    if (h < base || h >= end) return -1;
    unsigned long off = h - base;
    if (off % sizeof(struct dl_object)) return -1;
    unsigned long idx = off / sizeof(struct dl_object);
    if (idx >= (unsigned long)st->count) return -1;
    return (int)idx;
}

static int object_containing(struct dl_state *st, void *addr) {
    unsigned long a = (unsigned long)addr;
    for (int i = 0; i < st->count; i++) {
        struct dl_object *o = &st->objs[i];
        if (o->base && a >= (unsigned long)o->base && a < (unsigned long)o->base + o->total)
            return i;
    }
    return -1;
}

/* Load one object plus its DT_NEEDED tree.  `group` is the index of the root
 * object of the dlopen() (or 0 for the program itself); `global` says whether
 * that group goes into the global scope.  Returns the object's table index, or
 * -1 with dlerror set.  Already-loaded paths are reused, never mapped twice. */
static int open_object(struct dl_state *st, const char *path, int group, int global, int depth) {
    char abs[DL_MAX_PATH];
    __pagh_abs(path, abs, sizeof abs);
    unsigned long alen = strlen(abs);
    if (!alen) { dl_error_set("dlopen: empty path"); return -1; }
    /* __pagh_abs truncates at the buffer size, so a full buffer may be a
     * silently shortened path: refuse it rather than open the wrong file. */
    if (alen >= sizeof abs - 1) { dl_error_set("dlopen: object path is too long"); return -1; }

    for (int i = 0; i < st->count; i++) {
        if (!st->objs[i].path || strcmp(st->objs[i].path, abs)) continue;
        /* Already loaded: dlopen() returns the same handle and does not run
         * the constructors again.  A later RTLD_GLOBAL request still promotes
         * the whole existing group into the global scope. */
        if (global) promote_group(st, st->objs[i].group);
        return i;
    }
    if (depth >= DL_MAX_DEPTH) {
        dl_error_set1("DT_NEEDED nesting is too deep at %s", abs);
        return -1;
    }
    if (st->count >= DL_MAX_OBJECTS) { dl_error_set("too many loaded objects"); return -1; }

    struct dl_object *o = &st->objs[st->count];
    memset(o, 0, sizeof *o);
    if (read_file(o, abs) < 0) return -1;
    if (load_object(o) < 0) return -1;
    o->group = group;
    o->global = global;
    int idx = st->count++;

    unsigned long needed[DL_MAX_DEPS];
    int nneeded = 0;
    if (collect_needed(o, needed, DL_MAX_DEPS, &nneeded) < 0) return -1;
    if (!nneeded) return idx;

    unsigned long strtab = dyn_value(o, DT_STRTAB);
    char dir[DL_MAX_PATH];
    if (file_dir(abs, dir, sizeof dir) < 0) {
        dl_error_set1("object path too long: %s", abs);
        return -1;
    }
    for (int i = 0; i < nneeded; i++) {
        const char *name = needed_name(o, strtab, needed[i]);
        if (!name || !*name) { dl_error_set1("bad DT_NEEDED string in %s", abs); return -1; }
        char dep[DL_MAX_PATH];
        if (strchr(name, '/')) {
            /* A DT_NEEDED with a slash names a path, not a bare soname. */
            __pagh_abs(name, dep, sizeof dep);
        } else {
            unsigned long dl = strlen(dir), nl = strlen(name);
            if (dl + 1 + nl + 1 > sizeof dep) {
                dl_error_set1("dependency path too long in %s", abs);
                return -1;
            }
            memcpy(dep, dir, dl); dep[dl] = '/'; memcpy(dep + dl + 1, name, nl + 1);
        }
        if (open_object(st, dep, group, global, depth + 1) < 0) return -1;
    }
    return idx;
}

/* Tear down everything a failed dlopen() mapped, so a broken object does not
 * permanently consume the kernel's small anonymous-mapping budget. */
static void unload_group(struct dl_state *st, int first) {
    for (int i = first; i < st->count; i++) {
        struct dl_object *o = &st->objs[i];
        if (o->base) sys_munmap((unsigned long)o->base, o->total);
        memset(o, 0, sizeof *o);
    }
    st->count = first;
}

void *dlopen(const char *path, int flags) {
    dl_error = 0;
    struct dl_state *st = __pagh_dl_state;
    if (!st) { dl_error_set("no loader state (start the program with ld-pagh)"); return 0; }
    if (!path || !*path) { dl_error_set("dlopen: empty path"); return 0; }

    int global = (flags & RTLD_GLOBAL) != 0;
    int first = st->count;
    int idx = open_object(st, path, first, global, 0);
    if (idx < 0) { unload_group(st, first); return 0; }
    if (idx < first) return &st->objs[idx];  /* already loaded: nothing to do */

    for (int i = first; i < st->count; i++)
        if (relocate(&st->objs[i], st, i) < 0) { unload_group(st, first); return 0; }
    for (int i = first; i < st->count; i++)
        if (protect_object(&st->objs[i]) < 0) { unload_group(st, first); return 0; }
    /* Dependencies first, the object that pulled them in last. */
    for (int i = st->count - 1; i >= first; i--) call_inits(&st->objs[i]);
    return &st->objs[idx];
}

void *dlsym(void *handle, const char *name) {
    dl_error = 0;
    struct dl_state *st = __pagh_dl_state;
    if (!st) { dl_error_set("no loader state (start the program with ld-pagh)"); return 0; }
    if (!name || !*name) { dl_error_set("dlsym: empty name"); return 0; }

    if (handle == RTLD_DEFAULT) {
        unsigned long addr = resolve_global(st, name);
        if (!addr) { dl_error_set1("undefined symbol %s", name); return 0; }
        return (void *)addr;
    }
    if (handle == RTLD_NEXT) {
        /* The caller's object is identified from this function's return
         * address.  dlsym() makes calls, so the compiler has spilled ra. */
        void *ra = __builtin_extract_return_addr(__builtin_return_address(0));
        int from = object_containing(st, ra);
        if (from < 0) {
            dl_error_set("dlsym(RTLD_NEXT): caller is not a loaded object");
            return 0;
        }
        unsigned long addr = resolve_next(st, from, name);
        if (!addr) { dl_error_set1("undefined symbol %s", name); return 0; }
        return (void *)addr;
    }

    int idx = object_index(st, handle);
    if (idx < 0) { dl_error_set("dlsym: invalid handle"); return 0; }
    int group = st->objs[idx].group;
    for (int i = 0; i < st->count; i++) {
        if (st->objs[i].group != group) continue;
        unsigned long addr = resolve_in(&st->objs[i], name);
        if (addr) return (void *)addr;
    }
    dl_error_set1("undefined symbol %s", name);
    return 0;
}

int dlclose(void *handle) {
    (void)handle;
    return 0;
}

char *dlerror(void) {
    const char *e = dl_error;
    dl_error = 0;
    return (char *)e;
}

int __pagh_dl_run(int argc, char **argv, char **envp) {
    if (argc < 1 || !argv || !argv[0]) {
        printf("ld-pagh: usage: ld-pagh <program> [args...]\n");
        return 2;
    }
    struct dl_state *st = &dl_state;
    memset(st, 0, sizeof *st);
    __pagh_dl_state = st;
    dl_error = 0;

    /* Group 0 is the program: it and its DT_NEEDED tree go into the global
     * scope, exactly like the main executable of a real dynamic link. */
    if (open_object(st, argv[0], 0, 1, 0) != 0) goto fail;
    struct dl_object *exe = &st->objs[0];

    for (int i = 0; i < st->count; i++)
        if (relocate(&st->objs[i], st, i) < 0) goto fail;
    for (int i = 0; i < st->count; i++)
        if (protect_object(&st->objs[i]) < 0) goto fail;

    /* Publish this table into whichever loaded object exports the variable
     * (libpagh-c.so in a dynamically linked program).  Doing it before the
     * constructors means a constructor may legally call dlopen. */
    struct dl_state **slot = (struct dl_state **)resolve_global(st, "__pagh_dl_state");
    if (slot) *slot = st;

    for (int i = st->count - 1; i >= 0; i--) call_inits(&st->objs[i]);
    if (exe->eh->entry >= exe->total) { dl_error_set("entry point outside the image"); goto fail; }
    int (*entry)(int, char **, char **) =
        (int (*)(int, char **, char **))((unsigned long)exe->base + exe->eh->entry);
    return entry(argc, argv, envp);

fail:
    printf("ld-pagh: %s\n", dl_error ? dl_error : "load failed");
    return 1;
}
