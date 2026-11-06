#ifndef PAGH_STDIO_H
#define PAGH_STDIO_H
#include <stddef.h>
#include <stdarg.h>
#define EOF (-1)
int printf(const char *fmt, ...);
int fprintf(int fd, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int puts(const char *s);
int fputs(const char *s, int fd);
int putchar(int c);
int getchar(void);
#endif
