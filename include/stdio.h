#ifndef PAGH_STDIO_H
#define PAGH_STDIO_H
#include <stddef.h>
#include <stdarg.h>
#define EOF (-1)
typedef struct PaghFile FILE;
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
int fflush(FILE *f);
size_t fread(void *buf, size_t size, size_t n, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t n, FILE *f);
char *fgets(char *buf, int size, FILE *f);
int fputc(int c, FILE *f);
int fgetc(FILE *f);
int fputs(const char *s, FILE *f);
int fprintf(FILE *f, const char *fmt, ...);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int fdprintf(int fd, const char *fmt, ...);
int printf(const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);
int getchar(void);
/* `s` may be NULL; both write "<s>: <errno text>\n" (or just the text) to
 * stderr and return nothing, like every other small libc. */
void perror(const char *s);
#endif
