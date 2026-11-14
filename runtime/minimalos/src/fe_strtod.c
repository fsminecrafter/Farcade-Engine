/* strtod, strtol & co. The decimal conversion keeps 19 significant digits
 * and scales by a power of ten in double-double arithmetic, so the result is
 * correctly rounded except for inputs within 1e-30 of a rounding boundary. */
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include "fe_dd.h"

static int ch_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int ch_space(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static int ch_digit(int c) { return c >= '0' && c <= '9'; }
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = ch_lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int match_word(const char* s, const char* w) {
    int i = 0;
    for (; w[i]; i++) if (ch_lower((unsigned char)s[i]) != w[i]) return 0;
    return i;
}

/* m (exact up to 2^64) as double-double */
static dd_t u64_to_dd(uint64_t m) {
    if (!(m >> 53)) { dd_t r = { (double)m, 0.0 }; return r; }
    return quick_two_sum((double)(m >> 11) * 2048.0, (double)(m & 2047));
}

/* dd * 2^e2 rounded to double (once, also in the subnormal range) */
static double dd_finish(dd_t v, int e2) {
    double s = v.hi + v.lo;
    uint64_t u;
    __builtin_memcpy(&u, &s, 8);
    int ex = (int)((u >> 52) & 0x7ff) - 1023;
    if (s == 0.0 || ex + e2 >= -1022) return scalbn(s, e2);
    int sh = e2 + 1074;                      /* value in units of 2^-1074 */
    double vh = scalbn(v.hi, sh), vl = scalbn(v.lo, sh);
    double i = (double)(long long)(vh + (vh >= 0 ? 0.5 : -0.5));
    if (i - vh > 0.5 || vh - i > 0.5) i = (double)(long long)vh;
    double frac = (vh - i) + vl;
    if (frac > 0.5) i += 1.0;
    else if (frac < -0.5) i -= 1.0;
    else if (frac == 0.5 || frac == -0.5) { if ((long long)i & 1) i += frac > 0 ? 1.0 : -1.0; }
    return scalbn(i, -1074);
}

/* v * 10^n, keeping the binary exponent apart so nothing overflows early */
static double scale10(dd_t v, int n) {
    int neg = n < 0;
    unsigned k = (unsigned)(neg ? -n : n);
    dd_t p = { 1.0, 0.0 };
    dd_t b = { 10.0, 0.0 };
    int pe = 0, be = 0;
    while (k) {
        if (k & 1) {
            p = dd_mul(p, b);
            pe += be;
            int ex;
            double f = p.hi;
            if (f != 0.0) { ex = 0; while (f >= 2.0) { f *= 0.5; ex++; } while (f < 1.0) { f *= 2.0; ex--; }
                p.hi = scalbn(p.hi, -ex); p.lo = scalbn(p.lo, -ex); pe += ex; }
        }
        k >>= 1;
        if (k) {
            b = dd_mul(b, b);
            be *= 2;
            double f = b.hi;
            int ex = 0;
            while (f >= 2.0) { f *= 0.5; ex++; }
            b.hi = scalbn(b.hi, -ex); b.lo = scalbn(b.lo, -ex); be += ex;
        }
    }
    dd_t r = neg ? dd_div(v, p) : dd_mul(v, p);
    return dd_finish(r, neg ? -pe : pe);
}

double strtod(const char* __restrict s, char** __restrict end) {
    const char* p = s;
    while (ch_space((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    double result;
    if (ch_lower((unsigned char)*p) == 'i') {
        int n = match_word(p, "infinity");
        if (!n) n = match_word(p, "inf");
        if (n) { if (end) *end = (char*)(p + n); return neg ? -HUGE_VAL : HUGE_VAL; }
        if (end) *end = (char*)s;
        return 0.0;
    }
    if (ch_lower((unsigned char)*p) == 'n') {
        int n = match_word(p, "nan");
        if (n) {
            p += n;
            if (*p == '(') { const char* q = p + 1; while (*q && *q != ')' && (ch_digit(*q) || (ch_lower(*q) >= 'a' && ch_lower(*q) <= 'z') || *q == '_')) q++; if (*q == ')') p = q + 1; }
            if (end) *end = (char*)p;
            return neg ? -__builtin_nan("") : __builtin_nan("");
        }
        if (end) *end = (char*)s;
        return 0.0;
    }
    if (p[0] == '0' && ch_lower((unsigned char)p[1]) == 'x' && (hexval((unsigned char)p[2]) >= 0 || (p[2] == '.' && hexval((unsigned char)p[3]) >= 0))) {
        p += 2;
        uint64_t m = 0;
        int e2 = 0, any = 0;
        for (; hexval((unsigned char)*p) >= 0; p++) {
            any = 1;
            if (m >> 60) e2 += 4; else m = (m << 4) | (uint64_t)hexval((unsigned char)*p);
        }
        if (*p == '.') {
            p++;
            for (; hexval((unsigned char)*p) >= 0; p++) {
                any = 1;
                if (!(m >> 60)) { m = (m << 4) | (uint64_t)hexval((unsigned char)*p); e2 -= 4; }
            }
        }
        if (!any) { if (end) *end = (char*)s; return 0.0; }
        if (ch_lower((unsigned char)*p) == 'p') {
            const char* q = p + 1;
            int eneg = 0;
            if (*q == '-') { eneg = 1; q++; } else if (*q == '+') q++;
            if (ch_digit(*q)) {
                int ev = 0;
                for (; ch_digit(*q); q++) if (ev < 100000) ev = ev * 10 + (*q - '0');
                e2 += eneg ? -ev : ev;
                p = q;
            }
        }
        if (end) *end = (char*)p;
        result = dd_finish(u64_to_dd(m), e2);
        return neg ? -result : result;
    }
    /* decimal */
    uint64_t m = 0;
    int digits = 0;      /* significant digits stored in m */
    int dropped = 0;     /* integer digits not stored */
    int fracdigs = 0;    /* stored fractional digits */
    int any = 0;
    const char* q = p;
    for (; ch_digit(*q); q++) {
        any = 1;
        if (digits < 19) { m = m * 10 + (uint64_t)(*q - '0'); if (m) digits++; }
        else dropped++;
    }
    if (*q == '.') {
        q++;
        for (; ch_digit(*q); q++) {
            any = 1;
            if (digits < 19) { m = m * 10 + (uint64_t)(*q - '0'); if (m) digits++; fracdigs++; }
        }
    }
    if (!any) { if (end) *end = (char*)s; return 0.0; }
    int ev = 0;
    if (ch_lower((unsigned char)*q) == 'e') {
        const char* r = q + 1;
        int eneg = 0;
        if (*r == '-') { eneg = 1; r++; } else if (*r == '+') r++;
        if (ch_digit(*r)) {
            for (; ch_digit(*r); r++) if (ev < 100000) ev = ev * 10 + (*r - '0');
            if (eneg) ev = -ev;
            q = r;
        }
    }
    if (end) *end = (char*)q;
    if (m == 0) return neg ? -0.0 : 0.0;
    long e10 = (long)ev + dropped - fracdigs;
    /* m has `digits` digits: value in [10^(digits-1), 10^digits) * 10^e10 */
    long mag = e10 + digits;                 /* value is about 10^mag */
    if (mag > 310) { errno = ERANGE; return neg ? -HUGE_VAL : HUGE_VAL; }
    if (mag < -330) { errno = ERANGE; return neg ? -0.0 : 0.0; }
    result = scale10(u64_to_dd(m), (int)e10);
    if (result == HUGE_VAL) errno = ERANGE;
    return neg ? -result : result;
}

float strtof(const char* __restrict s, char** __restrict end) { return (float)strtod(s, end); }
long double strtold(const char* __restrict s, char** __restrict end) { return (long double)strtod(s, end); }
double atof(const char* s) { return strtod(s, (char**)0); }

static unsigned long long conv_int(const char* s, char** end, int base, int is_signed, int* neg_out, int* overflow) {
    const char* p = s;
    while (ch_space((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    if ((base == 0 || base == 16) && p[0] == '0' && ch_lower((unsigned char)p[1]) == 'x' && hexval((unsigned char)p[2]) >= 0) { p += 2; base = 16; }
    else if (base == 0) base = (p[0] == '0') ? 8 : 10;
    unsigned long long v = 0;
    int any = 0;
    *overflow = 0;
    for (;; p++) {
        int d = hexval((unsigned char)*p);
        if (d < 0) {
            int c = ch_lower((unsigned char)*p);
            if (c >= 'a' && c <= 'z') d = c - 'a' + 10; else break;
        }
        if (d >= base) break;
        any = 1;
        if (v > (ULLONG_MAX - (unsigned)d) / (unsigned)base) *overflow = 1;
        else v = v * (unsigned)base + (unsigned)d;
    }
    if (!any) { if (end) *end = (char*)s; *neg_out = 0; return 0; }
    if (end) *end = (char*)p;
    *neg_out = neg;
    (void)is_signed;
    return v;
}

unsigned long long strtoull(const char* __restrict s, char** __restrict end, int base) {
    int neg, ov;
    unsigned long long v = conv_int(s, end, base, 0, &neg, &ov);
    if (ov) { errno = ERANGE; return ULLONG_MAX; }
    return neg ? (unsigned long long)(-(long long)v) : v;
}

long long strtoll(const char* __restrict s, char** __restrict end, int base) {
    int neg, ov;
    unsigned long long v = conv_int(s, end, base, 1, &neg, &ov);
    if (ov || (!neg && v > (unsigned long long)LLONG_MAX) || (neg && v > (unsigned long long)LLONG_MAX + 1ULL)) {
        errno = ERANGE;
        return neg ? LLONG_MIN : LLONG_MAX;
    }
    return neg ? (long long)(0ULL - v) : (long long)v;
}

long strtol(const char* __restrict s, char** __restrict end, int base) { return (long)strtoll(s, end, base); }
unsigned long strtoul(const char* __restrict s, char** __restrict end, int base) { return (unsigned long)strtoull(s, end, base); }
int atoi(const char* s) { return (int)strtol(s, (char**)0, 10); }
long atol(const char* s) { return strtol(s, (char**)0, 10); }
long long atoll(const char* s) { return strtoll(s, (char**)0, 10); }
