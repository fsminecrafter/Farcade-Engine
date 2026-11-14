/* snprintf family on stb_sprintf. */
#include <stdio.h>
#include <stdarg.h>
#include "fe_stbsp.h"

#define STB_SPRINTF_IMPLEMENTATION
#include "stb/stb_sprintf.h"

int vsnprintf(char* __restrict buf, size_t n, const char* __restrict fmt, va_list ap) {
    return fe_stbsp_vsnprintf(buf, n > 0x7fffffff ? 0x7fffffff : (int)n, fmt, ap);
}

int snprintf(char* __restrict buf, size_t n, const char* __restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char* __restrict buf, const char* __restrict fmt, va_list ap) {
    return fe_stbsp_vsprintf(buf, fmt, ap);
}

int sprintf(char* __restrict buf, const char* __restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = fe_stbsp_vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}
