/* Formatted input: sscanf/vsscanf, scanf/vscanf and fscanf/vfscanf.
 *
 * The scanner is one small state machine over a byte source; the three public
 * entry points differ only in where the bytes come from (a string, stdin or a
 * FILE). The conversion set is the C/POSIX one an applet actually uses:
 *
 *   d i o u x X c s [ ] p n f e g %%
 *
 * with `*` suppression, field width and the hh/h/l/ll/z/j/t length modifiers.
 * Documented limits: wide-character conversions (%lc/%ls/%l[), `%L` long
 * double, `%a` hexadecimal floats and the C23 `%b` binary conversion (and its
 * 0b prefix in %i) are not implemented (a matching failure),
 * and there is no locale (the decimal point is always '.'). Unknown
 * conversions fail the match instead of being copied.
 *
 * `%f`/`%e`/`%g` are parsed into an unsigned 64-bit mantissa and scaled by a
 * table of exact powers of ten: for the usual case (at most 15 significant
 * digits, decimal exponent within +-22) the result is the correctly rounded
 * double, i.e. what strtod produces. Beyond that the value is the nearest
 * double of the accumulated arithmetic, not a correctly rounded decimal
 * conversion; the header says so. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { SCAN_EOF = -1, SCAN_OK = 1, SCAN_FAIL = 0 };

/* Length modifiers. */
enum {
    LEN_NONE = 0,
    LEN_HH,
    LEN_H,
    LEN_L,
    LEN_LL,
    LEN_J,
    LEN_Z,
    LEN_T
};

struct scan {
    int from_string;        /* sscanf: read from `text` */
    const char *text;
    FILE *stream;
    int consumed;           /* bytes consumed, for %n */
    int pushback[4];        /* LIFO pushback, at most a few characters */
    int pushed;
};

static int scan_getc(struct scan *sc) {
    if (sc->pushed > 0) {
        int c = sc->pushback[--sc->pushed];
        sc->consumed++;
        return c;
    }
    int c;
    if (sc->from_string) {
        c = (unsigned char)sc->text[sc->consumed];
        if (c == 0) return SCAN_EOF;
    } else {
        c = fgetc(sc->stream);
        if (c == EOF) return SCAN_EOF;
    }
    sc->consumed++;
    return c;
}

static void scan_ungetc(struct scan *sc, int c) {
    if (c < 0) return;
    sc->pushback[sc->pushed++] = c;
    sc->consumed--;
}

static int is_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static int digit_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Skip input whitespace. Returns SCAN_OK or SCAN_EOF. */
static int skip_space(struct scan *sc) {
    for (;;) {
        int c = scan_getc(sc);
        if (c == SCAN_EOF) return SCAN_EOF;
        if (!is_space(c)) {
            scan_ungetc(sc, c);
            return SCAN_OK;
        }
    }
}

/* Parse an integer item. Returns SCAN_OK (magnitude/negative set), SCAN_FAIL
 * (no digits) or SCAN_EOF (input ended before the item started). */
static int scan_integer(struct scan *sc, int base, int width, int allow_sign,
                        unsigned long long *magnitude, int *negative) {
    int used = 0;
    int neg = 0;
    int digits_base = base;
    int ndigits = 0;
    unsigned long long value = 0;
    int c = scan_getc(sc);
    if (c == SCAN_EOF) return SCAN_EOF;

    if (allow_sign && (c == '-' || c == '+')) {
        neg = c == '-';
        used++;
        if (width > 0 && used >= width) return SCAN_FAIL;
        c = scan_getc(sc);
        if (c == SCAN_EOF) return SCAN_FAIL;
    }

    if (base == 0) {
        if (c == '0') {
            used++;
            int next = (width > 0 && used >= width) ? -2 : scan_getc(sc);
            if (next == 'x' || next == 'X') {
                digits_base = 16;
                used++;
                c = (width > 0 && used >= width) ? -2 : scan_getc(sc);
            } else {
                digits_base = 8;
                c = next;
            }
            ndigits = 1; /* the leading zero itself is a digit */
            value = 0;
            if (c == -2 || c == SCAN_EOF) {
                *magnitude = 0;
                *negative = neg;
                return SCAN_OK;
            }
        } else {
            digits_base = 10;
        }
    } else if (base == 16 && c == '0') {
        used++;
        int next = (width > 0 && used >= width) ? -2 : scan_getc(sc);
        if (next == 'x' || next == 'X') {
            used++;
            c = (width > 0 && used >= width) ? -2 : scan_getc(sc);
        } else {
            c = next;
            value = 0;
            ndigits = 1;
            if (c == -2 || c == SCAN_EOF) {
                *magnitude = 0;
                *negative = neg;
                return SCAN_OK;
            }
        }
    }

    while (c >= 0 && ndigits < 1000000) {
        int d = digit_value(c);
        if (d < 0 || d >= digits_base) break;
        value = value * (unsigned)digits_base + (unsigned)d;
        ndigits++;
        used++;
        if (width > 0 && used >= width) {
            c = -2;
            break;
        }
        c = scan_getc(sc);
    }
    if (c >= 0) scan_ungetc(sc, c);
    if (ndigits == 0) return SCAN_FAIL;
    *magnitude = value;
    *negative = neg;
    return SCAN_OK;
}

static const double POW10[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

/* Parse a decimal floating-point item. Same return convention as
 * scan_integer. `*out` is the nearest double for the correctly-rounded range
 * described at the top of the file. */
static int scan_double(struct scan *sc, int width, double *out) {
    int used = 0;
    int neg = 0;
    unsigned long long mantissa = 0;
    int significant = 0;   /* digits kept in `mantissa` */
    int int_digits = 0;    /* digits before the point (all of them) */
    int saw_digit = 0;
    long long exponent = 0;
    int c = scan_getc(sc);
    if (c == SCAN_EOF) return SCAN_EOF;

    if (c == '-' || c == '+') {
        neg = c == '-';
        used++;
        if (width > 0 && used >= width) return SCAN_FAIL;
        c = scan_getc(sc);
        if (c == SCAN_EOF) return SCAN_FAIL;
    }

    while (c >= 0 && c >= '0' && c <= '9') {
        saw_digit = 1;
        int_digits++;
        if (significant < 19) {
            mantissa = mantissa * 10 + (unsigned)(c - '0');
            significant++;
        }
        used++;
        if (width > 0 && used >= width) {
            c = -2;
            break;
        }
        c = scan_getc(sc);
    }
    if (c == '.') {
        used++;
        c = (width > 0 && used >= width) ? -2 : scan_getc(sc);
        while (c >= 0 && c >= '0' && c <= '9') {
            saw_digit = 1;
            if (significant < 19) {
                mantissa = mantissa * 10 + (unsigned)(c - '0');
                significant++;
            }
            used++;
            if (width > 0 && used >= width) {
                c = -2;
                break;
            }
            c = scan_getc(sc);
        }
    }
    if (!saw_digit) {
        if (c >= 0) scan_ungetc(sc, c);
        return SCAN_FAIL;
    }

    /* Optional exponent. An 'e' without digits is not part of the item, so it
     * is pushed back (the scanner has room for the three characters consumed
     * while looking ahead). */
    if (c == 'e' || c == 'E') {
        int saved_e = c;
        int sign_char = 0;
        int sign_value = 1;
        used++;
        int next = (width > 0 && used >= width) ? -2 : scan_getc(sc);
        if (next == '+' || next == '-') {
            sign_char = next;
            sign_value = next == '-' ? -1 : 1;
            used++;
            next = (width > 0 && used >= width) ? -2 : scan_getc(sc);
        }
        long long exp_value = 0;
        int exp_digits = 0;
        while (next >= 0 && next >= '0' && next <= '9') {
            exp_digits++;
            if (exp_value < 1000000000LL) exp_value = exp_value * 10 + (next - '0');
            used++;
            if (width > 0 && used >= width) {
                next = -2;
                break;
            }
            next = scan_getc(sc);
        }
        if (exp_digits > 0) {
            exponent = sign_value * exp_value;
            if (next >= 0) scan_ungetc(sc, next);
        } else {
            /* An 'e' with no exponent digits makes the whole item invalid
             * (glibc returns a matching failure here, it does not fall back
             * to the mantissa alone). */
            (void)saved_e;
            (void)sign_char;
            return SCAN_FAIL;
        }
    } else if (c >= 0) {
        scan_ungetc(sc, c);
    }

    long long scale = exponent + int_digits - significant;
    double value = (double)mantissa;
    if (significant <= 15 && scale >= -22 && scale <= 22) {
        if (scale > 0) value *= POW10[scale];
        else if (scale < 0) value /= POW10[-scale];
    } else {
        /* Large/small magnitudes: scale in exact-power chunks. This is the
         * documented not-correctly-rounded fallback. */
        while (scale > 22) {
            value *= POW10[22];
            scale -= 22;
        }
        while (scale > 0) {
            value *= POW10[scale];
            scale = 0;
        }
        while (scale < -22) {
            value /= POW10[22];
            scale += 22;
        }
        if (scale < 0) value /= POW10[-scale];
    }
    *out = neg ? -value : value;
    return SCAN_OK;
}

/* Parse a scanset at `*format` (just after '['). Fills SET and advances the
 * format pointer past the closing ']'. Returns 0 on success, -1 on a malformed
 * set (no closing bracket). */
static int parse_scanset(const char **format, unsigned char set[256]) {
    const char *p = *format;
    int negate = 0;
    memset(set, 0, 256);
    if (*p == '^') {
        negate = 1;
        p++;
    }
    if (*p == ']') {
        set[(unsigned char)']'] = 1;
        p++;
    }
    while (*p && *p != ']') {
        unsigned char first = (unsigned char)*p++;
        if (p[0] == '-' && p[1] && p[1] != ']') {
            unsigned char last = (unsigned char)p[1];
            p += 2;
            for (unsigned v = first; v <= last; v++) set[v] = 1;
        } else {
            set[first] = 1;
        }
    }
    if (*p != ']') return -1;
    p++;
    if (negate) {
        for (int i = 0; i < 256; i++) set[i] = (unsigned char)!set[i];
    }
    *format = p;
    return 0;
}

static int scan_format(struct scan *sc, const char *format, va_list ap) {
    int assigned = 0;

    while (*format) {
        if (is_space(*format)) {
            while (is_space(*format)) format++;
            if (skip_space(sc) == SCAN_EOF) {
                if (assigned == 0) return EOF;
                return assigned;
            }
            continue;
        }
        if (*format != '%') {
            int c = scan_getc(sc);
            if (c == SCAN_EOF) {
                if (assigned == 0) return EOF;
                return assigned;
            }
            if (c != (unsigned char)*format) {
                scan_ungetc(sc, c);
                return assigned;
            }
            format++;
            continue;
        }

        format++;
        if (*format == '%') {
            int c = scan_getc(sc);
            if (c == SCAN_EOF) {
                if (assigned == 0) return EOF;
                return assigned;
            }
            if (c != '%') {
                scan_ungetc(sc, c);
                return assigned;
            }
            format++;
            continue;
        }

        int suppress = 0;
        if (*format == '*') {
            suppress = 1;
            format++;
        }
        int width = -1;
        if (*format >= '0' && *format <= '9') {
            width = 0;
            while (*format >= '0' && *format <= '9') width = width * 10 + (*format++ - '0');
            if (width == 0) width = -1;
        }
        int len = LEN_NONE;
        if (format[0] == 'h' && format[1] == 'h') { len = LEN_HH; format += 2; }
        else if (format[0] == 'h') { len = LEN_H; format += 1; }
        else if (format[0] == 'l' && format[1] == 'l') { len = LEN_LL; format += 2; }
        else if (format[0] == 'l') { len = LEN_L; format += 1; }
        else if (format[0] == 'j') { len = LEN_J; format += 1; }
        else if (format[0] == 'z') { len = LEN_Z; format += 1; }
        else if (format[0] == 't') { len = LEN_T; format += 1; }
        else if (format[0] == 'L') { len = -1; format += 1; } /* long double: unsupported */

        char conversion = *format;
        if (conversion == 0) { /* a trailing '%' is a malformed format */
            return assigned;
        }
        format++;

        if (conversion != 'c' && conversion != '[' && conversion != 'n') {
            if (skip_space(sc) == SCAN_EOF) {
                if (assigned == 0) return EOF;
                return assigned;
            }
        }

        switch (conversion) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            int base = (conversion == 'd' || conversion == 'u') ? 10
                     : (conversion == 'i') ? 0
                     : (conversion == 'o') ? 8 : 16;
            int allow_sign = conversion != 'u' && conversion != 'o' &&
                             conversion != 'x' && conversion != 'X';
            unsigned long long magnitude;
            int negative;
            int r = scan_integer(sc, base, width, allow_sign, &magnitude, &negative);
            if (r == SCAN_EOF) {
                if (assigned == 0) return EOF;
                return assigned;
            }
            if (r == SCAN_FAIL) return assigned;
            if (!suppress) {
                /* Store through va_arg here, never through a helper that
                 * copies `ap`: a va_list passed by value does not share its
                 * state on every ABI (RISC-V among them). */
                if (allow_sign) {
                    long long value = negative ? -(long long)magnitude : (long long)magnitude;
                    switch (len) {
                    case LEN_HH: *va_arg(ap, signed char *) = (signed char)value; break;
                    case LEN_H: *va_arg(ap, short *) = (short)value; break;
                    case LEN_L: *va_arg(ap, long *) = (long)value; break;
                    case LEN_LL: *va_arg(ap, long long *) = value; break;
                    case LEN_J: *va_arg(ap, intmax_t *) = (intmax_t)value; break;
                    case LEN_Z: *va_arg(ap, size_t *) = (size_t)value; break;
                    case LEN_T: *va_arg(ap, ptrdiff_t *) = (ptrdiff_t)value; break;
                    default: *va_arg(ap, int *) = (int)value; break;
                    }
                } else {
                    switch (len) {
                    case LEN_HH: *va_arg(ap, unsigned char *) = (unsigned char)magnitude; break;
                    case LEN_H: *va_arg(ap, unsigned short *) = (unsigned short)magnitude; break;
                    case LEN_L: *va_arg(ap, unsigned long *) = (unsigned long)magnitude; break;
                    case LEN_LL: *va_arg(ap, unsigned long long *) = magnitude; break;
                    case LEN_J: *va_arg(ap, uintmax_t *) = (uintmax_t)magnitude; break;
                    case LEN_Z: *va_arg(ap, size_t *) = (size_t)magnitude; break;
                    case LEN_T: *va_arg(ap, ptrdiff_t *) = (ptrdiff_t)magnitude; break;
                    default: *va_arg(ap, unsigned int *) = (unsigned int)magnitude; break;
                    }
                }
                assigned++;
            }
            break;
        }
        case 'p': {
            unsigned long long magnitude;
            int negative;
            int r = scan_integer(sc, 16, width, 0, &magnitude, &negative);
            if (r == SCAN_EOF) {
                if (assigned == 0) return EOF;
                return assigned;
            }
            if (r == SCAN_FAIL) return assigned;
            if (!suppress) {
                *va_arg(ap, void **) = (void *)(uintptr_t)magnitude;
                assigned++;
            }
            break;
        }
        case 'f': case 'e': case 'g': case 'E': case 'G': {
            if (len < 0) return assigned; /* %Lf: long double unsupported */
            double value;
            int r = scan_double(sc, width, &value);
            if (r == SCAN_EOF) {
                if (assigned == 0) return EOF;
                return assigned;
            }
            if (r == SCAN_FAIL) return assigned;
            if (!suppress) {
                if (len == LEN_L) *va_arg(ap, double *) = value;
                else *va_arg(ap, float *) = (float)value;
                assigned++;
            }
            break;
        }
        case 'c': {
            int count = width > 0 ? width : 1;
            char *out = suppress ? 0 : va_arg(ap, char *);
            int got = 0;
            int hit_eof = 0;
            while (got < count) {
                int c = scan_getc(sc);
                if (c == SCAN_EOF) {
                    hit_eof = 1;
                    break;
                }
                if (out) out[got] = (char)c;
                got++;
            }
            if (got == 0) {
                if (hit_eof && assigned == 0) return EOF;
                return assigned;
            }
            if (!suppress) assigned++;
            break;
        }
        case 's': {
            char *out = suppress ? 0 : va_arg(ap, char *);
            int got = 0;
            for (;;) {
                int c = scan_getc(sc);
                if (c == SCAN_EOF) break;
                if (is_space(c)) {
                    scan_ungetc(sc, c);
                    break;
                }
                if (width > 0 && got >= width) {
                    scan_ungetc(sc, c);
                    break;
                }
                if (out) out[got] = (char)c;
                got++;
            }
            if (got == 0) {
                if (assigned == 0) return EOF;
                return assigned;
            }
            if (out) out[got] = 0;
            if (!suppress) assigned++;
            break;
        }
        case '[': {
            unsigned char set[256];
            if (parse_scanset(&format, set) != 0) return assigned;
            char *out = suppress ? 0 : va_arg(ap, char *);
            int got = 0;
            int hit_eof = 0;
            for (;;) {
                int c = scan_getc(sc);
                if (c == SCAN_EOF) {
                    hit_eof = 1;
                    break;
                }
                if (!set[(unsigned char)c]) {
                    scan_ungetc(sc, c);
                    break;
                }
                if (width > 0 && got >= width) {
                    scan_ungetc(sc, c);
                    break;
                }
                if (out) out[got] = (char)c;
                got++;
            }
            if (got == 0) {
                if (hit_eof && assigned == 0) return EOF;
                return assigned;
            }
            if (out) out[got] = 0;
            if (!suppress) assigned++;
            break;
        }
        case 'n': {
            if (!suppress) {
                switch (len) {
                case LEN_HH: *va_arg(ap, signed char *) = (signed char)sc->consumed; break;
                case LEN_H: *va_arg(ap, short *) = (short)sc->consumed; break;
                case LEN_L: *va_arg(ap, long *) = (long)sc->consumed; break;
                case LEN_LL: *va_arg(ap, long long *) = (long long)sc->consumed; break;
                case LEN_J: *va_arg(ap, intmax_t *) = (intmax_t)sc->consumed; break;
                case LEN_Z: *va_arg(ap, size_t *) = (size_t)sc->consumed; break;
                case LEN_T: *va_arg(ap, ptrdiff_t *) = (ptrdiff_t)sc->consumed; break;
                default: *va_arg(ap, int *) = sc->consumed; break;
                }
            }
            break;
        }
        default:
            return assigned; /* unsupported conversion: matching failure */
        }
    }
    return assigned;
}

int vsscanf(const char *str, const char *format, va_list ap) {
    struct scan sc;
    memset(&sc, 0, sizeof sc);
    sc.from_string = 1;
    sc.text = str;
    va_list copy;
    va_copy(copy, ap);
    int result = scan_format(&sc, format, copy);
    va_end(copy);
    return result;
}

int sscanf(const char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int result = vsscanf(str, format, ap);
    va_end(ap);
    return result;
}

int vfscanf(FILE *stream, const char *format, va_list ap) {
    struct scan sc;
    memset(&sc, 0, sizeof sc);
    sc.from_string = 0;
    sc.stream = stream;
    va_list copy;
    va_copy(copy, ap);
    int result = scan_format(&sc, format, copy);
    va_end(copy);
    return result;
}

int fscanf(FILE *stream, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int result = vfscanf(stream, format, ap);
    va_end(ap);
    return result;
}

int vscanf(const char *format, va_list ap) {
    return vfscanf(stdin, format, ap);
}

int scanf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int result = vscanf(format, ap);
    va_end(ap);
    return result;
}