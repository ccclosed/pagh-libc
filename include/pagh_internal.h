#ifndef PAGH_INTERNAL_H
#define PAGH_INTERNAL_H
/* Resolve a relative path against the process-local cwd (no kernel chdir). */
const char *__pagh_abs(const char *path, char *buf, unsigned long size);
extern char __pagh_cwd[256];

/* Load an ET_DYN program plus its DT_NEEDED libraries and jump to its entry.
 * argc/argv are already shifted: argv[0] is the program path and the rest are
 * the program's arguments (the same shape its main() expects). */
int __pagh_dl_run(int argc, char **argv, char **envp);
#endif
