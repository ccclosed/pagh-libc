#ifndef PAGH_SYS_STAT_H
#define PAGH_SYS_STAT_H
#include <stdint.h>
#include <pagh.h>
typedef struct pagh_stat stat_t;
int stat(const char *path, struct pagh_stat *st);
int fstat(int fd, struct pagh_stat *st);
#define S_IFMT 0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IFLNK 0120000
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
