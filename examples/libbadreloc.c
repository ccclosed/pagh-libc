/* Negative control for relocation handling: build.sh re-tags this object's
 * one data relocation to the unassigned type 999, so dlopen must reject it
 * with "unsupported relocation type 999" instead of writing garbage.  The
 * symbol it names (extra_global) is resolvable, so the type is the only thing
 * that can fail. */
extern int extra_global;

int bad_reloc_value(void) { return extra_global; }
