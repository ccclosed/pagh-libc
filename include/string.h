#ifndef PAGH_STRING_H
#define PAGH_STRING_H
#include <stddef.h>
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
char *strdup(const char *s);
/* Iconv-free, locale-free standard C helpers. `strtok_r` and `strsep` are the
 * reentrant tokenizers (the former skips runs of delimiters, the latter
 * returns empty tokens for adjacent delimiters). */
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strtok_r(char *restrict s, const char *restrict delim, char **restrict saveptr);
char *strsep(char **stringp, const char *delim);
/* Text for an errno value from <errno.h>; "unknown error" for anything else.
 * The returned string is static. */
const char *strerror(int err);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
#endif
