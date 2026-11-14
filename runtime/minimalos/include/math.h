/* math.h of the Minimal-OS runtime: float and double, SSE2 only. */
#ifndef FE_MATH_H
#define FE_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

#define HUGE_VAL   (__builtin_huge_val())
#define HUGE_VALF  (__builtin_huge_valf())
#define INFINITY   (__builtin_inff())
#define NAN        (__builtin_nanf(""))

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf_sign(x)
#define isfinite(x)   __builtin_isfinite(x)
#define isnormal(x)   __builtin_isnormal(x)
#define signbit(x)    __builtin_signbit(x)
#define isgreater(a, b)      __builtin_isgreater(a, b)
#define isgreaterequal(a, b) __builtin_isgreaterequal(a, b)
#define isless(a, b)         __builtin_isless(a, b)
#define islessequal(a, b)    __builtin_islessequal(a, b)
#define isunordered(a, b)    __builtin_isunordered(a, b)

typedef float  float_t;
typedef double double_t;

#define FE_MATH_DECL(name) \
    double name(double); float name##f(float);
#define FE_MATH_DECL2(name) \
    double name(double, double); float name##f(float, float);

FE_MATH_DECL(sqrt) FE_MATH_DECL(cbrt)
FE_MATH_DECL(sin) FE_MATH_DECL(cos) FE_MATH_DECL(tan)
FE_MATH_DECL(asin) FE_MATH_DECL(acos) FE_MATH_DECL(atan)
FE_MATH_DECL(sinh) FE_MATH_DECL(cosh) FE_MATH_DECL(tanh)
FE_MATH_DECL(asinh) FE_MATH_DECL(acosh) FE_MATH_DECL(atanh)
FE_MATH_DECL(exp) FE_MATH_DECL(exp2) FE_MATH_DECL(expm1)
FE_MATH_DECL(log) FE_MATH_DECL(log2) FE_MATH_DECL(log10) FE_MATH_DECL(log1p)
FE_MATH_DECL(logb)
FE_MATH_DECL(floor) FE_MATH_DECL(ceil) FE_MATH_DECL(round) FE_MATH_DECL(trunc)
FE_MATH_DECL(rint) FE_MATH_DECL(nearbyint) FE_MATH_DECL(fabs)
FE_MATH_DECL2(atan2) FE_MATH_DECL2(pow) FE_MATH_DECL2(fmod) FE_MATH_DECL2(hypot)
FE_MATH_DECL2(fmin) FE_MATH_DECL2(fmax) FE_MATH_DECL2(fdim)
FE_MATH_DECL2(copysign) FE_MATH_DECL2(nextafter) FE_MATH_DECL2(remainder)

double fma(double, double, double);
float  fmaf(float, float, float);
double ldexp(double, int);
float  ldexpf(float, int);
double scalbn(double, int);
float  scalbnf(float, int);
double frexp(double, int*);
float  frexpf(float, int*);
double modf(double, double*);
float  modff(float, float*);
int    ilogb(double);
int    ilogbf(float);
long   lrint(double);
long   lrintf(float);
long   lround(double);
long   lroundf(float);
long long llrint(double);
long long llround(double);
double nan(const char*);
float  nanf(const char*);

#ifdef __cplusplus
}
#endif

#endif
