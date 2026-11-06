#ifndef PAGH_INTERNAL_H
#define PAGH_INTERNAL_H
/* Resolve a relative path against the process-local cwd (no kernel chdir). */
const char *__pagh_abs(const char *path, char *buf, unsigned long size);
extern char __pagh_cwd[256];
#endif
