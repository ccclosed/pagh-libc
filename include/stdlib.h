#ifndef PAGH_STDLIB_H
#define PAGH_STDLIB_H
#include <stddef.h>
void *malloc(size_t size);
void *calloc(size_t n, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void exit(int code) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int atexit(void (*fn)(void));
int atoi(const char *s);
long atol(const char *s);
int abs(int x);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
extern char **environ;
char *getenv(const char *name);
#endif
