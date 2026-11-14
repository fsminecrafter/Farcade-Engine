/* Double-double helpers shared by fe_math.c and fe_strtod.c. Needs scalbn(). */
#ifndef FE_DD_H
#define FE_DD_H
typedef struct { double hi, lo; } dd_t;
double scalbn(double, int);

static inline dd_t two_sum(double a, double b) {
    double s = a + b, bb = s - a;
    dd_t r = { s, (a - (s - bb)) + (b - bb) };
    return r;
}
static inline dd_t quick_two_sum(double a, double b) {
    double s = a + b;
    dd_t r = { s, b - (s - a) };
    return r;
}
static inline dd_t two_prod(double a, double b) {
    double p = a * b;
    double t = 134217729.0 * a, ah = t - (t - a), al = a - ah;
    t = 134217729.0 * b;
    double bh = t - (t - b), bl = b - bh;
    dd_t r = { p, ((ah * bh - p) + ah * bl + al * bh) + al * bl };
    return r;
}
static inline dd_t dd_add(dd_t a, dd_t b) {
    dd_t s = two_sum(a.hi, b.hi), t = two_sum(a.lo, b.lo);
    s.lo += t.hi;
    s = quick_two_sum(s.hi, s.lo);
    s.lo += t.lo;
    return quick_two_sum(s.hi, s.lo);
}
static inline dd_t dd_neg(dd_t a) { dd_t r = { -a.hi, -a.lo }; return r; }
static inline dd_t dd_sub(dd_t a, dd_t b) { return dd_add(a, dd_neg(b)); }
static inline dd_t dd_add_d(dd_t a, double b) { dd_t y = { b, 0.0 }; return dd_add(a, y); }
static inline dd_t dd_mul(dd_t a, dd_t b) {
    dd_t p = two_prod(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return quick_two_sum(p.hi, p.lo);
}
static inline dd_t dd_mul_d(dd_t a, double b) {
    dd_t p = two_prod(a.hi, b);
    p.lo += a.lo * b;
    return quick_two_sum(p.hi, p.lo);
}
static inline dd_t dd_div(dd_t a, dd_t b) {
    double q1 = a.hi / b.hi;
    dd_t r = dd_sub(a, dd_mul_d(b, q1));
    double q2 = r.hi / b.hi;
    r = dd_sub(r, dd_mul_d(b, q2));
    double q3 = r.hi / b.hi;
    dd_t q = quick_two_sum(q1, q2);
    return dd_add_d(q, q3);
}
static inline dd_t dd_scale(dd_t a, int k) {
    dd_t r = { scalbn(a.hi, k), scalbn(a.lo, k) };
    return r;
}

#endif
