/* Host test: the runtime's libm (symbols prefixed fe_) against glibc. */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define D1(n) double fe_##n(double); float fe_##n##f(float);
#define D2(n) double fe_##n(double, double); float fe_##n##f(float, float);
D1(sqrt) D1(cbrt) D1(sin) D1(cos) D1(tan) D1(asin) D1(acos) D1(atan) D1(sinh) D1(cosh) D1(tanh)
D1(asinh) D1(acosh) D1(atanh) D1(exp) D1(exp2) D1(expm1) D1(log) D1(log2) D1(log10) D1(log1p)
D1(floor) D1(ceil) D1(round) D1(trunc) D1(rint) D1(fabs)
D2(atan2) D2(pow) D2(fmod) D2(hypot) D2(fmin) D2(fmax) D2(copysign) D2(remainder)
double fe_ldexp(double, int); double fe_frexp(double, int*); double fe_modf(double, double*);
double fe_fma(double, double, double);

int fe_errno_value;
static uint64_t rng = 0x9e3779b97f4a7c15ULL;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static double urand(double lo, double hi) { return lo + (hi - lo) * ((double)(rnd() >> 11) / 9007199254740992.0); }
static double lrand_(double lo, double hi) { return exp(urand(log(lo), log(hi))); }

static int64_t d2i(double d) { int64_t i; memcpy(&i, &d, 8); return i < 0 ? INT64_MIN - i : i; }
static double ulps(double a, double b) {
    if (isnan(a) && isnan(b)) return 0;
    if (isnan(a) || isnan(b)) return 1e30;
    if (isinf(a) || isinf(b)) return a == b ? 0 : 1e30;
    if (a == b) return 0;
    int64_t x = d2i(a), y = d2i(b);
    return (double)(x > y ? x - y : y - x);
}

static int failures = 0;
struct stat_ { double worst; double wx, wy; long n; };
static double fscale = 1.0;
static void rep(const char* name, struct stat_* s, double limit) {
    s->worst /= fscale;
    int bad = s->worst > limit;
    printf("  %-8s n=%-6ld worst %.2f ulp at (%.17g, %.17g)%s\n", name, s->n, s->worst, s->wx, s->wy, bad ? "  <-- FAIL" : "");
    if (bad) failures++;
}
static void upd(struct stat_* s, double x, double y, double a, double b) {
    double u = ulps(a, b);
    s->n++;
    if (u > s->worst) { s->worst = u; s->wx = x; s->wy = y; }
}

typedef double (*f1)(double);
typedef double (*f2)(double, double);
static void t1(const char* n, f1 mine, f1 ref, double lo, double hi, int logscale, double limit, int count) {
    struct stat_ s = {0};
    for (int i = 0; i < count; i++) {
        double x = logscale ? lrand_(lo, hi) : urand(lo, hi);
        if (rnd() & 1 && lo < 0) x = -fabs(x);
        upd(&s, x, mine(x), mine(x), ref(x));
    }
    rep(n, &s, limit);
}
static void t1pos(const char* n, f1 mine, f1 ref, double lo, double hi, int logscale, double limit, int count) {
    struct stat_ s = {0};
    for (int i = 0; i < count; i++) {
        double x = logscale ? lrand_(lo, hi) : urand(lo, hi);
        upd(&s, x, mine(x), mine(x), ref(x));
    }
    rep(n, &s, limit);
}
static void t2(const char* n, f2 mine, f2 ref, double xlo, double xhi, double ylo, double yhi, double limit, int count) {
    struct stat_ s = {0};
    for (int i = 0; i < count; i++) {
        double x = urand(xlo, xhi), y = urand(ylo, yhi);
        upd(&s, x, y, mine(x, y), ref(x, y));
    }
    rep(n, &s, limit);
}

static void special(void) {
    static const double v[] = { 0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, -2.0, 3.0, 0.1, 1e-310, -1e-310, 1e308, -1e308,
        INFINITY, -INFINITY, NAN, 1.5, -1.5, 2.5, -2.5, 1e300, 1e-300, 4503599627370496.0, 9007199254740993.0,
        3.0, 7.0, -7.0, 10.0, 100.0, 1000.0 };
    int n = sizeof v / sizeof v[0];
    int bad = 0;
#define CHK1(name) for (int i = 0; i < n; i++) { double a = fe_##name(v[i]), b = name(v[i]); \
    if (ulps(a, b) > 2 || (a == 0 && b == 0 && signbit(a) != signbit(b))) { printf("  special %s(%.17g): %.17g vs %.17g\n", #name, v[i], a, b); bad++; } }
    CHK1(sqrt) CHK1(sin) CHK1(cos) CHK1(tan) CHK1(asin) CHK1(acos) CHK1(atan) CHK1(exp) CHK1(exp2) CHK1(log)
    CHK1(log2) CHK1(log10) CHK1(floor) CHK1(ceil) CHK1(round) CHK1(trunc) CHK1(rint) CHK1(fabs) CHK1(cbrt)
    CHK1(sinh) CHK1(cosh) CHK1(tanh) CHK1(expm1) CHK1(log1p) CHK1(asinh) CHK1(acosh) CHK1(atanh)
#define CHK2(name) for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) { double a = fe_##name(v[i], v[j]), b = name(v[i], v[j]); \
    if (ulps(a, b) > 2 || (a == 0 && b == 0 && signbit(a) != signbit(b) && strncmp(#name, "fm", 2))) { printf("  special %s(%.17g, %.17g): %.17g vs %.17g\n", #name, v[i], v[j], a, b); bad++; } }
    CHK2(pow) CHK2(atan2) CHK2(fmod) CHK2(hypot) CHK2(fmin) CHK2(fmax) CHK2(copysign) CHK2(remainder)
    printf("  special values: %d mismatches\n", bad);
    if (bad) failures++;
}

static void exact(void) {
    int bad = 0;
    for (int k = -300; k <= 300; k++) {
        double a = fe_pow(10.0, k), b = pow(10.0, k);
        if ((k >= 0 && k <= 22) ? a != b : ulps(a, b) > 1) { if (bad++ < 5) printf("  pow(10,%d) = %.17g vs %.17g\n", k, a, b); }
    }
    for (int k = 0; k <= 22; k++) {
        double p = pow(10.0, k);
        if (fe_log10(p) != (double)k) { if (bad++ < 10) printf("  log10(1e%d) = %.17g\n", k, fe_log10(p)); }
    }
    for (int k = -1000; k <= 1023; k++) {
        double p = ldexp(1.0, k);
        if (fe_log2(p) != (double)k) { if (bad++ < 10) printf("  log2(2^%d) = %.17g\n", k, fe_log2(p)); }
        if (fe_exp2((double)k) != p) { if (bad++ < 10) printf("  exp2(%d)\n", k); }
    }
    for (int a = 2; a < 30; a++) for (int b = 0; b < 12; b++) {
        double x = fe_pow(a, b), y = pow(a, b);
        if (ulps(x, y) > 1 || (y < 9007199254740992.0 && x != y)) { if (bad++ < 10) printf("  pow(%d,%d)\n", a, b); }
    }
    printf("  exact cases: %d mismatches\n", bad);
    if (bad) failures++;
}

int main(void) {
    printf("math: double functions vs glibc (ulp)\n");
    special();
    exact();
    t1pos("sqrt", fe_sqrt, sqrt, 1e-300, 1e300, 1, 0, 20000);
    t1("sin", fe_sin, sin, 0, 8, 0, 2, 50000);
    t1("sin big", fe_sin, sin, 1e3, 1e5, 1, 2, 50000);
    t1("sin huge", fe_sin, sin, 1e6, 1e300, 1, 2, 50000);
    t1("cos", fe_cos, cos, 0, 8, 0, 2, 50000);
    t1("cos big", fe_cos, cos, 1e3, 1e5, 1, 2, 50000);
    t1("cos huge", fe_cos, cos, 1e6, 1e300, 1, 2, 50000);
    t1("tan", fe_tan, tan, 0, 8, 0, 4, 50000);
    t1("tan huge", fe_tan, tan, 1e6, 1e300, 1, 4, 50000);
    t1("asin", fe_asin, asin, -1, 1, 0, 3, 50000);
    t1("acos", fe_acos, acos, -1, 1, 0, 3, 50000);
    t1("atan", fe_atan, atan, 0, 5, 0, 2, 50000);
    t1("atan lg", fe_atan, atan, 1e-6, 1e12, 1, 2, 50000);
    t1("exp", fe_exp, exp, -700, 700, 0, 2, 50000);
    t1("exp sm", fe_exp, exp, -2, 2, 0, 2, 50000);
    t1("exp2", fe_exp2, exp2, -1000, 1000, 0, 1, 20000);
    t1("expm1", fe_expm1, expm1, -5, 5, 0, 2, 20000);
    t1("expm1sm", fe_expm1, expm1, -1e-3, 1e-3, 0, 2, 20000);
    t1pos("log", fe_log, log, 1e-300, 1e300, 1, 2, 50000);
    t1pos("log nr1", fe_log, log, 0.5, 2, 0, 2, 50000);
    t1pos("log2", fe_log2, log2, 1e-300, 1e300, 1, 1, 20000);
    t1pos("log10", fe_log10, log10, 1e-300, 1e300, 1, 1, 20000);
    t1pos("log1p", fe_log1p, log1p, 1e-8, 10, 1, 2, 20000);
    t1("sinh", fe_sinh, sinh, -20, 20, 0, 4, 20000);
    t1("cosh", fe_cosh, cosh, -20, 20, 0, 4, 20000);
    t1("tanh", fe_tanh, tanh, -5, 5, 0, 4, 20000);
    t1("asinh", fe_asinh, asinh, -100, 100, 0, 4, 20000);
    t1pos("acosh", fe_acosh, acosh, 1.0001, 1000, 0, 4, 20000);
    t1("atanh", fe_atanh, atanh, -0.99, 0.99, 0, 4, 20000);
    {   /* glibc's cbrt is only 4 ulp; use long double as the reference */
        struct stat_ s = {0};
        for (int i = 0; i < 20000; i++) { double x = urand(-1000, 1000); upd(&s, x, 0, fe_cbrt(x), (double)cbrtl((long double)x)); }
        rep("cbrt", &s, 1);
    }
    t2("atan2", fe_atan2, atan2, -10, 10, -10, 10, 2, 50000);
    t2("pow", fe_pow, pow, 0.01, 100, -20, 20, 1, 50000);
    t2("pow big", fe_pow, pow, 1.0, 2.0, -1000, 1000, 1, 50000);
    t2("pow neg", fe_pow, pow, -10, -0.1, -10, 10, 1, 20000);
    t2("fmod", fe_fmod, fmod, -1e6, 1e6, 0.001, 1000, 0, 50000);
    t2("hypot", fe_hypot, hypot, -1e10, 1e10, -1e10, 1e10, 1, 50000);
    {   /* integer-valued exponents of pow */
        struct stat_ s = {0};
        for (int i = 0; i < 20000; i++) { double x = urand(-5, 5); double y = (double)(int)urand(-30, 30); upd(&s, x, y, fe_pow(x, y), pow(x, y)); }
        rep("pow int", &s, 1);
    }
    {
        struct stat_ s = {0};
        for (int i = 0; i < 20000; i++) { double x = urand(-1e6, 1e6); upd(&s, x, 0, fe_floor(x), floor(x)); upd(&s, x, 0, fe_ceil(x), ceil(x)); upd(&s, x, 0, fe_round(x), round(x)); upd(&s, x, 0, fe_trunc(x), trunc(x)); upd(&s, x, 0, fe_rint(x), rint(x)); }
        rep("rounding", &s, 0);
    }
    {
        struct stat_ s = {0};
        for (int i = 0; i < 20000; i++) {
            double x = urand(-1e9, 1e9);
            int e1, e2; double a = fe_frexp(x, &e1), b = frexp(x, &e2);
            upd(&s, x, 0, a, b); if (e1 != e2) s.worst = 1e9;
            double i1, i2; a = fe_modf(x, &i1); b = modf(x, &i2); upd(&s, x, 0, a, b); upd(&s, x, 0, i1, i2);
            int k = (int)urand(-1100, 1100); upd(&s, x, k, fe_ldexp(x, k), ldexp(x, k));
        }
        rep("frexp etc", &s, 0);
    }
    {
        struct stat_ s = {0};
        for (int i = 0; i < 20000; i++) { float x = (float)urand(-100, 100); float a = fe_sinf(x), b = sinf(x); upd(&s, x, 0, a, b); a = fe_cosf(x); b = cosf(x); upd(&s, x, 1, a, b);
            float y = (float)urand(0.01, 50); a = fe_powf(y, x / 10); b = powf(y, x / 10); upd(&s, y, x, a, b); a = fe_sqrtf(y); b = sqrtf(y); upd(&s, y, 0, a, b);
            a = fe_atan2f(x, y); b = atan2f(x, y); upd(&s, x, y, a, b); a = fe_logf(y); b = logf(y); upd(&s, y, 0, a, b); a = fe_expf(x / 5); b = expf(x / 5); upd(&s, x, 0, a, b);
            a = fe_fmodf(x, y); b = fmodf(x, y); upd(&s, x, y, a, b); a = fe_tanf(x); b = tanf(x); upd(&s, x, y, a, b); a = fe_acosf(x / 100); b = acosf(x / 100); upd(&s, x, y, a, b); }
        fscale = 536870912.0; rep("float", &s, 1); fscale = 1.0;
    }
    if (failures) { printf("MATH TEST FAILED (%d groups)\n", failures); return 1; }
    printf("MATH TEST PASSED\n");
    return 0;
}
