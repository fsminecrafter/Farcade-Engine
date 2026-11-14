/* exit/atexit/abort, getenv, qsort/bsearch, abs/div, rand, assert. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include "minimalos.h"

int fe_errno_value;

extern char** environ;                 /* crt0.c */
int main(int argc, char** argv, char** envp);
void fe_stdio_exit(void);              /* fe_stdio.c: flush and close all files */

/* ---- exit ----------------------------------------------------------- */

typedef struct { void (*fn)(void*); void* arg; int plain; } exit_fn_t;
#define EXIT_FN_STATIC 64
static exit_fn_t g_exit_static[EXIT_FN_STATIC];
static exit_fn_t* g_exit_fns = g_exit_static;
static size_t g_exit_count, g_exit_cap = EXIT_FN_STATIC;
static volatile int g_exit_lock;

static void exit_lock(void) {
    while (__atomic_exchange_n(&g_exit_lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
}
static void exit_unlock(void) { __atomic_store_n(&g_exit_lock, 0, __ATOMIC_RELEASE); }

static int exit_register(void (*fn)(void*), void* arg, int plain) {
    exit_lock();
    if (g_exit_count == g_exit_cap) {
        exit_fn_t* n = (exit_fn_t*)malloc(g_exit_cap * 2 * sizeof(exit_fn_t));
        if (!n) { exit_unlock(); return -1; }
        memcpy(n, g_exit_fns, g_exit_count * sizeof(exit_fn_t));
        if (g_exit_fns != g_exit_static) free(g_exit_fns);
        g_exit_fns = n;
        g_exit_cap *= 2;
    }
    exit_fn_t e = { fn, arg, plain };
    g_exit_fns[g_exit_count++] = e;
    exit_unlock();
    return 0;
}

int atexit(void (*fn)(void)) { return exit_register((void (*)(void*))(void*)fn, NULL, 1); }

/* The compiler registers destructors of static C++ objects here. */
int __cxa_atexit(void (*fn)(void*), void* arg, void* dso) {
    (void)dso;
    return exit_register(fn, arg, 0);
}

void exit(int code) {
    static volatile int once;
    if (!__atomic_exchange_n(&once, 1, __ATOMIC_ACQUIRE)) {
        for (;;) {
            exit_lock();
            if (!g_exit_count) { exit_unlock(); break; }
            exit_fn_t e = g_exit_fns[--g_exit_count];
            exit_unlock();
            if (e.plain) ((void (*)(void))(void*)e.fn)();
            else e.fn(e.arg);
        }
        fe_stdio_exit();
    }
    mos_exit(code);
}

void _Exit(int code) { mos_exit(code); }

void abort(void) {
    static const char msg[] = "abort() called\n";
    mos_write(2, msg, sizeof(msg) - 1);
    fe_stdio_exit();
    mos_exit(134);
}

void __fe_assert_fail(const char* expr, const char* file, int line, const char* func) {
    char buf[512];
    int n = snprintf(buf, sizeof buf, "Assertion failed: %s (%s:%d, %s)\n", expr, file, line, func);
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    mos_write(2, buf, (size_t)n);
    abort();
}

/* crt0.c is built with -Dmain=fe_crt_main: it calls this, and returning
 * from main() goes through exit() (atexit handlers, static destructors,
 * buffered files) instead of straight to the kernel. */
int fe_crt_main(int argc, char** argv, char** envp) {
    exit(main(argc, argv, envp));
}

/* ---- environment ---------------------------------------------------- */

char* getenv(const char* name) {
    if (!name || !*name || !environ) return NULL;
    size_t n = strlen(name);
    for (char** e = environ; *e; e++)
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') return *e + n + 1;
    return NULL;
}

/* ---- integers ------------------------------------------------------- */

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }
div_t div(int a, int b) { div_t r = { a / b, a % b }; return r; }
ldiv_t ldiv(long a, long b) { ldiv_t r = { a / b, a % b }; return r; }
lldiv_t lldiv(long long a, long long b) { lldiv_t r = { a / b, a % b }; return r; }

static unsigned g_rand_state = 1;
int rand_r(unsigned* seed) {
    unsigned s = *seed;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    if (!s) s = 0x9e3779b9u;
    *seed = s;
    return (int)(s & RAND_MAX);
}
int rand(void) { return rand_r(&g_rand_state); }
void srand(unsigned seed) { g_rand_state = seed ? seed : 0x9e3779b9u; }

/* ---- sorting -------------------------------------------------------- */

typedef int (*cmp_fn)(const void*, const void*);

static void insertion_sort(char* base, size_t n, size_t size, cmp_fn cmp, char* tmp) {
    for (size_t i = 1; i < n; i++) {
        char* cur = base + i * size;
        if (cmp(cur - size, cur) <= 0) continue;
        memcpy(tmp, cur, size);
        size_t j = i;
        while (j > 0 && cmp(base + (j - 1) * size, tmp) > 0) {
            memcpy(base + j * size, base + (j - 1) * size, size);
            j--;
        }
        memcpy(base + j * size, tmp, size);
    }
}

/* stable merge sort: sorts `a` (n elements) using `b` as scratch */
static void merge_sort(char* a, char* b, size_t n, size_t size, cmp_fn cmp) {
    if (n <= 8) { insertion_sort(a, n, size, cmp, b); return; }
    size_t h = n / 2;
    merge_sort(a, b, h, size, cmp);
    merge_sort(a + h * size, b, n - h, size, cmp);
    if (cmp(a + (h - 1) * size, a + h * size) <= 0) return;
    memcpy(b, a, h * size);
    char *l = b, *lend = b + h * size, *r = a + h * size, *rend = a + n * size, *out = a;
    while (l < lend && r < rend) {
        if (cmp(r, l) < 0) { memcpy(out, r, size); r += size; }
        else { memcpy(out, l, size); l += size; }
        out += size;
    }
    if (l < lend) memcpy(out, l, (size_t)(lend - l));
}

static void sift_down(char* base, size_t start, size_t end, size_t size, cmp_fn cmp, char* tmp) {
    size_t root = start;
    for (;;) {
        size_t child = 2 * root + 1;
        if (child >= end) break;
        if (child + 1 < end && cmp(base + child * size, base + (child + 1) * size) < 0) child++;
        if (cmp(base + root * size, base + child * size) >= 0) break;
        memcpy(tmp, base + root * size, size);
        memcpy(base + root * size, base + child * size, size);
        memcpy(base + child * size, tmp, size);
        root = child;
    }
}

void qsort(void* basep, size_t n, size_t size, cmp_fn cmp) {
    char* base = (char*)basep;
    if (n < 2 || !size) return;
    char stackbuf[1024];
    size_t need = (n <= 8 ? 1 : (n / 2 + 1)) * size;
    char* tmp = need <= sizeof stackbuf ? stackbuf : (char*)malloc(need);
    if (tmp) {
        merge_sort(base, tmp, n, size, cmp);
        if (tmp != stackbuf) free(tmp);
        return;
    }
    /* no memory for the scratch buffer: heap sort in place (not stable) */
    char* el = size <= sizeof stackbuf ? stackbuf : (char*)__builtin_alloca(size);
    for (size_t i = n / 2; i-- > 0;) sift_down(base, i, n, size, cmp, el);
    for (size_t i = n - 1; i > 0; i--) {
        memcpy(el, base, size);
        memcpy(base, base + i * size, size);
        memcpy(base + i * size, el, size);
        sift_down(base, 0, i, size, cmp, el);
    }
}

void* bsearch(const void* key, const void* basep, size_t n, size_t size, cmp_fn cmp) {
    const char* base = (const char*)basep;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp(key, base + mid * size);
        if (c == 0) return (void*)(base + mid * size);
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}
