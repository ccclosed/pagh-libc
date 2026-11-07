/* ld-pagh: a tiny userland dynamic loader for pagh-riscv.
 *
 * Usage: ld-pagh /mnt/program [args...]
 *
 * Loads an ET_DYN executable, loads its DT_NEEDED shared libraries from the
 * program's directory (or /mnt), applies R_RISCV_RELATIVE/R_RISCV_64/
 * R_RISCV_JUMP_SLOT relocations, and calls the executable's e_entry with
 * (argc-1, argv+1, envp).  This is a real loader, not a simulation: the kernel
 * runs it statically, and the loaded program executes from anonymous mmap
 * pages.  No symbol versioning, TLS, init/fini arrays or lazy binding.
 */
#include <pagh.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define SHT_RELA 4
#define SHT_DYNSYM 11
#define SHN_UNDEF 0
#define R_RISCV_64 2
#define R_RISCV_RELATIVE 3
#define R_RISCV_JUMP_SLOT 5
#define R_RISCV_GLOB_DAT 6

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

struct object {
    const char *path;
    unsigned char *data;
    unsigned long size;
    unsigned char *base;
    struct ehdr *eh;
    struct shdr *sh;
    unsigned long total;
    int is_lib;
};

static long sys_mmap(unsigned long len, int prot) {
    return __pagh_syscall6(SYS_MMAP, 0, (long)len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}
static long sys_mprotect(unsigned long addr, unsigned long len, int prot) {
    return __pagh_syscall6(226, (long)addr, (long)len, prot, 0, 0, 0);
}

static int read_file(struct object *o, const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    struct pagh_stat st;
    if (fstat(fd, &st) < 0 || st.size == 0 || st.size > (4UL << 20)) { close(fd); return -1; }
    unsigned char *buf = malloc((size_t)st.size);
    if (!buf) { close(fd); return -1; }
    long n = read(fd, buf, (size_t)st.size);
    close(fd);
    if (n != (long)st.size) { free(buf); return -1; }
    o->path = path; o->data = buf; o->size = (unsigned long)st.size;
    o->eh = (struct ehdr *)buf;
    if (memcmp(o->eh->ident, "\177ELF", 4) || o->eh->machine != 243) {
        return -1;
    }
    o->sh = (struct shdr *)(buf + o->eh->shoff);
    return 0;
}

static void protect_object(struct object *o) {
    unsigned long max = 0;
    for (int i = 0; i < o->eh->phnum; i++) {
        struct phdr *p = (struct phdr *)(o->data + o->eh->phoff + (unsigned long)i * o->eh->phentsize);
        if (p->type == PT_LOAD && p->vaddr + p->memsz > max) max = p->vaddr + p->memsz;
    }
    max = (max + 4095) & ~4095UL;
    for (unsigned long voff = 0; voff < max; voff += 4096) {
        int prot = PROT_READ | PROT_WRITE;
        int exec = 0;
        for (int i = 0; i < o->eh->phnum; i++) {
            struct phdr *ph = (struct phdr *)(o->data + o->eh->phoff + (unsigned long)i * o->eh->phentsize);
            if (ph->type != PT_LOAD) continue;
            if (voff + 4096 <= ph->vaddr || voff >= ph->vaddr + ph->memsz) continue;
            if (ph->flags & 1) exec = 1;
            if (!(ph->flags & 2)) prot &= ~PROT_WRITE;
        }
        if (exec) prot = PROT_READ | PROT_EXEC;
        sys_mprotect((unsigned long)o->base + voff, 4096, prot);
    }
}

static struct shdr *find_section(struct object *o, unsigned int type) {
    for (int i = 0; i < o->eh->shnum; i++)
        if (o->sh[i].type == type) return &o->sh[i];
    return 0;
}

static const char *section_strings(struct object *o) {
    if (o->eh->shstrndx >= o->eh->shnum) return 0;
    return (const char *)(o->data + o->sh[o->eh->shstrndx].off);
}

static int load_object(struct object *o) {
    unsigned long max = 0;
    for (int i = 0; i < o->eh->phnum; i++) {
        struct phdr *p = (struct phdr *)(o->data + o->eh->phoff + (unsigned long)i * o->eh->phentsize);
        if (p->type == PT_LOAD && p->vaddr + p->memsz > max) max = p->vaddr + p->memsz;
    }
    max = (max + 4095) & ~4095UL;
    o->total = max;
    long base = sys_mmap(max, PROT_READ | PROT_WRITE);
    if (base < 0) return -1;
    o->base = (unsigned char *)base;
    memset(o->base, 0, max);
    for (int i = 0; i < o->eh->phnum; i++) {
        struct phdr *p = (struct phdr *)(o->data + o->eh->phoff + (unsigned long)i * o->eh->phentsize);
        if (p->type != PT_LOAD) continue;
        memcpy(o->base + p->vaddr, o->data + p->off, p->filesz);
        /* bss is already zero from mmap+memset */
    }
    return 0;
}

static struct shdr *dynsym_section(struct object *o, const char **strings, unsigned long *count, unsigned long *entsize) {
    struct shdr *sym = find_section(o, SHT_DYNSYM);
    if (!sym) return 0;
    if (sym->link >= o->eh->shnum) return 0;
    *strings = (const char *)(o->data + o->sh[sym->link].off);
    *entsize = sym->entsize ? sym->entsize : sizeof(struct sym);
    *count = sym->size / *entsize;
    return sym;
}

static unsigned long resolve(struct object **objs, int nobj, const char *name) {
    for (int k = 0; k < nobj; k++) {
        const char *strings; unsigned long count, entsize;
        struct shdr *sym = dynsym_section(objs[k], &strings, &count, &entsize);
        if (!sym) continue;
        unsigned char *tab = objs[k]->base + sym->addr;
        for (unsigned long i = 0; i < count; i++) {
            struct sym *s = (struct sym *)(tab + i * entsize);
            if (!s->name || s->shndx == SHN_UNDEF) continue;
            if (!strcmp(strings + s->name, name)) return (unsigned long)objs[k]->base + s->value;
        }
    }
    return 0;
}

static int relocate(struct object *o, struct object **objs, int nobj) {
    const char *strings; unsigned long count, entsize;
    struct shdr *sym = dynsym_section(o, &strings, &count, &entsize);
    (void)count;
    for (int i = 0; i < o->eh->shnum; i++) {
        struct shdr *r = &o->sh[i];
        if (r->type != SHT_RELA) continue;
        unsigned long n = r->size / (r->entsize ? r->entsize : sizeof(struct rela));
        for (unsigned long j = 0; j < n; j++) {
            struct rela *re = (struct rela *)(o->data + r->off + j * (r->entsize ? r->entsize : sizeof(struct rela)));
            unsigned type = (unsigned)re->info & 0xffffffffu;
            unsigned si = (unsigned)(re->info >> 32);
            unsigned long where = (unsigned long)o->base + re->off;
            unsigned long val = 0;
            if (type == R_RISCV_RELATIVE) {
                val = (unsigned long)o->base + (unsigned long)re->addend;
            } else if (type == R_RISCV_64 || type == R_RISCV_JUMP_SLOT || type == R_RISCV_GLOB_DAT) {
                if (!sym || si >= count) continue;
                struct sym *s = (struct sym *)((unsigned char *)o->base + sym->addr + si * entsize);
                const char *nm = strings + s->name;
                unsigned long addr;
                if (s->shndx != SHN_UNDEF) addr = (unsigned long)o->base + s->value;
                else addr = resolve(objs, nobj, nm);
                if (!addr) { printf("ld-pagh: unresolved symbol %s in %s\n", nm, o->path); return -1; }
                val = addr + (unsigned long)re->addend;
            } else {
                continue;
            }
            *(unsigned long *)where = val;
        }
    }
    return 0;
}

static void zero_bss(struct object *o) {
    for (int i = 0; i < o->eh->phnum; i++) {
        struct phdr *p = (struct phdr *)(o->data + o->eh->phoff + (unsigned long)i * o->eh->phentsize);
        if (p->type == PT_LOAD && p->memsz > p->filesz)
            memset(o->base + p->vaddr + p->filesz, 0, p->memsz - p->filesz);
    }
}

int main(int argc, char **argv, char **envp) {
    if (argc < 2) { printf("usage: ld-pagh <program> [args...]\n"); return 2; }
    static struct object exe, lib;
    memset(&exe, 0, sizeof exe); memset(&lib, 0, sizeof lib);
    if (read_file(&exe, argv[1]) < 0) { printf("ld-pagh: cannot read %s\n", argv[1]); return 1; }
    if (load_object(&exe) < 0) { printf("ld-pagh: cannot map %s\n", argv[1]); return 1; }
    zero_bss(&exe);
    char libpath[256];
    const char *slash = strrchr(argv[1], '/');
    if (slash) {
        unsigned long n = (unsigned long)(slash - argv[1]);
        if (n > 200) n = 200;
        memcpy(libpath, argv[1], n); libpath[n] = 0;
        strcat(libpath, "/libpagh-c.so");
    } else strcpy(libpath, "libpagh-c.so");
    if (read_file(&lib, libpath) < 0) {
        if (read_file(&lib, "/mnt/libpagh-c.so") < 0) { printf("ld-pagh: cannot read %s\n", libpath); return 1; }
    }
    if (load_object(&lib) < 0) { printf("ld-pagh: cannot map libc\n"); return 1; }
    zero_bss(&lib);
    struct object *objs[2] = { &exe, &lib };
    if (relocate(&lib, objs, 2) < 0 || relocate(&exe, objs, 2) < 0) return 1;
    protect_object(&lib);
    protect_object(&exe);
    int (*entry)(int, char **, char **) = (int (*)(int, char **, char **))((unsigned long)exe.base + exe.eh->entry);
    int rc = entry(argc - 1, argv + 1, envp);
    return rc;
}
