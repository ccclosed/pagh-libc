#include <pagh.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <pagh_internal.h>

static void out_char(char *buf, size_t size, size_t *pos, char c) {
    if (*pos + 1 < size) buf[*pos] = c;
    (*pos)++;
}
static void out_str(char *buf, size_t size, size_t *pos, const char *s) {
    while (*s) out_char(buf, size, pos, *s++);
}

/* Emit `count` copies of `c`.  Same truncation contract as out_char(): only
 * bytes with index < size-1 are stored, and the position keeps counting so
 * snprintf's return value is still the untruncated length.  Filling the run in
 * one go matters: the width and precision come from the format or from an
 * argument, and an attacker-controlled `%.2147483647d` must not turn into a
 * per-byte loop. */
static void out_fill(char *buf, size_t size, size_t *pos, char c, unsigned long count) {
    size_t p = *pos;
    if (count && size >= 2 && p < size - 1) {
        size_t n = count < size - 1 - p ? (size_t)count : size - 1 - p;
        memset(buf + p, c, n);
    }
    *pos = p + count;
}

/* Parse a decimal field width or precision.  Saturates at INT_MAX instead of
 * overflowing (signed overflow is undefined behaviour, and "%.9999999999d" is
 * a valid format string). */
static int parse_dec(const char **p) {
    int v = 0;
    while (**p >= '0' && **p <= '9') {
        int d = **p - '0';
        if (v > (2147483647 - d) / 10) v = 2147483647;
        else v = v * 10 + d;
        (*p)++;
    }
    return v;
}

/* Unsigned value to `out`, most significant digit first; returns the length. */
static int utoa(char *out, unsigned long v, int base, int upper) {
    char tmp[32];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % base]; v /= base; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

/* Emit `prefix` + `zeros` zero digits + `text` with width/padding.  The
 * precision zero-extension is emitted straight into the output instead of
 * being inserted into the caller's stack buffer ("%.128d" needs 128 digits; a
 * fixed 32-byte digit array is where the old code overran the stack). Zero
 * padding from the width flag goes between the prefix and the digits
 * ("-00012", "0x00ff"), space padding in front. */
static void out_field(char *buf, size_t size, size_t *pos, const char *prefix, int plen,
                      int zeros, const char *text, int len, int width, int zero, int left) {
    unsigned long total = (unsigned long)plen + (unsigned long)zeros + (unsigned long)len;
    unsigned long pad = width > 0 && (unsigned long)width > total ? (unsigned long)width - total : 0;
    if (left) {
        for (int i = 0; i < plen; i++) out_char(buf, size, pos, prefix[i]);
        out_fill(buf, size, pos, '0', (unsigned long)zeros);
        for (int i = 0; i < len; i++) out_char(buf, size, pos, text[i]);
        out_fill(buf, size, pos, ' ', pad);
    } else if (zero) {
        for (int i = 0; i < plen; i++) out_char(buf, size, pos, prefix[i]);
        out_fill(buf, size, pos, '0', pad);
        out_fill(buf, size, pos, '0', (unsigned long)zeros);
        for (int i = 0; i < len; i++) out_char(buf, size, pos, text[i]);
    } else {
        out_fill(buf, size, pos, ' ', pad);
        for (int i = 0; i < plen; i++) out_char(buf, size, pos, prefix[i]);
        out_fill(buf, size, pos, '0', (unsigned long)zeros);
        for (int i = 0; i < len; i++) out_char(buf, size, pos, text[i]);
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    size_t pos = 0;
    while (*fmt) {
        if (*fmt != '%') { out_char(buf, size, &pos, *fmt++); continue; }
        fmt++;
        int left = 0, plus = 0, space = 0, alt = 0, zero = 0, width = 0;
        int precision = -1, longs = 0, half = 0, longdouble = 0;
        for (;;) {
            if (*fmt == '-') { left = 1; fmt++; }
            else if (*fmt == '+') { plus = 1; fmt++; }
            else if (*fmt == ' ') { space = 1; fmt++; }
            else if (*fmt == '#') { alt = 1; fmt++; }
            else if (*fmt == '0') { zero = 1; fmt++; }
            else break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
            /* -INT_MIN is not representable; saturate instead of overflowing. */
            if (width < 0) { left = 1; width = width == (-2147483647 - 1) ? 2147483647 : -width; }
        } else {
            width = parse_dec(&fmt);
        }
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                fmt++;
                if (precision < 0) precision = -1;
            } else {
                precision = parse_dec(&fmt);
            }
        }
        if (*fmt == 'l') { longs = 1; fmt++; if (*fmt == 'l') { longs = 2; fmt++; } }
        else if (*fmt == 'h') { half = 1; fmt++; if (*fmt == 'h') { half = 2; fmt++; } }
        else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { longs = 1; fmt++; }
        else if (*fmt == 'L') { longdouble = 1; fmt++; }
        if (left) zero = 0;

        /* The conversion character is captured before the switch advances the
         * pointer: reading *fmt inside a case would read the *next* character
         * (that is how %lu silently became %lx). */
        char conv = *fmt;
        fmt++;
        switch (conv) {
        case 's': {
            const char *sv = va_arg(ap, const char *);
            if (!sv) sv = "(null)";
            int n = 0;
            while (sv[n] && (precision < 0 || n < precision)) n++;
            int pad = width > n ? width - n : 0;
            if (!left) out_fill(buf, size, &pos, ' ', (unsigned long)pad);
            for (int i = 0; i < n; i++) out_char(buf, size, &pos, sv[i]);
            if (left) out_fill(buf, size, &pos, ' ', (unsigned long)pad);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            int pad = width > 1 ? width - 1 : 0;
            if (!left) out_fill(buf, size, &pos, ' ', (unsigned long)pad);
            out_char(buf, size, &pos, c);
            if (left) out_fill(buf, size, &pos, ' ', (unsigned long)pad);
            break;
        }
        case 'd': case 'i': {
            long v = longs ? va_arg(ap, long) : (long)va_arg(ap, int);
            if (half == 1) v = (short)v;
            else if (half == 2) v = (signed char)v;
            char text[32];
            unsigned long mag = v < 0 ? (unsigned long)-(v + 1) + 1UL : (unsigned long)v;
            int n = utoa(text, mag, 10, 0);
            int len = n, zeros = 0;
            if (precision >= 0) {
                zero = 0;
                if (mag == 0 && precision == 0) len = 0;
                else if (precision > n) {
                    /* The zero extension is emitted by out_field(); it must
                     * never be materialised into text[32] (F-rt-2). */
                    zeros = precision - n;
                }
            }
            char prefix[1];
            int plen = 0;
            if (v < 0) prefix[plen++] = '-';
            else if (plus) prefix[plen++] = '+';
            else if (space) prefix[plen++] = ' ';
            out_field(buf, size, &pos, prefix, plen, zeros, text, len, width, zero, left);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            int base = conv == 'u' ? 10 : (conv == 'o' ? 8 : 16);
            int upper = conv == 'X';
            unsigned long v = longs ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            if (half == 1) v = (unsigned short)v;
            else if (half == 2) v = (unsigned char)v;
            char text[32];
            int n = utoa(text, v, base, upper);
            int len = n, zeros = 0;
            if (precision >= 0) {
                zero = 0;
                if (v == 0 && precision == 0) len = 0;
                else if (precision > n) zeros = precision - n;
            }
            char prefix[2];
            int plen = 0;
            /* '#' on octal forces a leading zero, unless the precision
             * padding already starts the field with one (or the digits do). */
            int first_is_zero = zeros > 0 || (len > 0 && text[0] == '0');
            if (alt && base == 16 && v != 0) {
                prefix[plen++] = '0';
                prefix[plen++] = upper ? 'X' : 'x';
            } else if (alt && base == 8 && (len == 0 || !first_is_zero)) {
                prefix[plen++] = '0';
            }
            out_field(buf, size, &pos, prefix, plen, zeros, text, len, width, zero, left);
            break;
        }
        case 'p': {
            unsigned long v = (unsigned long)va_arg(ap, void *);
            char text[32];
            int n = utoa(text, v, 16, 0);
            out_field(buf, size, &pos, "0x", 2, 0, text, n, width, 0, left);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            if (longdouble) {
                /* No long-double support: say so instead of reading a double
                 * out of the wrong register and printing a wrong number. The
                 * argument is deliberately not consumed. */
                out_str(buf, size, &pos, "%L");
                out_char(buf, size, &pos, conv);
                break;
            }
            double v = va_arg(ap, double);
            __pagh_fmt_float(buf, size, &pos, v, conv, precision, width,
                             zero, left, plus, space, alt);
            break;
        }
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
int vprintf(const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0) write(1, buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
    return n;
}
/* The buffer is unbounded by definition; this is the same formatter as
 * vsnprintf with "no limit" (a negative return would still mean an error). */
int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, (size_t)-1, fmt, ap);
}
int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap); va_end(ap); return n;
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
