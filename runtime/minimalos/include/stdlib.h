/* stdlib.h of the Minimal-OS runtime. Replaces the SDK's (whose malloc is a
 * kernel call per block): malloc/free here are the runtime allocator. */
#ifndef FE_STDLIB_H
#define FE_STDLIB_H
#define MINIMALOS_STDLIB_H 1   /* keeps the SDK's stdlib.h from being read */

#include <stddef.h>
#include <stdint.h>
#include <alloca.h>   /* glibc's stdlib.h declares alloca too */

#ifdef __cplusplus
extern "C" {
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff

typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

void*  malloc(size_t size);
void*  calloc(size_t count, size_t size);
void*  realloc(void* ptr, size_t size);
void   free(void* ptr);
void*  aligned_alloc(size_t alignment, size_t size);
int    posix_memalign(void** out, size_t alignment, size_t size);

__attribute__((noreturn)) void exit(int code);
__attribute__((noreturn)) void _Exit(int code);
__attribute__((noreturn)) void abort(void);
int    atexit(void (*fn)(void));
char*  getenv(const char* name);

int    abs(int x);
long   labs(long x);
long long llabs(long long x);
div_t  div(int a, int b);
ldiv_t ldiv(long a, long b);
lldiv_t lldiv(long long a, long long b);

int    rand(void);
void   srand(unsigned seed);
int    rand_r(unsigned* seed);

double strtod(const char* __restrict s, char** __restrict end);
float  strtof(const char* __restrict s, char** __restrict end);
long double strtold(const char* __restrict s, char** __restrict end);
long   strtol(const char* __restrict s, char** __restrict end, int base);
unsigned long strtoul(const char* __restrict s, char** __restrict end, int base);
long long strtoll(const char* __restrict s, char** __restrict end, int base);
unsigned long long strtoull(const char* __restrict s, char** __restrict end, int base);
int    atoi(const char* s);
long   atol(const char* s);
long long atoll(const char* s);
double atof(const char* s);

void   qsort(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*));
void*  bsearch(const void* key, const void* base, size_t n, size_t size,
               int (*cmp)(const void*, const void*));

#ifdef __cplusplus
}
inline long abs(long x) { return labs(x); }
inline long long abs(long long x) { return llabs(x); }
inline float abs(float x) { return __builtin_fabsf(x); }
inline double abs(double x) { return __builtin_fabs(x); }
#endif

#ifdef MINIMALOS_TARGET
#include "fe_sdk_compat.h"
#endif

#endif
