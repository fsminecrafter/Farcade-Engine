/* sscanf: %d %i %u %o %x %X %c %s %[set] %e %f %g %a %p %n %% with the
 * length modifiers hh h l ll j z t L, field widths and the * flag. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

static int sp(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

int vsscanf(const char* __restrict str, const char* __restrict fmt, va_list ap) {
    const char* in = str;
    int count = 0;
    int converted_any = 0;
    for (const char* f = fmt; *f; f++) {
        if (sp((unsigned char)*f)) {
            while (sp((unsigned char)*in)) in++;
            continue;
        }
        if (*f != '%') {
            if (*in != *f) return (*in == 0 && !converted_any && count == 0) ? EOF : count;
            in++;
            continue;
        }
        f++;
        if (*f == '%') {
            while (sp((unsigned char)*in)) in++;
            if (*in != '%') return count;
            in++;
            continue;
        }
        int suppress = 0;
        if (*f == '*') { suppress = 1; f++; }
        int width = 0;
        while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
        int lenmod = 0; /* -2 hh, -1 h, 0, 1 l, 2 ll, 3 j/z/t (64-bit), 4 L */
        if (*f == 'h') { f++; if (*f == 'h') { f++; lenmod = -2; } else lenmod = -1; }
        else if (*f == 'l') { f++; if (*f == 'l') { f++; lenmod = 2; } else lenmod = 1; }
        else if (*f == 'j' || *f == 'z' || *f == 't') { f++; lenmod = 3; }
        else if (*f == 'L') { f++; lenmod = 4; }
        char conv = *f;
        if (!conv) return count;
        if (conv == 'n') {
            if (!suppress) {
                long n = (long)(in - str);
                void* dst = va_arg(ap, void*);
                switch (lenmod) {
                case -2: *(signed char*)dst = (signed char)n; break;
                case -1: *(short*)dst = (short)n; break;
                case 1: case 2: case 3: *(long*)dst = n; break;
                default: *(int*)dst = (int)n; break;
                }
            }
            continue;
        }
        if (conv != 'c' && conv != '[') while (sp((unsigned char)*in)) in++;
        if (!*in) return converted_any || count ? count : EOF;
        int w = width ? width : 0x7fffffff;
        switch (conv) {
        case 'c': {
            if (!width) w = 1;
            char* dst = suppress ? 0 : va_arg(ap, char*);
            int i = 0;
            for (; i < w && in[i]; i++) if (dst) dst[i] = in[i];
            if (i < w) return count;     /* input ended inside the field */
            in += i;
            if (!suppress) count++;
            break;
        }
        case 's': {
            char* dst = suppress ? 0 : va_arg(ap, char*);
            int i = 0;
            while (i < w && in[i] && !sp((unsigned char)in[i])) { if (dst) dst[i] = in[i]; i++; }
            if (dst) dst[i] = 0;
            in += i;
            if (!suppress) count++;
            break;
        }
        case '[': {
            f++;
            int negate = 0;
            if (*f == '^') { negate = 1; f++; }
            const char* set = f;
            if (*f == ']') f++;
            while (*f && *f != ']') f++;
            const char* setend = f;      /* points at the closing ] */
            char* dst = suppress ? 0 : va_arg(ap, char*);
            int i = 0;
            while (i < w && in[i]) {
                int c = (unsigned char)in[i], hit = 0;
                for (const char* t = set; t < setend; t++) {
                    if (t + 2 < setend && t[1] == '-') {
                        if (c >= (unsigned char)t[0] && c <= (unsigned char)t[2]) { hit = 1; break; }
                        t += 2;
                    } else if ((unsigned char)*t == c) { hit = 1; break; }
                }
                if (hit == negate) break;
                if (dst) dst[i] = in[i];
                i++;
            }
            if (i == 0) return count;
            if (dst) dst[i] = 0;
            in += i;
            if (!suppress) count++;
            if (!*f) f--;
            break;
        }
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
            char buf[80];
            int n = 0;
            int lim = w < 79 ? w : 79;
            while (n < lim && in[n]) { buf[n] = in[n]; n++; }
            buf[n] = 0;
            char* e;
            int base = conv == 'd' || conv == 'u' ? 10 : conv == 'i' ? 0 : conv == 'o' ? 8 : 16;
            long long sv = 0; unsigned long long uv = 0;
            if (conv == 'd' || conv == 'i') sv = strtoll(buf, &e, base);
            else { uv = strtoull(buf, &e, base); sv = (long long)uv; }
            if (e == buf) return count;
            in += e - buf;
            converted_any = 1;
            if (!suppress) {
                void* dst = va_arg(ap, void*);
                if (conv == 'p') *(void**)dst = (void*)(uintptr_t)uv;
                else switch (lenmod) {
                case -2: *(signed char*)dst = (signed char)sv; break;
                case -1: *(short*)dst = (short)sv; break;
                case 1: case 2: case 3: *(long long*)dst = sv; break;
                default: *(int*)dst = (int)sv; break;
                }
                count++;
            }
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            char buf[512];
            int n = 0;
            int lim = w < 511 ? w : 511;
            while (n < lim && in[n]) { buf[n] = in[n]; n++; }
            buf[n] = 0;
            char* e;
            double v = strtod(buf, &e);
            if (e == buf) return count;
            in += e - buf;
            converted_any = 1;
            if (!suppress) {
                void* dst = va_arg(ap, void*);
                if (lenmod == 1) *(double*)dst = v;
                else if (lenmod == 4) *(long double*)dst = v;
                else *(float*)dst = (float)v;
                count++;
            }
            break;
        }
        default:
            return count;
        }
    }
    return count;
}

int sscanf(const char* __restrict str, const char* __restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}
