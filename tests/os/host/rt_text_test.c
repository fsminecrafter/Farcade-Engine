/* Host test: the runtime's snprintf / sscanf / strtod / strtol (symbols
 * prefixed fe_) against glibc. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>

int fe_errno_value;
int fe_snprintf(char*, size_t, const char*, ...);
int fe_vsnprintf(char*, size_t, const char*, va_list);
int fe_sprintf(char*, const char*, ...);
int fe_sscanf(const char*, const char*, ...);
double fe_strtod(const char*, char**);
float fe_strtof(const char*, char**);
long fe_strtol(const char*, char**, int);
unsigned long fe_strtoul(const char*, char**, int);
long long fe_strtoll(const char*, char**, int);
unsigned long long fe_strtoull(const char*, char**, int);
int fe_atoi(const char*);
double fe_atof(const char*);

static uint64_t rng = 88172645463325252ULL;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static int failures;
#define FAIL(...) do { if (failures++ < 40) { printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void test_strtod(void) {
    long n = 0, bad = 0;
    char buf[128];
    for (int i = 0; i < 200000; i++) {
        uint64_t bits = rnd();
        double d;
        memcpy(&d, &bits, 8);
        if (isnan(d) || isinf(d)) continue;
        snprintf(buf, sizeof buf, "%.17g", d);
        char *e1, *e2;
        double a = fe_strtod(buf, &e1), b = strtod(buf, &e2);
        n++;
        if (memcmp(&a, &b, 8) != 0 || e1 != e2) { if (bad++ < 5) FAIL("strtod(\"%s\") = %.17g vs %.17g", buf, a, b); }
    }
    for (int i = 0; i < 200000; i++) {
        int nd = 1 + (int)(rnd() % 25);
        char* p = buf;
        if (rnd() % 4 == 0) *p++ = '-';
        int dot = (int)(rnd() % (nd + 1));
        for (int k = 0; k < nd; k++) { if (k == dot) *p++ = '.'; *p++ = (char)('0' + rnd() % 10); }
        if (rnd() % 3) p += sprintf(p, "e%d", (int)(rnd() % 700) - 350);
        *p = 0;
        char *e1, *e2;
        double a = fe_strtod(buf, &e1), b = strtod(buf, &e2);
        n++;
        if (memcmp(&a, &b, 8) != 0 || e1 != e2) {
            /* strings with more than 19 digits may differ in the last bit; allow 1 ulp there */
            int64_t x, y; memcpy(&x, &a, 8); memcpy(&y, &b, 8);
            if (nd <= 19 || llabs(x - y) > 1 || e1 != e2) { if (bad++ < 10) FAIL("strtod(\"%s\") = %.17g vs %.17g", buf, a, b); }
        }
    }
    static const char* cases[] = { "0", "-0", "1", "  12.5abc", ".5", "5.", "-.5e-3x", "1e", "1e+", "1e5x", "inf", "-Infinity", "nan", "NaN(abc)x", "0x1p3", "0x1.8p1", "-0X.8P-2", "0x", "abc", "", "+", "-", "1.7976931348623157e308", "1.8e308", "4.9e-324", "2.5e-324", "1e-400", "123456789012345678901234567890", "0.000000000000000000000000000001", "9007199254740993", "1e23", "8.5e-5", "3.14159265358979323846" };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        char *e1, *e2;
        double a = fe_strtod(cases[i], &e1), b = strtod(cases[i], &e2);
        if ((memcmp(&a, &b, 8) != 0 && !(isnan(a) && isnan(b))) || e1 != e2) FAIL("strtod case \"%s\": %.17g (+%ld) vs %.17g (+%ld)", cases[i], a, (long)(e1 - cases[i]), b, (long)(e2 - cases[i]));
    }
    for (int i = 0; i < 20000; i++) {
        float f; uint32_t bits = (uint32_t)rnd(); memcpy(&f, &bits, 4);
        if (isnan(f) || isinf(f)) continue;
        snprintf(buf, sizeof buf, "%.9g", f);
        float a = fe_strtof(buf, 0), b = strtof(buf, 0);
        if (memcmp(&a, &b, 4) != 0) { if (bad++ < 5) FAIL("strtof(\"%s\")", buf); }
    }
    printf("  strtod: %ld values checked, %ld mismatches\n", n, bad);
}

static void test_strtol(void) {
    static const char* cases[] = { "0", "123", "-123", "  +42x", "0x1f", "0X1F", "017", "0b11", "99999999999999999999", "-99999999999999999999", "9223372036854775807", "9223372036854775808", "-9223372036854775808", "18446744073709551615", "18446744073709551616", "zz", "", "-", "0x", "0xg", "  \t\n7" };
    static const int bases[] = { 0, 10, 16, 8, 2, 36 };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++)
        for (unsigned j = 0; j < sizeof bases / sizeof *bases; j++) {
            char *e1, *e2;
            errno = 0; long a = fe_strtol(cases[i], &e1, bases[j]);
            long b = strtol(cases[i], &e2, bases[j]);
            if (a != b || e1 != e2) FAIL("strtol(\"%s\", %d) = %ld (+%ld) vs %ld (+%ld)", cases[i], bases[j], a, (long)(e1 - cases[i]), b, (long)(e2 - cases[i]));
            unsigned long ua = fe_strtoul(cases[i], &e1, bases[j]), ub = strtoul(cases[i], &e2, bases[j]);
            if (ua != ub || e1 != e2) FAIL("strtoul(\"%s\", %d) = %lu vs %lu", cases[i], bases[j], ua, ub);
            long long qa = fe_strtoll(cases[i], &e1, bases[j]), qb = strtoll(cases[i], &e2, bases[j]);
            if (qa != qb || e1 != e2) FAIL("strtoll(\"%s\", %d)", cases[i], bases[j]);
        }
    printf("  strtol family: done\n");
}

#define CMP_PRINTF(...) do { char a[600], b[600]; int ra = fe_snprintf(a, sizeof a, __VA_ARGS__); int rb = snprintf(b, sizeof b, __VA_ARGS__); \
    if (ra != rb || strcmp(a, b)) FAIL("printf(%s): \"%s\" (%d) vs \"%s\" (%d)", #__VA_ARGS__, a, ra, b, rb); } while (0)

static void test_printf(void) {
    CMP_PRINTF("%d %i %u", -5, 7, 4000000000u);
    CMP_PRINTF("%5d|%-5d|%05d|%+d|% d", 42, 42, 42, 42, 42);
    CMP_PRINTF("%ld %lld %lu %llu", -1234567890123L, -1234567890123456LL, 1234567890123UL, 12345678901234567890ULL);
    CMP_PRINTF("%zu %zd %td", (size_t)123456789012, (ssize_t)-5, (ptrdiff_t)77);
    CMP_PRINTF("%x %X %#x %08x %o %#o", 255, 255, 255, 255, 8, 8);
    CMP_PRINTF("%c%c%c", 'a', 'b', 'c');
    CMP_PRINTF("%s|%10s|%-10s|%.3s", "hello", "hi", "hi", "truncated");
    CMP_PRINTF("%f %e %g %G", 3.14159, 3.14159, 3.14159, 1e-10);
    CMP_PRINTF("%.0f %.1f %.2f %.10f", 2.6, 2.26, 2.676, 1.0 / 3);
    CMP_PRINTF("%8.3f|%-8.3f|%+.2f|%08.2f", 3.14159, 3.14159, 3.14159, -3.14159);
    CMP_PRINTF("%g %g %g %g", 100000.0, 1000000.0, 1e-5, 0.0001);
    CMP_PRINTF("%.3g %.10g %g", 1234.5678, 1234.5678, 1e300);
    CMP_PRINTF("100%%");
    CMP_PRINTF("%*d|%-*d|%.*f", 6, 42, 6, 42, 3, 3.14159);
    CMP_PRINTF("%f %f", 1e15, 123456789.123456789);
    CMP_PRINTF("%s", "");
    for (int i = 0; i < 20000; i++) {
        uint64_t bits = rnd(); double d; memcpy(&d, &bits, 8);
        if (isnan(d) || isinf(d)) continue;
        /* stb_sprintf is exact for about 15 significant digits; ties round half up */
        d = (double)((int64_t)(bits >> 44) - (1 << 19)) / 1e3 / (1.0 + (double)(rnd() % 997) / 7.0) + 1e-9;
        const char* fmts[] = { "%.6f", "%g", "%.3e", "%.9g", "%10.4f", "%.2f" };
        char a[400], b[400];
        const char* f = fmts[rnd() % 6];
        fe_snprintf(a, sizeof a, f, d); snprintf(b, sizeof b, f, d);
        if (strcmp(a, b)) FAIL("printf(%s, %.17g): \"%s\" vs \"%s\"", f, d, a, b);
    }
    for (int i = 0; i < 5000; i++) {
        long long v = (long long)rnd() >> (rnd() % 64);
        CMP_PRINTF("%lld %llx %llu %d", v, v, (unsigned long long)v, (int)v);
    }
    /* truncation and return values */
    char small[8];
    int r = fe_snprintf(small, sizeof small, "%s", "0123456789");
    if (r != 10 || strcmp(small, "0123456")) FAIL("snprintf truncation: %d \"%s\"", r, small);
    r = fe_snprintf(small, 0, "%d", 12345);
    if (r != 5) FAIL("snprintf(n=0) returned %d", r);
    r = fe_snprintf(0, 0, "%s", "abc");
    if (r != 3) FAIL("snprintf(NULL,0) returned %d", r);
    char big[2000];
    memset(big, 'x', 1500); big[1500] = 0;
    char out[2000];
    r = fe_snprintf(out, sizeof out, "[%s]", big);
    if (r != 1502 || out[0] != '[' || out[1501] != ']') FAIL("long string %d", r);
    printf("  printf: done\n");
}

static void test_scanf(void) {
    int x, y, r, n;
    unsigned u1, u2;
    char c;
    float fv;
    double dv;
    char s1[64], s2[64];
    /* the formats ImGui uses, verbatim */
    r = fe_sscanf("Pos=10,-20", "Pos=%i,%i", &x, &y); if (r != 2 || x != 10 || y != -20) FAIL("Pos");
    r = fe_sscanf("Size=0x10,010", "Size=%i,%i", &x, &y); if (r != 2 || x != 16 || y != 8) FAIL("Size %d %d %d", r, x, y);
    r = fe_sscanf("ViewportId=0x0000ABCD", "ViewportId=0x%08X", &u1); if (r != 1 || u1 != 0xabcd) FAIL("ViewportId");
    r = fe_sscanf("Collapsed=1", "Collapsed=%d", &x); if (r != 1 || x != 1) FAIL("Collapsed");
    r = fe_sscanf("DockId=0x00000005,3", "DockId=0x%X,%d", &u1, &x); if (r != 2 || u1 != 5 || x != 3) FAIL("DockId");
    r = fe_sscanf("DockId=0x00000005", "DockId=0x%X", &u1); if (r != 1 || u1 != 5) FAIL("DockId1");
    r = fe_sscanf("DockId=0x00000005", "Collapsed=%d", &x); if (r != 0) FAIL("nomatch %d", r);
    r = fe_sscanf("ID=0x00000001 Window=0x0000BEEF Pos=1,2 Split=Y", "ID=0x%08X%n", &u1, &n); if (r != 1 || u1 != 1 || n != 13) FAIL("ID%%n r=%d n=%d", r, n);
    const char* line = "ID=0x00000001 Window=0x0000BEEF Pos=1,2 Split=Y";
    r = fe_sscanf(line, "ID=0x%08X%n", &u1, &n); line += n;
    r = fe_sscanf(line, " Window=0x%08X%n", &u2, &n); if (r != 1 || u2 != 0xbeef) FAIL("Window"); line += n;
    r = fe_sscanf(line, " Pos=%i,%i%n", &x, &y, &n); if (r != 2 || x != 1 || y != 2) FAIL("Pos2"); line += n;
    r = fe_sscanf(line, " Split=%c%n", &c, &n); if (r != 1 || c != 'Y') FAIL("Split");
    r = fe_sscanf("0x00000123", "%X", &u1); if (r != 1 || u1 != 0x123) FAIL("%%X");
    /* general */
    r = fe_sscanf("3.5 -2e3 abc", "%f %lf %s", &fv, &dv, s1); if (r != 3 || fv != 3.5f || dv != -2000.0 || strcmp(s1, "abc")) FAIL("float/double/string");
    r = fe_sscanf("key: value with spaces", "%[a-z]: %[^\n]", s1, s2); if (r != 2 || strcmp(s1, "key") || strcmp(s2, "value with spaces")) FAIL("scanset %d '%s' '%s'", r, s1, s2);
    r = fe_sscanf("12345", "%2d%3d", &x, &y); if (r != 2 || x != 12 || y != 345) FAIL("width");
    r = fe_sscanf("12 34", "%*d %d", &x); if (r != 1 || x != 34) FAIL("suppress");
    r = fe_sscanf("", "%d", &x); if (r != EOF) FAIL("empty -> %d", r);
    r = fe_sscanf("  ", "%d", &x); if (r != EOF) FAIL("blank -> %d", r);
    r = fe_sscanf("abc", "%d", &x); if (r != 0) FAIL("abc -> %d", r);
    r = fe_sscanf("100%", "%d%%", &x); if (r != 1 || x != 100) FAIL("percent");
    r = fe_sscanf("-7 +8 0", "%d %d %d", &x, &y, &n); if (r != 3 || x != -7 || y != 8 || n != 0) FAIL("signs");
    long long ll; unsigned char uc; short sh;
    r = fe_sscanf("-9000000000 200 -300", "%lld %hhu %hd", &ll, &uc, &sh); if (r != 3 || ll != -9000000000LL || uc != 200 || sh != -300) FAIL("sizes");
    r = fe_sscanf("1,2,3", "%d,%d,%d", &x, &y, &n); if (r != 3 || n != 3) FAIL("commas");
    r = fe_sscanf("1.5e2", "%f", &fv); if (r != 1 || fv != 150.0f) FAIL("1.5e2");
    r = fe_sscanf("ab", "%c%c", &c, (char*)&x); if (r != 2) FAIL("%%c");
    r = fe_sscanf("hello", "%3s", s1); if (r != 1 || strcmp(s1, "hel")) FAIL("%%3s");
    printf("  sscanf: done\n");
}

int main(void) {
    printf("text: printf/scanf/strtod vs glibc\n");
    test_strtod(); test_strtol(); test_printf(); test_scanf();
    if (failures) { printf("TEXT TEST FAILED (%d)\n", failures); return 1; }
    printf("TEXT TEST PASSED\n");
    return 0;
}
