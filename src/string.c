#include <string.h>
#include <stdlib.h>

size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strncpy(char *d, const char *s, size_t n) {
    char *r = d; while (n && (*d = *s)) { d++; s++; n--; }
    while (n--) *d++ = 0;
    return r;
}
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strchr(const char *s, int c) {
    for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; }
}
char *strrchr(const char *s, int c) {
    const char *last = 0;
    for (;; s++) { if (*s == (char)c) last = s; if (!*s) return (char *)last; }
}
char *strstr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++) if (!strncmp(h, n, nl)) return (char *)h;
    return 0;
}
char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) n++;
    return n;
}
size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) n++;
    return n;
}
/* The reentrant tokenizer: `*saveptr` carries the position between calls, so
 * two independent tokenizations can be live at once. Leading delimiters are
 * skipped, the token is NUL-terminated in place, and a pointer to the rest is
 * stored in `*saveptr` (an empty string at the end; NULL is returned). */
char *strtok_r(char *s, const char *delim, char **saveptr) {
    if (!s) s = *saveptr;
    if (!s) return 0;
    s += strspn(s, delim);
    if (!*s) { *saveptr = s; return 0; }
    char *end = s + strcspn(s, delim);
    if (!*end) {
        *saveptr = end;
    } else {
        *end = 0;
        *saveptr = end + 1;
    }
    return s;
}
/* Like strtok_r but the delimiter is *not* skipped: adjacent delimiters give
 * empty tokens and the caller sees every field of e.g. a CSV-style record. */
char *strsep(char **stringp, const char *delim) {
    char *s = *stringp;
    if (!s) return 0;
    char *end = s + strcspn(s, delim);
    if (*end) {
        *end = 0;
        *stringp = end + 1;
    } else {
        *stringp = 0;
    }
    return s;
}
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dd = d; const unsigned char *ss = s;
    for (size_t i = 0; i < n; i++) dd[i] = ss[i];
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dd = d; const unsigned char *ss = s;
    if (dd < ss) for (size_t i = 0; i < n; i++) dd[i] = ss[i];
    else for (size_t i = n; i > 0; i--) dd[i-1] = ss[i-1];
    return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *dd = d;
    for (size_t i = 0; i < n; i++) dd[i] = (unsigned char)c;
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *aa = a, *bb = b;
    for (size_t i = 0; i < n; i++) if (aa[i] != bb[i]) return aa[i] - bb[i];
    return 0;
}
void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    unsigned char ch = (unsigned char)c;
    for (size_t i = 0; i < n; i++) if (p[i] == ch) return (void *)(p + i);
    return 0;
}
