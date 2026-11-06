#include <pagh.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

static void out_char(char *buf, size_t size, size_t *pos, char c) {
    if (*pos + 1 < size) buf[*pos] = c;
    (*pos)++;
}
static void out_str(char *buf, size_t size, size_t *pos, const char *s) {
    while (*s) out_char(buf, size, pos, *s++);
}
static void out_uint(char *buf, size_t size, size_t *pos, unsigned long v, int base, int upper, int width, int zero) {
    char tmp[32]; int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % base]; v /= base; }
    for (int i = n; i < width; i++) out_char(buf, size, pos, zero ? '0' : ' ');
    while (n) out_char(buf, size, pos, tmp[--n]);
}
static void out_int(char *buf, size_t size, size_t *pos, long v, int width, int zero) {
    if (v < 0) { out_char(buf, size, pos, '-'); v = -v; }
    out_uint(buf, size, pos, (unsigned long)v, 10, 0, width, zero);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    size_t pos = 0;
    while (*fmt) {
        if (*fmt != '%') { out_char(buf, size, &pos, *fmt++); continue; }
        fmt++;
        int zero = 0, width = 0, precision = -1;
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt++ - '0'); }
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') { precision = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') { precision = precision * 10 + (*fmt++ - '0'); }
        }
        int lng = 0;
        if (*fmt == 'l') { lng = 1; fmt++; if (*fmt == 'l') { lng = 2; fmt++; } }
        switch (*fmt++) {
        case 's': {
            const char *sv = va_arg(ap, const char *);
            if (!sv) sv = "(null)";
            if (precision >= 0) {
                for (int i = 0; i < precision && sv[i]; i++) out_char(buf, size, &pos, sv[i]);
            } else {
                out_str(buf, size, &pos, sv);
            }
            break;
        }
        case 'c': out_char(buf, size, &pos, (char)va_arg(ap, int)); break;
        case 'd': case 'i':
            out_int(buf, size, &pos, lng ? va_arg(ap, long) : (long)va_arg(ap, int), width, zero);
            break;
        case 'u':
            out_uint(buf, size, &pos, lng ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int), 10, 0, width, zero);
            break;
        case 'x':
            out_uint(buf, size, &pos, lng ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int), 16, 0, width, zero);
            break;
        case 'X':
            out_uint(buf, size, &pos, lng ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int), 16, 1, width, zero);
            break;
        case 'o':
            out_uint(buf, size, &pos, lng ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int), 8, 0, width, zero);
            break;
        case 'p':
            out_str(buf, size, &pos, "0x");
            out_uint(buf, size, &pos, (unsigned long)va_arg(ap, void *), 16, 0, 0, 0);
            break;
        case '%': out_char(buf, size, &pos, '%'); break;
        default: out_char(buf, size, &pos, '%'); out_char(buf, size, &pos, *(fmt-1)); break;
        }
    }
    if (size) buf[pos < size ? pos : size - 1] = 0;
    return (int)pos;
}

int printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) write(1, buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
    return n;
}
int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
int puts(const char *s) { write(1, s, strlen(s)); write(1, "\n", 1); return 0; }
int putchar(int c) { char ch = (char)c; write(1, &ch, 1); return c; }
int getchar(void) { char c; return read(0, &c, 1) == 1 ? (unsigned char)c : -1; }
