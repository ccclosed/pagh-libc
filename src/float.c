/* Float formatting for vsnprintf: %f %e %g (and the %F %E %G uppercase forms).
 *
 * The decimal digits are generated **exactly** from the IEEE-754 bit pattern
 * with a small binary bignum: the value is `mantissa * 2^exp`, the integer part
 * is a left/right shift, and the fraction is produced by repeatedly multiplying
 * the remainder by ten and peeling the bits that cross the binary point. Every
 * double (including the smallest denormal, whose exact expansion is 751
 * significant digits preceded by 323 zeros) therefore terminates inside the
 * digit buffer, and rounding is decided on the real digits, never on a
 * `double` comparison that has already lost them.
 *
 * `%f` is *not* a fixed-point approximation: `printf("%.0f", 0.5)` rounds to
 * even ("0"), `printf("%.20f", 0.1)` prints the true binary value of 0.1, and
 * values past 2^53 print their real decimal digits. The differential test
 * tests/printf_test.c compares this implementation with the host's glibc over
 * millions of random doubles and every precision.
 *
 * Limits, deliberate and documented in the README: `%a`/`%A` are not
 * implemented, `%Lf` (long double) is refused rather than misread, and a
 * precision above FP_PREC_MAX is clamped. */
#include <pagh_internal.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* The exact decimal expansion of the smallest denormal needs 323 leading
 * fraction zeros plus 751 significant digits; 1120 leaves room for the widest
 * integer part (309 digits) too. */
#define FP_MAX_DIGITS 1120
#define FP_BODY 2100
#define FP_PREC_MAX 1000

/* ── a little binary bignum (little-endian 32-bit words) ─────────────────── */
#define BN_WORDS 40 /* 1280 bits: 1074 + 53 rounded up */

typedef struct {
    uint32_t w[BN_WORDS];
    int n;
} bn;

static void bn_set_u64(bn *a, uint64_t v) {
    memset(a->w, 0, sizeof a->w);
    a->w[0] = (uint32_t)v;
    a->w[1] = (uint32_t)(v >> 32);
    a->n = a->w[1] ? 2 : (a->w[0] ? 1 : 0);
}

static int bn_is_zero(const bn *a) { return a->n == 0; }

static void bn_shl1(bn *a) {
    uint32_t carry = 0;
    for (int i = 0; i < a->n; i++) {
        uint32_t next = a->w[i] >> 31;
        a->w[i] = (a->w[i] << 1) | carry;
        carry = next;
    }
    if (carry && a->n < BN_WORDS) a->w[a->n++] = carry;
}

static void bn_shr_bits(bn *a, int k) {
    int word = k / 32, bit = k % 32;
    if (word >= a->n) { a->n = 0; return; }
    if (bit) {
        for (int i = word; i < a->n; i++) {
            uint32_t lo = a->w[i] >> bit;
            uint32_t hi = (i + 1 < a->n) ? (a->w[i + 1] << (32 - bit)) : 0;
            a->w[i - word] = lo | hi;
        }
    } else {
        for (int i = word; i < a->n; i++) a->w[i - word] = a->w[i];
    }
    for (int i = a->n - word; i < a->n; i++) a->w[i] = 0;
    a->n -= word;
    while (a->n > 0 && a->w[a->n - 1] == 0) a->n--;
}

static void bn_mul_small(bn *a, uint32_t m) {
    uint64_t carry = 0;
    for (int i = 0; i < a->n; i++) {
        uint64_t cur = (uint64_t)a->w[i] * m + carry;
        a->w[i] = (uint32_t)cur;
        carry = cur >> 32;
    }
    while (carry && a->n < BN_WORDS) {
        a->w[a->n++] = (uint32_t)carry;
        carry >>= 32;
    }
}

/* a /= 1e9, returns the remainder (the least significant nine decimal digits). */
static uint32_t bn_divmod_1e9(bn *a) {
    uint64_t rem = 0;
    for (int i = a->n - 1; i >= 0; i--) {
        uint64_t cur = (rem << 32) | a->w[i];
        a->w[i] = (uint32_t)(cur / 1000000000u);
        rem = cur % 1000000000u;
    }
    while (a->n > 0 && a->w[a->n - 1] == 0) a->n--;
    return (uint32_t)rem;
}

/* Keep only the bits below `k`. */
static void bn_mask_low(bn *a, int k) {
    int word = k / 32, bit = k % 32;
    if (word >= BN_WORDS) return;
    if (bit) {
        a->w[word] &= (uint32_t)((1u << bit) - 1);
        word++;
    }
    for (int i = word; i < BN_WORDS; i++) a->w[i] = 0;
    while (a->n > 0 && a->w[a->n - 1] == 0) a->n--;
}

/* The four bits at positions k..k+3. The caller only ever asks after a
 * multiply by ten, when the value above bit k is a single decimal digit. */
static uint32_t bn_bits_above(const bn *a, int k) {
    uint32_t out = 0;
    for (int b = 0; b < 4; b++) {
        int pos = k + b;
        if (pos >= a->n * 32) break;
        out |= ((a->w[pos / 32] >> (pos % 32)) & 1u) << b;
    }
    return out;
}

/* Decimal digits of `a`, most significant first. Returns the count written
 * (capped at `cap`, but the count is exact so the caller can tell). */
static int bn_to_digits(bn *a, char *out, int cap) {
    uint32_t groups[40];
    int ng = 0;
    while (!bn_is_zero(a) && ng < 40) groups[ng++] = bn_divmod_1e9(a);
    int n = 0;
    if (ng == 0) {
        if (cap > 0) out[0] = '0';
        return 1;
    }
    uint32_t top = groups[ng - 1];
    char tmp[10];
    int t = 0;
    do { tmp[t++] = (char)('0' + top % 10); top /= 10; } while (top);
    while (t) { if (n < cap) out[n] = tmp[--t]; n++; }
    for (int g = ng - 2; g >= 0; g--) {
        char nine[9];
        uint32_t v = groups[g];
        for (int i = 8; i >= 0; i--) { nine[i] = (char)('0' + v % 10); v /= 10; }
        for (int i = 0; i < 9; i++) { if (n < cap) out[n] = nine[i]; n++; }
    }
    return n;
}

/* ── one decoded double ──────────────────────────────────────────────────── */
typedef struct {
    char digits[FP_MAX_DIGITS];
    int nd;  /* significant digits held */
    int exp; /* digits[0] has place value 10^exp */
    int neg;
    int special; /* 1 = NaN, 2 = infinity */
} fp_num;

static void fp_decode(double value, fp_num *o) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    o->neg = (int)(bits >> 63);
    int biased = (int)((bits >> 52) & 0x7ff);
    uint64_t mant = bits & 0xfffffffffffffULL;
    o->nd = 0;
    o->exp = 0;
    o->special = 0;
    if (biased == 0x7ff) { o->special = mant ? 1 : 2; return; }
    uint64_t m;
    int e;
    if (biased == 0) {
        if (mant == 0) { /* signed zero: one digit so the formatters stay simple */
            o->digits[0] = '0';
            o->nd = 1;
            return;
        }
        m = mant;
        e = -1074;
    } else {
        m = mant | (1ULL << 52);
        e = biased - 1075;
    }
    if (e >= 0) {
        bn a;
        bn_set_u64(&a, m);
        for (int i = 0; i < e; i++) bn_shl1(&a);
        o->nd = bn_to_digits(&a, o->digits, FP_MAX_DIGITS);
        o->exp = o->nd - 1;
        return;
    }
    int k = -e;
    bn ip;
    bn_set_u64(&ip, m);
    bn_shr_bits(&ip, k);
    int intn = bn_is_zero(&ip) ? 0 : bn_to_digits(&ip, o->digits, FP_MAX_DIGITS);
    bn r;
    bn_set_u64(&r, m);
    bn_mask_low(&r, k);
    int fd = 0;
    while (!bn_is_zero(&r) && intn + fd < FP_MAX_DIGITS) {
        bn_mul_small(&r, 10);
        uint32_t d = bn_bits_above(&r, k);
        bn_mask_low(&r, k);
        o->digits[intn + fd++] = (char)('0' + (d % 10));
    }
    /* When the integer part is zero, the fraction's leading zeros only set the
     * exponent and can be dropped; when it is not, they are significant digit
     * positions in the fixed layout ("1.0000000000000002220...") and must
     * stay. Getting this wrong prints 1+2^-52 as 1.2220. */
    int z = 0;
    if (intn == 0) {
        while (z < fd && o->digits[z] == '0') z++;
        if (z) memmove(o->digits, o->digits + z, (size_t)(fd - z));
    }
    o->nd = intn + (fd - z);
    o->exp = intn > 0 ? intn - 1 : -(z + 1);
}

/* Round away everything from index `cut` on. `cut` may be negative (the value
 * is far below the last requested place) or past the end (nothing to do). */
static void fp_round(fp_num *f, int cut) {
    if (cut >= f->nd) return;
    if (cut < 0) { f->nd = 0; return; }
    int up = 0;
    char d = f->digits[cut];
    if (d > '5') up = 1;
    else if (d == '5') {
        int rest = 0;
        for (int i = cut + 1; i < f->nd; i++) {
            if (f->digits[i] != '0') { rest = 1; break; }
        }
        if (rest) up = 1;
        else up = ((cut > 0 ? f->digits[cut - 1] : '0') - '0') & 1;
    }
    f->nd = cut;
    if (!up) return;
    int i = cut - 1;
    while (i >= 0) {
        if (f->digits[i] == '9') { f->digits[i] = '0'; i--; }
        else { f->digits[i]++; break; }
    }
    if (i < 0) {
        /* Every kept digit was a nine: 999 -> 1000, one place up. */
        memmove(f->digits + 1, f->digits, (size_t)cut);
        f->digits[0] = '1';
        f->nd = cut + 1;
        f->exp++;
    }
}

/* The digit at fraction position i (place 10^-(i+1)) when the digits are laid
 * out as the fixed form: digits[0] has place value 10^exp. */
static char fp_frac_digit(const fp_num *f, int i) {
    int idx = f->exp + 1 + i;
    return (idx >= 0 && idx < f->nd) ? f->digits[idx] : '0';
}

/* The i-th significant digit, for the exponential form (digits[0] is the one
 * before the point). */
static char fp_sig_digit(const fp_num *f, int i) {
    return (i >= 0 && i < f->nd) ? f->digits[i] : '0';
}

static char fp_int_digit(const fp_num *f, int i) {
    return (i >= 0 && i < f->nd) ? f->digits[i] : '0';
}

/* ── layout ──────────────────────────────────────────────────────────────── */
static int emit_fixed(char *body, const fp_num *f, int prec, int alt) {
    int n = 0;
    int intlen = f->exp >= 0 ? f->exp + 1 : 1;
    if (f->exp >= 0) {
        for (int i = 0; i < intlen; i++) body[n++] = fp_int_digit(f, i);
    } else {
        body[n++] = '0';
    }
    if (prec > 0 || alt) body[n++] = '.';
    for (int i = 0; i < prec; i++) body[n++] = fp_frac_digit(f, i);
    return n;
}

static int emit_exp(char *body, const fp_num *f, int prec, int alt, int upper) {
    int n = 0;
    body[n++] = fp_sig_digit(f, 0);
    if (prec > 0 || alt) body[n++] = '.';
    for (int i = 0; i < prec; i++) body[n++] = fp_sig_digit(f, i + 1);
    int e = f->exp;
    char sign = '-';
    if (e < 0) e = -e; else sign = '+';
    body[n++] = upper ? 'E' : 'e';
    body[n++] = sign;
    char digits[4];
    int t = 0;
    do { digits[t++] = (char)('0' + e % 10); e /= 10; } while (e);
    while (t < 2) digits[t++] = '0';
    while (t) body[n++] = digits[--t];
    return n;
}

int __pagh_fmt_float(char *buf, size_t size, size_t *pos, double value,
                     int conv, int precision, int width, int zero, int left,
                     int plus, int space, int alt) {
    char body[FP_BODY];
    int bodylen = 0;
    fp_num f;
    fp_decode(value, &f);
    int upper = (conv == 'F' || conv == 'E' || conv == 'G');
    int base = upper ? conv + 32 : conv;
    int prec = precision < 0 ? 6 : precision;
    if (prec > FP_PREC_MAX) prec = FP_PREC_MAX;

    if (f.special) {
        const char *text = f.special == 1 ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        while (text[bodylen]) { body[bodylen] = text[bodylen]; bodylen++; }
        zero = 0; /* padding for nan/inf is spaces, like glibc */
    } else if (base == 'f') {
        fp_round(&f, f.exp + 1 + prec);
        bodylen = emit_fixed(body, &f, prec, alt);
    } else if (base == 'e') {
        fp_round(&f, prec + 1);
        bodylen = emit_exp(body, &f, prec, alt, upper);
    } else { /* 'g' */
        /* The style is chosen from the exponent of the value already rounded
         * to P significant digits, which is what the standard says and what
         * makes 999.9 with %.3g print 1e+03, not 1000. */
        int p = precision == 0 ? 1 : prec;
        fp_round(&f, p);
        if (f.exp >= -4 && f.exp < p) {
            bodylen = emit_fixed(body, &f, p - 1 - f.exp, alt);
        } else {
            bodylen = emit_exp(body, &f, p - 1, alt, upper);
        }
        if (!alt) {
            /* Style g drops trailing zeros *of the fraction* and a bare decimal
             * point; the zeros of an integer part ("100") stay. */
            int epos = -1;
            for (int i = 0; i < bodylen; i++) if (body[i] == 'e' || body[i] == 'E') { epos = i; break; }
            int mant = epos < 0 ? bodylen : epos;
            int dot = -1;
            for (int i = 0; i < mant; i++) if (body[i] == '.') { dot = i; break; }
            if (dot >= 0) {
                while (mant > dot + 1 && body[mant - 1] == '0') mant--;
                if (mant == dot + 1) mant = dot;
            }
            if (mant == 0) body[mant++] = '0';
            if (epos >= 0) {
                memmove(body + mant, body + epos, (size_t)(bodylen - epos));
                bodylen = mant + (bodylen - epos);
            } else {
                bodylen = mant;
            }
        }
    }

    char sign = 0;
    if (f.neg) sign = '-';
    else if (plus) sign = '+';
    else if (space) sign = ' ';
    int signlen = sign ? 1 : 0;
    int total = signlen + bodylen;
    int pad = width > total ? width - total : 0;

    /* out_char semantics: pos always advances, bytes past `size` are dropped. */
    #define PUT(c) do { if (*pos + 1 < size) buf[*pos] = (char)(c); (*pos)++; } while (0)
    if (left) {
        if (sign) PUT(sign);
        for (int i = 0; i < bodylen; i++) PUT(body[i]);
        for (int i = 0; i < pad; i++) PUT(' ');
    } else if (zero) {
        if (sign) PUT(sign);
        for (int i = 0; i < pad; i++) PUT('0');
        for (int i = 0; i < bodylen; i++) PUT(body[i]);
    } else {
        for (int i = 0; i < pad; i++) PUT(' ');
        if (sign) PUT(sign);
        for (int i = 0; i < bodylen; i++) PUT(body[i]);
    }
    #undef PUT
    return 0;
}
