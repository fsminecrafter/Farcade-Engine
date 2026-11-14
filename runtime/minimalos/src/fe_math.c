/*
 * libm for the Minimal-OS runtime. IEEE double arithmetic on SSE2 only (no
 * x87, no FMA). Accuracy: sqrt, floor, ceil, round, trunc, fmod and the
 * conversions are exact; the others are within about 2 ulp (exp, log, sin,
 * cos, tan, atan family) or close to correctly rounded (pow, log2, log10,
 * exp2 are computed in double-double). The float functions evaluate the
 * double function and round.
 *
 * Build flags: -fno-builtin -fno-math-errno -ffp-contract=off.
 */
#include <math.h>
#include <stdint.h>

static inline uint64_t d2u(double d) { uint64_t u; __builtin_memcpy(&u, &d, 8); return u; }
static inline double u2d(uint64_t u) { double d; __builtin_memcpy(&d, &u, 8); return d; }

#define SIGN 0x8000000000000000ULL
#define EXPM 0x7ff0000000000000ULL

#define LN2_HI     6.93147180369123816490e-01
#define LN2_LO     1.90821492927058770002e-10
#define INV_LN2    1.44269504088896338700e+00
#define SQRT2      1.41421356237309504880
#define PIO2_HI    1.5707963267948966
#define PIO2_LO    6.123233995736766e-17
#define PI_HI      3.141592653589793
#define PI_LO      1.2246467991473532e-16

/* ---- basic helpers ------------------------------------------------- */

double fabs(double x) { return u2d(d2u(x) & ~SIGN); }
double copysign(double x, double y) { return u2d((d2u(x) & ~SIGN) | (d2u(y) & SIGN)); }
double sqrt(double x) { return __builtin_sqrt(x); }

double trunc(double x) {
    uint64_t u = d2u(x);
    int e = (int)((u >> 52) & 0x7ff) - 1023;
    if (e < 0) return u2d(u & SIGN);
    if (e >= 52) return x;
    return u2d(u & ~((~0ULL >> 12) >> e));
}

double floor(double x) {
    double t = trunc(x);
    if (x < t) t -= 1.0;
    return t;
}

double ceil(double x) {
    double t = trunc(x);
    if (x > t) t += 1.0;
    return t;
}

double round(double x) {
    double t = trunc(x);
    if (fabs(x - t) >= 0.5) t += copysign(1.0, x);
    return t;
}

double rint(double x) {
    const double M = 4503599627370496.0; /* 2^52 */
    double ax = fabs(x);
    if (!(ax < M)) return x;
    volatile double r = (ax + M);
    r = r - M;
    return copysign(r, x);
}
double nearbyint(double x) { return rint(x); }

long lrint(double x) { return (long)rint(x); }
long lround(double x) { return (long)round(x); }
long long llrint(double x) { return (long long)rint(x); }
long long llround(double x) { return (long long)round(x); }

double scalbn(double x, int n) {
    if (n > 1023) {
        x *= 0x1p1023; n -= 1023;
        if (n > 1023) { x *= 0x1p1023; n -= 1023; if (n > 1023) n = 1023; }
    } else if (n < -1022) {
        x *= 0x1p-1022 * 0x1p53; n += 1022 - 53;
        if (n < -1022) {
            x *= 0x1p-1022 * 0x1p53; n += 1022 - 53;
            if (n < -1022) n = -1022;
        }
    }
    return x * u2d((uint64_t)(0x3ff + n) << 52);
}
double ldexp(double x, int n) { return scalbn(x, n); }

double frexp(double x, int* e) {
    uint64_t u = d2u(x);
    int ee = (int)((u >> 52) & 0x7ff);
    if (!ee) {
        if (x != 0.0) {
            x = frexp(x * 0x1p64, e);
            *e -= 64;
        } else *e = 0;
        return x;
    }
    if (ee == 0x7ff) { *e = 0; return x; }
    *e = ee - 0x3fe;
    u &= ~EXPM & ~0ULL; /* keep sign + mantissa */
    u |= 0x3fe0000000000000ULL;
    return u2d(u);
}

double modf(double x, double* ip) {
    double t = trunc(x);
    if (!(fabs(x) < INFINITY)) { *ip = x; return x != x ? x : copysign(0.0, x); }
    *ip = t;
    return copysign(x - t, x);
}

int ilogb(double x) {
    uint64_t u = d2u(x) & ~SIGN;
    int e = (int)(u >> 52);
    if (e == 0x7ff) return u == EXPM ? 0x7fffffff : (-0x7fffffff - 1);
    if (e == 0) {
        if (!u) return -0x7fffffff - 1;
        return (63 - __builtin_clzll(u)) - 1074;
    }
    return e - 1023;
}
double logb(double x) {
    if (x == 0) return -HUGE_VAL;
    if (!(fabs(x) < INFINITY)) return fabs(x);
    return (double)ilogb(x);
}

double fmin(double a, double b) { if (a != a) return b; if (b != b) return a; return a < b ? a : b; }
double fmax(double a, double b) { if (a != a) return b; if (b != b) return a; return a > b ? a : b; }
double fdim(double a, double b) { if (a != a || b != b) return a + b; return a > b ? a - b : 0.0; }

double nan(const char* s) { (void)s; return __builtin_nan(""); }

double nextafter(double x, double y) {
    if (x != x || y != y) return x + y;
    if (x == y) return y;
    uint64_t u = d2u(x);
    if (x == 0.0) return copysign(u2d(1), y);
    if ((x < y) == (x > 0.0)) u++; else u--;
    return u2d(u);
}

double fmod(double x, double y) {
    uint64_t uxi = d2u(x), uyi = d2u(y);
    int ex = (int)(uxi >> 52 & 0x7ff), ey = (int)(uyi >> 52 & 0x7ff);
    uint64_t sx = uxi & SIGN, i;
    if ((uyi << 1) == 0 || y != y || ex == 0x7ff) return (x * y) / (x * y);
    if ((uxi << 1) <= (uyi << 1)) {
        if ((uxi << 1) == (uyi << 1)) return 0 * x;
        return x;
    }
    if (!ex) {
        for (i = uxi << 12; !(i >> 63); ex--, i <<= 1) {}
        uxi <<= -ex + 1;
    } else { uxi &= ~0ULL >> 12; uxi |= 1ULL << 52; }
    if (!ey) {
        for (i = uyi << 12; !(i >> 63); ey--, i <<= 1) {}
        uyi <<= -ey + 1;
    } else { uyi &= ~0ULL >> 12; uyi |= 1ULL << 52; }
    for (; ex > ey; ex--) {
        i = uxi - uyi;
        if (!(i >> 63)) { if (i == 0) return 0 * x; uxi = i; }
        uxi <<= 1;
    }
    i = uxi - uyi;
    if (!(i >> 63)) { if (i == 0) return 0 * x; uxi = i; }
    for (; (uxi >> 52) == 0; uxi <<= 1, ex--) {}
    if (ex > 0) { uxi -= 1ULL << 52; uxi |= (uint64_t)ex << 52; }
    else uxi >>= -ex + 1;
    uxi |= sx;
    return u2d(uxi);
}

double remainder(double x, double y) {
    if (x != x || y != y || !(fabs(x) < INFINITY) || y == 0.0) return (x * y) / (x * y);
    if (!(fabs(y) < INFINITY)) return x;
    double r = fmod(x, y);
    double ay = fabs(y), ar = fabs(r);
    if (ar > 0.5 * ay || (ar == 0.5 * ay && fmod(fabs(x - r) / ay, 2.0) != 0.0))
        r = (r > 0) ? r - ay : r + ay;
    if (r == 0.0) r = copysign(0.0, x);
    return r;
}

#include "fe_dd.h"

static const dd_t DD_LN2 = { 0.6931471805599453, 2.3190468138462996e-17 };
static const dd_t DD_INV_LN2 = { 1.4426950408889634, 2.0355273740931033e-17 };
static const dd_t DD_INV_LN10 = { 0.4342944819032518, 1.098319650216765e-17 };
static const dd_t DD_PIO2 = { 1.5707963267948966, 6.123233995736766e-17 };

/* exp of a double-double argument (|a| < ~746), result as double-double
 * scaled by the returned exponent so that nothing overflows early. */
static dd_t exp_dd_k(dd_t a, int* kout) {
    double kd = rint(a.hi * INV_LN2);
    int k = (int)kd;
    dd_t r = dd_sub(a, dd_mul_d(DD_LN2, kd));
    r.hi *= 0x1p-9; r.lo *= 0x1p-9;
    dd_t s = r, t = r;
    for (int i = 2; i <= 9; i++) {
        t = dd_mul(t, r);
        dd_t di = { (double)i, 0.0 };
        t = dd_div(t, di);
        s = dd_add(s, t);
    }
    /* s = e^r - 1; square 9 times: (1+s)^2 - 1 = s * (s + 2) */
    for (int i = 0; i < 9; i++) s = dd_mul(s, dd_add_d(s, 2.0));
    *kout = k;
    return dd_add_d(s, 1.0);
}

static double dd_to_double_scaled(dd_t r, int k) {
    return scalbn(r.hi, k) + scalbn(r.lo, k);
}

/* log of a positive finite double as double-double. */
static dd_t log_dd(double x) {
    uint64_t u = d2u(x);
    int e = 0;
    if (!(u >> 52)) { x *= 0x1p54; e = -54; u = d2u(x); }
    e += (int)(u >> 52) - 1023;
    u = (u & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;
    double m = u2d(u);
    if (m > SQRT2) { m *= 0.5; e += 1; }
    double y0 = log(m);
    dd_t res;
    if (m == 1.0) { res.hi = 0.0; res.lo = 0.0; }
    else {
        dd_t na = { -y0, 0.0 };
        int k;
        dd_t ex = exp_dd_k(na, &k);
        ex = dd_scale(ex, k);
        dd_t t = dd_add_d(dd_mul_d(ex, m), -1.0);
        dd_t y = { y0, 0.0 };
        res = dd_add(y, t);
    }
    if (e) res = dd_add(res, dd_mul_d(DD_LN2, (double)e));
    return res;
}

/* ---- exp, log ------------------------------------------------------- */

double exp(double x) {
    if (x != x) return x;
    if (x > 709.782712893384) return HUGE_VAL;
    if (x < -745.1332191019412) return 0.0;
    double kd = rint(x * INV_LN2);
    int k = (int)kd;
    double r = (x - kd * LN2_HI) - kd * LN2_LO;
    double p = 1.0 / 6227020800.0;
    p = p * r + 1.0 / 479001600.0;
    p = p * r + 1.0 / 39916800.0;
    p = p * r + 1.0 / 3628800.0;
    p = p * r + 1.0 / 362880.0;
    p = p * r + 1.0 / 40320.0;
    p = p * r + 1.0 / 5040.0;
    p = p * r + 1.0 / 720.0;
    p = p * r + 1.0 / 120.0;
    p = p * r + 1.0 / 24.0;
    p = p * r + 1.0 / 6.0;
    p = p * r + 0.5;
    double q = r * r * p;
    double res = 1.0 + (r + q);
    return scalbn(res, k);
}

double log(double x) {
    if (x != x) return x;
    if (x < 0) return NAN;
    if (x == 0) return -HUGE_VAL;
    if (x == INFINITY) return x;
    uint64_t u = d2u(x);
    int e = 0;
    if (!(u >> 52)) { x *= 0x1p54; e = -54; u = d2u(x); }
    e += (int)(u >> 52) - 1023;
    u = (u & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;
    double m = u2d(u);
    if (m > SQRT2) { m *= 0.5; e += 1; }
    double f = m - 1.0;
    double dk = (double)e;
    double s = f / (2.0 + f);
    double z = s * s;
    double R = 2.0 / 27.0;
    R = R * z + 2.0 / 25.0;
    R = R * z + 2.0 / 23.0;
    R = R * z + 2.0 / 21.0;
    R = R * z + 2.0 / 19.0;
    R = R * z + 2.0 / 17.0;
    R = R * z + 2.0 / 15.0;
    R = R * z + 2.0 / 13.0;
    R = R * z + 2.0 / 11.0;
    R = R * z + 2.0 / 9.0;
    R = R * z + 2.0 / 7.0;
    R = R * z + 2.0 / 5.0;
    R = R * z + 2.0 / 3.0;
    R = R * z;
    double hfsq = 0.5 * f * f;
    return dk * LN2_HI - ((hfsq - (s * (hfsq + R) + dk * LN2_LO)) - f);
}

double log2(double x) {
    if (x != x || x <= 0 || x == INFINITY) return log(x);
    dd_t l = dd_mul(log_dd(x), DD_INV_LN2);
    return l.hi + l.lo;
}

double log10(double x) {
    if (x != x || x <= 0 || x == INFINITY) return log(x);
    dd_t l = dd_mul(log_dd(x), DD_INV_LN10);
    return l.hi + l.lo;
}

double exp2(double x) {
    if (x != x) return x;
    if (x >= 1024.0) return HUGE_VAL;
    if (x < -1080.0) return 0.0;
    if (x == trunc(x)) return scalbn(1.0, (int)x);
    dd_t a = dd_mul_d(DD_LN2, x);
    int k;
    dd_t r = exp_dd_k(a, &k);
    return dd_to_double_scaled(r, k);
}

double expm1(double x) {
    if (x != x) return x;
    if (x > 709.782712893384) return HUGE_VAL;
    if (x < -40.0) return -1.0;
    if (fabs(x) < 0x1p-60) return x;
    dd_t a = { x, 0.0 };
    int k;
    dd_t r = exp_dd_k(a, &k);
    r = dd_scale(r, k);
    r = dd_add_d(r, -1.0);
    return r.hi + r.lo;
}

double log1p(double x) {
    if (x != x) return x;
    if (x < -1.0) return NAN;
    if (x == -1.0) return -HUGE_VAL;
    if (x == INFINITY) return x;
    double u = 1.0 + x;
    if (u == 1.0) return x;
    return log(u) * (x / (u - 1.0));
}

/* ---- pow ------------------------------------------------------------ */

static int is_int(double y) { return fabs(y) >= 0x1p53 || y == trunc(y); }
static int is_odd_int(double y) { return fabs(y) < 0x1p53 && y == trunc(y) && fmod(fabs(y), 2.0) == 1.0; }

double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 1.0) return 1.0;
    if (x != x || y != y) return x + y;
    double ax = fabs(x), ay = fabs(y);
    int yodd = is_odd_int(y), yint = is_int(y);
    if (ay == INFINITY) {
        if (ax == 1.0) return 1.0;
        if ((ax < 1.0) == (y > 0)) return 0.0;
        return HUGE_VAL;
    }
    if (ax == INFINITY) {
        if (x > 0) return y > 0 ? HUGE_VAL : 0.0;
        if (y > 0) return yodd ? -HUGE_VAL : HUGE_VAL;
        return yodd ? -0.0 : 0.0;
    }
    if (ax == 0.0) {
        int neg = signbit(x) && yodd;
        if (y < 0) return neg ? -HUGE_VAL : HUGE_VAL;
        return neg ? -0.0 : 0.0;
    }
    int neg = 0;
    if (x < 0) {
        if (!yint) return NAN;
        neg = yodd;
    }
    double t = log(ax) * y;
    double res;
    if (ax == 1.0) res = 1.0;
    else if (t > 800.0) res = HUGE_VAL;
    else if (t < -800.0) res = 0.0;
    else {
        dd_t p = dd_mul_d(log_dd(ax), y);
        if (p.hi > 709.8) res = HUGE_VAL;
        else if (p.hi < -745.2) res = 0.0;
        else {
            int k;
            dd_t r = exp_dd_k(p, &k);
            res = dd_to_double_scaled(r, k);
        }
    }
    return neg ? -res : res;
}

/* ---- trigonometry --------------------------------------------------- */

static double ksin(double x, double y) {
    double z = x * x;
    double p = -1.0 / 25852016738884976640000.0;       /* -1/23! */
    p = p * z + 1.0 / 51090942171709440000.0;          /* 1/21! */
    p = p * z - 1.0 / 121645100408832000.0;            /* 1/19! */
    p = p * z + 1.0 / 355687428096000.0;               /* 1/17! */
    p = p * z - 1.0 / 1307674368000.0;                 /* 1/15! */
    p = p * z + 1.0 / 6227020800.0;                    /* 1/13! */
    p = p * z - 1.0 / 39916800.0;                      /* 1/11! */
    p = p * z + 1.0 / 362880.0;                        /* 1/9! */
    p = p * z - 1.0 / 5040.0;                          /* 1/7! */
    p = p * z + 1.0 / 120.0;                           /* 1/5! */
    p = p * z - 1.0 / 6.0;                             /* 1/3! */
    return x + (x * z * p + y * (1.0 - 0.5 * z));
}

static double kcos(double x, double y) {
    double z = x * x;
    double c = 1.0 / 620448401733239439360000.0;       /* +1/24! */
    c = c * z - 1.0 / 1124000727777607680000.0;        /* -1/22! */
    c = c * z + 1.0 / 2432902008176640000.0;           /* +1/20! */
    c = c * z - 1.0 / 6402373705728000.0;              /* -1/18! */
    c = c * z + 1.0 / 20922789888000.0;                /* +1/16! */
    c = c * z - 1.0 / 87178291200.0;                   /* -1/14! */
    c = c * z + 1.0 / 479001600.0;                     /* +1/12! */
    c = c * z - 1.0 / 3628800.0;                       /* -1/10! */
    c = c * z + 1.0 / 40320.0;                         /* +1/8! */
    c = c * z - 1.0 / 720.0;                           /* -1/6! */
    c = c * z + 1.0 / 24.0;                            /* +1/4! */
    double hz = 0.5 * z;
    double w = 1.0 - hz;
    return w + (((1.0 - w) - hz) + (z * z * c - x * y));
}

/* Reduce x modulo pi/2: returns n mod 4, x = n*pi/2 + (y0 + y1). */
static int rem_pio2_large(double x, double* y0, double* y1) {
    static const uint64_t W[24] = {
        0xa2f9836e4e441529ULL, 0xfc2757d1f534ddc0ULL, 0xdb6295993c439041ULL, 0xfe5163abdebbc561ULL,
        0xb7246e3a424dd2e0ULL, 0x06492eea09d1921cULL, 0xfe1deb1cb129a73eULL, 0xe88235f52ebb4484ULL,
        0xe99c7026b45f7e41ULL, 0x3991d639835339f4ULL, 0x9c845f8bbdf9283bULL, 0x1ff897ffde05980fULL,
        0xef2f118b5a0a6d1fULL, 0x6d367ecf27cb09b7ULL, 0x4f463f669e5fea2dULL, 0x7527bac7ebe5f17bULL,
        0x3d0739f78a5292eaULL, 0x6bfb5fb11f8d5d08ULL, 0x56033046fc7b6babULL, 0xf0cfbc209af4361dULL,
        0xa9e391615ee61b08ULL, 0x6599855f14a06840ULL, 0x8dffd8804d732731ULL, 0x06061556ca73a8c9ULL };
    uint64_t u = d2u(x);
    int neg = (int)(u >> 63);
    int ex = (int)((u >> 52) & 0x7ff);
    uint64_t M = (u & 0x000fffffffffffffULL) | (1ULL << 52);
    int q = ex - 1075;
    uint64_t acc[3] = { 0, 0, 0 };
    int j0 = q >= 2 ? (q - 2) / 64 : 0;
    int j1 = (q + 244) / 64 + 1;
    if (j1 > 23) j1 = 23;
    for (int j = j0; j <= j1; j++) {
        unsigned __int128 P = (unsigned __int128)M * W[j];
        int o = q - 64 * (j + 1) + 128;
        uint64_t parts[2] = { (uint64_t)P, (uint64_t)(P >> 64) };
        for (int h = 0; h < 2; h++) {
            uint64_t v = parts[h];
            int oo = o + 64 * h;
            if (oo >= 192 || oo <= -64) continue;
            if (oo < 0) { v >>= -oo; oo = 0; }
            int li = oo >> 6, bs = oo & 63;
            unsigned __int128 t = (unsigned __int128)v << bs;
            uint64_t w[2] = { (uint64_t)t, (uint64_t)(t >> 64) };
            for (int c = 0; c < 2; c++) {
                int l = li + c;
                uint64_t add = w[c];
                while (l < 3 && add) {
                    uint64_t s = acc[l] + add;
                    add = s < add ? 1 : 0;
                    acc[l] = s;
                    l++;
                }
            }
        }
    }
    int n = (int)(acc[2] & 3);
    unsigned __int128 frac = ((unsigned __int128)acc[1] << 64) | acc[0];
    int fneg = 0;
    if (frac >> 127) { n = (n + 1) & 3; frac = -frac; fneg = 1; }
    dd_t f = { 0.0, 0.0 };
    if (frac) {
        int lz = 0;
        unsigned __int128 t = frac;
        while (!(t >> 127)) { t <<= 1; lz++; }
        uint64_t top = (uint64_t)(t >> 64), low = (uint64_t)t;
        double fh = (double)(top >> 11) * scalbn(1.0, -(lz + 53));
        double fl = (double)(top & 0x7ff) * scalbn(1.0, -(lz + 64)) + (double)(low >> 11) * scalbn(1.0, -(lz + 128 - 11));
        f.hi = fh; f.lo = fl;
        f = quick_two_sum(f.hi, f.lo);
    }
    if (fneg) f = dd_neg(f);
    dd_t y = dd_mul(f, DD_PIO2);
    if (neg) { y = dd_neg(y); n = (-n) & 3; }
    *y0 = y.hi; *y1 = y.lo;
    return n;
}

static int rem_pio2(double x, double* y0, double* y1) {
    double ax = fabs(x);
    if (ax < 0x1p19) {
        double fn = rint(ax * 0.63661977236758134308);
        double a = ax - fn * 1.5707963267341256;
        dd_t s1 = two_sum(a, -(fn * 6.077100506303966e-11));
        dd_t s2 = two_sum(s1.hi, -(fn * 2.0222662487111665e-21));
        double lo = s1.lo + s2.lo - fn * 8.4784276603689e-32;
        dd_t y = quick_two_sum(s2.hi, lo);
        int n = (int)fn & 3;
        if (x < 0) { y.hi = -y.hi; y.lo = -y.lo; n = (-n) & 3; }
        *y0 = y.hi; *y1 = y.lo;
        return n;
    }
    return rem_pio2_large(x, y0, y1);
}

double sin(double x) {
    if (!(fabs(x) < INFINITY)) return (x - x) / (x - x);
    if (fabs(x) <= 0.7853981633974483) {
        if (fabs(x) < 0x1p-26) return x;
        return ksin(x, 0.0);
    }
    double y0, y1;
    int n = rem_pio2(x, &y0, &y1);
    switch (n) {
    case 0: return ksin(y0, y1);
    case 1: return kcos(y0, y1);
    case 2: return -ksin(y0, y1);
    default: return -kcos(y0, y1);
    }
}

double cos(double x) {
    if (!(fabs(x) < INFINITY)) return (x - x) / (x - x);
    if (fabs(x) <= 0.7853981633974483) {
        if (fabs(x) < 0x1p-27) return 1.0;
        return kcos(x, 0.0);
    }
    double y0, y1;
    int n = rem_pio2(x, &y0, &y1);
    switch (n) {
    case 0: return kcos(y0, y1);
    case 1: return -ksin(y0, y1);
    case 2: return -kcos(y0, y1);
    default: return ksin(y0, y1);
    }
}

double tan(double x) {
    if (!(fabs(x) < INFINITY)) return (x - x) / (x - x);
    if (fabs(x) < 0x1p-27) return x;
    double y0 = x, y1 = 0.0;
    int n = 0;
    if (fabs(x) > 0.7853981633974483) n = rem_pio2(x, &y0, &y1);
    double s = ksin(y0, y1), c = kcos(y0, y1);
    return (n & 1) ? -c / s : s / c;
}

/* ---- inverse trigonometry ------------------------------------------- */

static const double atanhi[] = { 4.636476090008061e-01, 7.853981633974483e-01,
                                 9.827937232473290e-01, 1.5707963267948966e+00 };
static const double atanlo[] = { 2.2698777452961687e-17, 3.061616997868383e-17,
                                 1.3903311031230998e-17, 6.123233995736766e-17 };

double atan(double x) {
    if (x != x) return x;
    double ax = fabs(x);
    if (ax >= 0x1p66) return copysign(PIO2_HI, x);
    int id;
    double t;
    if (ax < 0.4375) {
        if (ax < 0x1p-27) return x;
        id = -1; t = ax;
    } else if (ax < 0.6875) { id = 0; t = (ax - 0.5) / (1.0 + 0.5 * ax); }
    else if (ax < 1.1875) { id = 1; t = (ax - 1.0) / (1.0 + ax); }
    else if (ax < 2.4375) { id = 2; t = (ax - 1.5) / (1.0 + 1.5 * ax); }
    else { id = 3; t = -1.0 / ax; }
    double z = t * t;
    /* atan t = t - t*z*(1/3 - z/5 + z^2/7 - ...) */
    double p = -1.0 / 47.0;
    p = p * z + 1.0 / 45.0; p = p * z - 1.0 / 43.0; p = p * z + 1.0 / 41.0;
    p = p * z - 1.0 / 39.0; p = p * z + 1.0 / 37.0; p = p * z - 1.0 / 35.0;
    p = p * z + 1.0 / 33.0; p = p * z - 1.0 / 31.0; p = p * z + 1.0 / 29.0;
    p = p * z - 1.0 / 27.0; p = p * z + 1.0 / 25.0; p = p * z - 1.0 / 23.0;
    p = p * z + 1.0 / 21.0; p = p * z - 1.0 / 19.0; p = p * z + 1.0 / 17.0;
    p = p * z - 1.0 / 15.0; p = p * z + 1.0 / 13.0; p = p * z - 1.0 / 11.0;
    p = p * z + 1.0 / 9.0; p = p * z - 1.0 / 7.0; p = p * z + 1.0 / 5.0;
    p = p * z - 1.0 / 3.0;
    double s = t * z * (-p);   /* positive-leading series: 1/3 - ... */
    double r;
    if (id < 0) r = t - s;
    else r = atanhi[id] - ((s - atanlo[id]) - t);
    return copysign(r, x);
}

double atan2(double y, double x) {
    if (x != x || y != y) return x + y;
    if (x == 1.0) return atan(y);
    int sx = (int)signbit(x), sy = (int)signbit(y);
    double ax = fabs(x), ay = fabs(y);
    if (ay == 0.0) {
        if (!sx) return y;               /* +-0 */
        return sy ? -PI_HI : PI_HI;      /* x < 0 or -0 */
    }
    if (ax == 0.0) return sy ? -PIO2_HI : PIO2_HI;
    if (ax == INFINITY) {
        if (ay == INFINITY) {
            double r = sx ? 3.0 * 0.7853981633974483 + 3.0 * 3.061616997868383e-17 : 0.7853981633974483;
            return sy ? -r : r;
        }
        if (sx) return sy ? -PI_HI : PI_HI;
        return sy ? -0.0 : 0.0;
    }
    if (ay == INFINITY) return sy ? -PIO2_HI : PIO2_HI;
    double z;
    if (ay > ax * 0x1p60) z = PIO2_HI;          /* |y/x| huge */
    else if (ax > ay * 0x1p60 && sx) z = 0.0;   /* tiny, x < 0 */
    else z = atan(ay / ax);
    if (sx) z = PI_HI - (z - PI_LO);
    return sy ? -z : z;
}

double asin(double x) {
    if (x != x) return x;
    double ax = fabs(x);
    if (ax > 1.0) return (x - x) / (x - x);
    if (ax < 0x1p-27) return x;
    if (ax == 1.0) return copysign(PIO2_HI, x);
    return atan2(x, sqrt((1.0 - x) * (1.0 + x)));
}

double acos(double x) {
    if (x != x) return x;
    if (fabs(x) > 1.0) return (x - x) / (x - x);
    return atan2(sqrt((1.0 - x) * (1.0 + x)), x);
}

/* ---- hyperbolics and friends ---------------------------------------- */

double sinh(double x) {
    double ax = fabs(x);
    if (!(ax < INFINITY) || x != x) return x;
    if (ax < 0x1p-27) return x;
    double r;
    if (ax > 709.0) {
        double h = exp(0.5 * ax);
        r = 0.5 * h * h;
    } else {
        double em = expm1(ax);
        r = 0.5 * (em + em / (em + 1.0));
    }
    return copysign(r, x);
}

double cosh(double x) {
    double ax = fabs(x);
    if (x != x) return x;
    if (ax > 709.0) { double h = exp(0.5 * ax); return 0.5 * h * h; }
    double e = exp(ax);
    return 0.5 * (e + 1.0 / e);
}

double tanh(double x) {
    if (x != x) return x;
    double ax = fabs(x);
    if (ax > 22.0) return copysign(1.0, x);
    if (ax < 0x1p-27) return x;
    double em = expm1(2.0 * ax);
    return copysign(em / (em + 2.0), x);
}

double asinh(double x) {
    if (x != x || !(fabs(x) < INFINITY)) return x;
    double ax = fabs(x), r;
    if (ax < 0x1p-27) return x;
    if (ax > 0x1p28) r = log(ax) + M_LN2;
    else r = log1p(ax + ax * ax / (1.0 + sqrt(1.0 + ax * ax)));
    return copysign(r, x);
}

double acosh(double x) {
    if (x != x) return x;
    if (x < 1.0) return NAN;
    if (x > 0x1p28) return log(x) + M_LN2;
    double t = x - 1.0;
    return log1p(t + sqrt(2.0 * t + t * t));
}

double atanh(double x) {
    if (x != x) return x;
    double ax = fabs(x);
    if (ax > 1.0) return NAN;
    if (ax == 1.0) return copysign(HUGE_VAL, x);
    if (ax < 0x1p-27) return x;
    return copysign(0.5 * log1p(2.0 * ax / (1.0 - ax)), x);
}

double cbrt(double x) {
    if (x != x || !(fabs(x) < INFINITY) || x == 0.0) return x;
    double ax = fabs(x);
    int e;
    double m = frexp(ax, &e);               /* ax = m * 2^e, m in [0.5, 1) */
    int r = e % 3;
    if (r < 0) r += 3;
    m = scalbn(m, r);                        /* m in [0.5, 4) */
    e = (e - r) / 3;
    double y = 0.6 + 0.4 * m * 0.5;          /* rough start */
    for (int i = 0; i < 6; i++) y = y - (y * y * y - m) / (3.0 * y * y);
    /* final correction in double-double: y += (m - y^3) / (3 y^2) */
    dd_t y3 = dd_mul_d(two_prod(y, y), y);
    double corr = ((m - y3.hi) - y3.lo) / (3.0 * y * y);
    y += corr;
    return copysign(scalbn(y, e), x);
}

double hypot(double x, double y) {
    double ax = fabs(x), ay = fabs(y);
    if (ax == INFINITY || ay == INFINITY) return HUGE_VAL;
    if (x != x || y != y) return x + y;
    if (ax < ay) { double t = ax; ax = ay; ay = t; }
    if (ay == 0.0) return ax;
    int e;
    frexp(ax, &e);
    double sx = scalbn(ax, -e), sy = scalbn(ay, -e);
    dd_t a = two_prod(sx, sx), b = two_prod(sy, sy);
    dd_t s = dd_add(a, b);
    double r = sqrt(s.hi);
    /* one Newton correction using the low part */
    r = r + ((s.hi - r * r) + s.lo) / (2.0 * r);
    return scalbn(r, e);
}

double fma(double x, double y, double z) {
    dd_t p = two_prod(x, y);
    dd_t s = two_sum(p.hi, z);
    return s.hi + (s.lo + p.lo);
}

/* ---- float versions: evaluate in double and round ------------------- */

#define F1(n) float n##f(float x) { return (float)n((double)x); }
#define F2(n) float n##f(float x, float y) { return (float)n((double)x, (double)y); }
float sqrtf(float x) { return __builtin_sqrtf(x); }
F1(cbrt) F1(sin) F1(cos) F1(tan) F1(asin) F1(acos) F1(atan)
F1(sinh) F1(cosh) F1(tanh) F1(asinh) F1(acosh) F1(atanh)
F1(exp) F1(exp2) F1(expm1) F1(log) F1(log2) F1(log10) F1(log1p) F1(logb)
F1(floor) F1(ceil) F1(round) F1(trunc) F1(rint) F1(nearbyint) F1(fabs)
F2(atan2) F2(pow) F2(fmod) F2(hypot) F2(fmin) F2(fmax) F2(fdim) F2(copysign) F2(remainder)
float fmaf(float x, float y, float z) { return (float)((double)x * (double)y + (double)z); }
float ldexpf(float x, int n) { return (float)scalbn((double)x, n); }
float scalbnf(float x, int n) { return (float)scalbn((double)x, n); }
float frexpf(float x, int* e) { return (float)frexp((double)x, e); }
float modff(float x, float* ip) { double i; double f = modf((double)x, &i); *ip = (float)i; return (float)f; }
int ilogbf(float x) { return ilogb((double)x); }
long lrintf(float x) { return lrint((double)x); }
long lroundf(float x) { return lround((double)x); }
float nanf(const char* s) { (void)s; return __builtin_nanf(""); }
float nextafterf(float x, float y) {
    if (x != x || y != y) return x + y;
    if (x == y) return y;
    uint32_t u;
    __builtin_memcpy(&u, &x, 4);
    if (x == 0.0f) { u = 1; if (signbit(y)) u |= 0x80000000u; float r; __builtin_memcpy(&r, &u, 4); return r; }
    if ((x < y) == (x > 0.0f)) u++; else u--;
    float r; __builtin_memcpy(&r, &u, 4); return r;
}
