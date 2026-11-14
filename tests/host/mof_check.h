// Tiny check macros for the .mof tests (no framework, C headers only).
#pragma once
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0, g_failures = 0;

#define CHECK(cond) \
    do { g_checks++; if (!(cond)) { g_failures++; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_MSG(cond, msg) \
    do { g_checks++; if (!(cond)) { g_failures++; fprintf(stderr, "%s:%d: CHECK failed: %s (%s)\n", __FILE__, __LINE__, #cond, (msg)); } } while (0)
#define CHECK_EQ(a, b) CHECK((long long)(a) == (long long)(b))
#define CHECK_STR(a, b) CHECK(strcmp((a), (b)) == 0)
#define CHECK_NEAR(a, b) CHECK(fabsf((float)(a) - (float)(b)) < 1e-5f)

static int finish_tests(const char* name) {
    printf("%s: %d checks, %d failures\n", name, g_checks, g_failures);
    return g_failures ? 1 : 0;
}

// xorshift64*: fixed-seed deterministic PRNG for the fuzz loops
struct Rng {
    unsigned long long s;
    explicit Rng(unsigned long long seed) : s(seed ? seed : 1) {}
    unsigned long long next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 2685821657736338717ull; }
    unsigned below(unsigned n) { return n ? (unsigned)(next() % n) : 0; }
};
