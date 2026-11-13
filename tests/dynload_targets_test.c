/* Host test for the F-r2-libc-1 call-target validation in ld-pagh.
 *
 * The loader's constructor/fini validation is pure address arithmetic over the
 * ET_DYN program headers and the mapped image: it never needs a syscall.  This
 * test therefore compiles src/dynload.c for the host (the static functions are
 * visible through the include) and calls it against synthetic objects:
 *
 *   - DT_INIT/DT_FINI must be file-backed executable code of the same object
 *     (not a bss-only exec tail, not the writable data segment, not an
 *     arbitrary 64-bit value);
 *   - DT_INIT_ARRAY/DT_FINI_ARRAY must be a writable, non-executable,
 *     file-backed data table, and every non-zero entry must point at such
 *     executable code;
 *   - a relocation against an SHN_ABS symbol may only store an address inside
 *     the defining object's image (the arms the audit PoCs used to plant
 *     0x4141414141414141 in the array without knowing the load base).
 *
 * The live end-to-end proof is tools/live-security.py; this is the fast inner
 * loop and covers shapes that are awkward to boot for (fini tables, bss tails,
 * missing DT_*_ARRAYSZ, defined-symbol values outside the image).
 */

#include "../src/dynload.c"

/* The loader's syscall entry points are unreachable from these pure checks;
 * provide stubs so the test links on the host.  __pagh_abs is likewise only
 * used by file loading, which this test does not perform. */
long __pagh_syscall3(long nr, long a0, long a1, long a2) {
    (void)nr; (void)a0; (void)a1; (void)a2;
    return -1;
}
long __pagh_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    (void)nr; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return -1;
}
const char *__pagh_abs(const char *path, char *buf, unsigned long size) {
    snprintf(buf, size, "%s", path ? path : "");
    return buf;
}

#define TEST_TOTAL 0x400UL
#define PHOFF 64UL
#define PHENTSIZE 56UL
#define SHOFF 0x280UL
#define SHENTSIZE 64UL

static unsigned char file_data[0x600] __attribute__((aligned(16)));
static unsigned char image_data[0x600] __attribute__((aligned(16)));
static struct dl_object obj;
static struct dl_state state;
static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void put_u64(unsigned char *buf, unsigned long off, unsigned long value) {
    memcpy(buf + off, &value, sizeof value);
}

static void put_phdr(int i, unsigned int type, unsigned int flags,
                     unsigned long off, unsigned long vaddr,
                     unsigned long filesz, unsigned long memsz) {
    struct phdr *p = (struct phdr *)(file_data + PHOFF + (unsigned long)i * PHENTSIZE);
    memset(p, 0, sizeof *p);
    p->type = type;
    p->flags = flags;
    p->off = off;
    p->vaddr = vaddr;
    p->filesz = filesz;
    p->memsz = memsz;
    p->align = 0x1000;
}

static void put_shdr(int i, unsigned int type, unsigned long addr,
                     unsigned long off, unsigned long size,
                     unsigned int link, unsigned int info) {
    struct shdr *s = (struct shdr *)(file_data + SHOFF + (unsigned long)i * SHENTSIZE);
    memset(s, 0, sizeof *s);
    s->type = type;
    s->addr = addr;
    s->off = off;
    s->size = size;
    s->link = link;
    s->info = info;
    s->addralign = 8;
    s->entsize = type == SHT_DYNSYM ? sizeof(struct sym) : type == SHT_RELA ? sizeof(struct rela) : 0;
}

static void put_dyn(int slot, unsigned long tag, unsigned long value) {
    put_u64(image_data, 0x200 + (unsigned long)slot * 16, tag);
    put_u64(image_data, 0x200 + (unsigned long)slot * 16 + 8, value);
}

/* Two PT_LOADs: executable [0, 0x100) with a 0x100-byte zero-filled bss tail
 * up to 0x200, and writable data [0x200, 0x400).  PT_DYNAMIC sits in the data
 * segment.  File offsets equal virtual addresses, so the fixture can place a
 * section at the same number in both views. */
static void reset_object(void) {
    memset(file_data, 0, sizeof file_data);
    memset(image_data, 0, sizeof image_data);
    memset(&obj, 0, sizeof obj);
    memset(&state, 0, sizeof state);

    struct ehdr *eh = (struct ehdr *)file_data;
    eh->phoff = PHOFF;
    eh->phentsize = PHENTSIZE;
    eh->phnum = 3;
    eh->shoff = SHOFF;
    eh->shentsize = SHENTSIZE;
    eh->shnum = 0;

    put_phdr(0, PT_LOAD, PF_X, 0, 0, 0x100, 0x200);
    put_phdr(1, PT_LOAD, PF_W, 0x200, 0x200, 0x200, 0x200);
    put_phdr(2, PT_DYNAMIC, PF_W, 0x200, 0x200, 0x30, 0x30);

    memcpy(image_data, file_data, sizeof file_data);
    put_dyn(0, DT_NULL, 0);

    obj.data = file_data;
    obj.size = TEST_TOTAL;
    obj.base = image_data;
    obj.total = TEST_TOTAL;
    obj.eh = (struct ehdr *)file_data;
    obj.path = "synthetic-object";
}

static unsigned long image_base(void) {
    return (unsigned long)image_data;
}

static int refused(const char *needle) {
    const char *error;
    if (validate_call_targets(&obj) >= 0) {
        printf("NOTE: expected a refusal (%s), validation accepted the object\n", needle);
        return 0;
    }
    error = dlerror();
    if (!error) {
        printf("NOTE: refused without dlerror (%s)\n", needle);
        return 0;
    }
    if (!strstr(error, needle)) {
        printf("NOTE: refused with %s\n", error);
        return 0;
    }
    return 1;
}

static void test_ok_cases(void) {
    reset_object();
    CHECK(validate_call_targets(&obj) == 0);

    reset_object();
    put_dyn(0, DT_INIT, 0x40);
    put_dyn(1, DT_NULL, 0);
    CHECK(validate_call_targets(&obj) == 0);

    reset_object();
    put_dyn(0, DT_INIT_ARRAY, 0x240);
    put_dyn(1, DT_INIT_ARRAYSZ, 8);
    put_dyn(2, DT_NULL, 0);
    put_u64(image_data, 0x240, image_base() + 0x40);
    CHECK(validate_call_targets(&obj) == 0);

    /* A zero entry is an empty slot, not an address to call. */
    reset_object();
    put_dyn(0, DT_INIT_ARRAY, 0x240);
    put_dyn(1, DT_INIT_ARRAYSZ, 16);
    put_dyn(2, DT_NULL, 0);
    put_u64(image_data, 0x240, 0);
    put_u64(image_data, 0x248, image_base() + 0x40);
    CHECK(validate_call_targets(&obj) == 0);

    /* A present-but-empty table is harmless. */
    reset_object();
    put_dyn(0, DT_INIT_ARRAY, 0x240);
    put_dyn(1, DT_INIT_ARRAYSZ, 0);
    put_dyn(2, DT_NULL, 0);
    CHECK(validate_call_targets(&obj) == 0);
}

static void test_init_refused(void) {
    reset_object();
    put_dyn(0, DT_INIT, 0x4141414141414141UL);
    put_dyn(1, DT_NULL, 0);
    CHECK(refused("unsafe DT_INIT"));

    reset_object();
    put_dyn(0, DT_INIT, 0x240); /* writable data, not executable */
    put_dyn(1, DT_NULL, 0);
    CHECK(refused("unsafe DT_INIT"));

    reset_object();
    put_dyn(0, DT_INIT, 0x1f0); /* executable bss tail, not file-backed */
    put_dyn(1, DT_NULL, 0);
    CHECK(refused("unsafe DT_INIT"));

    reset_object();
    put_dyn(0, DT_INIT, 0x41); /* unaligned RISC-V instruction */
    put_dyn(1, DT_NULL, 0);
    CHECK(refused("unsafe DT_INIT"));

    reset_object();
    put_dyn(0, DT_FINI, 0x4141414141414141UL);
    put_dyn(1, DT_NULL, 0);
    CHECK(refused("unsafe DT_FINI"));
}

static void test_array_refused(void) {
    reset_object();
    put_dyn(0, DT_INIT_ARRAY, 0x240);
    put_dyn(1, DT_INIT_ARRAYSZ, 8);
    put_dyn(2, DT_NULL, 0);
    put_u64(image_data, 0x240, 0x4141414141414140UL);
    CHECK(refused("unsafe DT_INIT_ARRAY"));

    /* The audit's evil_call_rel shape: the table itself sits in the executable
     * segment, so a relative relocation can plant an in-image code offset. */
    reset_object();
    put_dyn(0, DT_INIT_ARRAY, 0x40);
    put_dyn(1, DT_INIT_ARRAYSZ, 8);
    put_dyn(2, DT_NULL, 0);
    put_u64(image_data, 0x40, image_base() + 0x50);
    CHECK(refused("unsafe DT_INIT_ARRAY"));

    reset_object();
    put_dyn(0, DT_INIT_ARRAY, 0x240); /* table without its size tag */
    put_dyn(1, DT_NULL, 0);
    CHECK(refused("unsafe DT_INIT_ARRAY"));

    reset_object();
    put_dyn(0, DT_INIT_ARRAYSZ, 8); /* size without a table */
    put_dyn(1, DT_NULL, 0);
    CHECK(refused("unsafe DT_INIT_ARRAY"));

    reset_object();
    put_dyn(0, DT_INIT_ARRAY, 0x240);
    put_dyn(1, DT_INIT_ARRAYSZ, 7); /* not a whole pointer */
    put_dyn(2, DT_NULL, 0);
    CHECK(refused("unsafe DT_INIT_ARRAY"));

    reset_object();
    put_dyn(0, DT_INIT_ARRAY, 0x3f8); /* table runs past the image */
    put_dyn(1, DT_INIT_ARRAYSZ, 16);
    put_dyn(2, DT_NULL, 0);
    CHECK(refused("unsafe DT_INIT_ARRAY"));

    reset_object();
    put_dyn(0, DT_FINI_ARRAY, 0x240);
    put_dyn(1, DT_FINI_ARRAYSZ, 8);
    put_dyn(2, DT_NULL, 0);
    put_u64(image_data, 0x240, image_base() + 0x240); /* data, not code */
    CHECK(refused("unsafe DT_FINI_ARRAY"));
}

/* Relocation fixture: one executable and one writable segment plus a
 * .dynsym/.dynstr/.rela set.  The single symbol is SHN_ABS (or a defined
 * section symbol, for the out-of-image value case) and the relocation stores
 * it into a writable slot, the same write-what the audit used to fill
 * .init_array. */
static void reset_reloc(void) {
    reset_object();
    struct ehdr *eh = (struct ehdr *)file_data;
    eh->shnum = 3;
    put_shdr(0, SHT_DYNSYM, 0x180, 0x180, 2 * sizeof(struct sym), 1, 0);
    put_shdr(1, 3 /* SHT_STRTAB */, 0x1c0, 0x1c0, 8, 0, 0);
    put_shdr(2, SHT_RELA, 0x200, 0x200, sizeof(struct rela), 0, 0);

    struct sym *s = (struct sym *)(file_data + 0x180);
    memset(s, 0, 2 * sizeof(struct sym));
    s[1].name = 1;
    s[1].info = 0x11; /* STB_GLOBAL | STT_OBJECT */
    s[1].shndx = SHN_ABS;
    s[1].size = 8;
    memcpy(file_data + 0x1c0, "\0abs\0", 5);

    struct rela *r = (struct rela *)(file_data + 0x200);
    r->off = 0x240;
    r->info = ((unsigned long)1 << 32) | R_RISCV_64;
    r->addend = 0;

    /* dynsym_view() reads the symbol table from the mapped image, so mirror
     * the finished file into the image view. */
    memcpy(image_data, file_data, sizeof file_data);
    obj.sh = (struct shdr *)(file_data + SHOFF);
}

static void set_abs_symbol(unsigned short shndx, unsigned long value) {
    struct sym *s = (struct sym *)(image_data + 0x180);
    s[1].shndx = shndx;
    s[1].value = value;
}

static void test_relocations(void) {
    /* SHN_ABS outside the object image is refused. */
    reset_reloc();
    set_abs_symbol(SHN_ABS, 0x4141414141414141UL);
    CHECK(relocate(&obj, &state, 0) < 0);
    {
        const char *error = dlerror();
        CHECK(error && strstr(error, "SHN_ABS symbol outside the image"));
    }

    /* SHN_ABS inside the image is the same rule resolve_in() applies: accept
     * it and store the address. */
    reset_reloc();
    set_abs_symbol(SHN_ABS, image_base() + 0x40);
    CHECK(relocate(&obj, &state, 0) == 0);
    CHECK(*(unsigned long *)(image_data + 0x240) == image_base() + 0x40);

    /* A non-ABS (section-relative) symbol value outside the image is equally
     * malformed: the loader would otherwise add a file-chosen offset to the
     * base and store base + value. */
    reset_reloc();
    set_abs_symbol(1, 0x100000UL);
    CHECK(relocate(&obj, &state, 0) < 0);
    {
        const char *error = dlerror();
        CHECK(error && strstr(error, "defined symbol outside the image"));
    }

    /* In-image section-relative values still work (the normal ET_DYN shape). */
    reset_reloc();
    set_abs_symbol(1, 0x40);
    CHECK(relocate(&obj, &state, 0) == 0);
    CHECK(*(unsigned long *)(image_data + 0x240) == image_base() + 0x40);
}

int main(void) {
    test_ok_cases();
    test_init_refused();
    test_array_refused();
    test_relocations();
    if (failures) {
        printf("dynload_targets_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("dynload_targets_test: ok (init/fini targets, tables, SHN_ABS relocation)\n");
    return 0;
}