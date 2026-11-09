/* Negative control for RTLD_LOCAL: dep_answer() exists only in
 * libplugdep.so, which was loaded RTLD_LOCAL as part of libplug's group.  A
 * later dlopen must therefore fail with "unresolved symbol dep_answer"
 * instead of reaching into that group. */
extern int dep_answer(void);

int late_bad_value(void) { return dep_answer(); }
