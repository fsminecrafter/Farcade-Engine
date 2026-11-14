/* Things the SDK's own stdlib.h/stdio.h provided that SDK headers and old
 * programs use. Only for Minimal-OS builds (MINIMALOS_TARGET). */
#ifndef FE_SDK_COMPAT_H
#define FE_SDK_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "minimalos.h"

#ifdef __cplusplus
extern "C" {
#endif
int vprintf(const char* fmt, va_list ap);
#ifdef __cplusplus
}
#endif

static inline int mos_heap_failed(long value) {
    return value < 0 && value >= (long)-16;
}

/* Cryptographic-quality where the CPU has RDRAND; TSC/RTC-derived
 * otherwise. Returns the number of bytes written. */
static inline long mos_random_bytes(void* buf, size_t len) {
    return mos_syscall(SYS_RANDOM, (long)(uintptr_t)buf, (long)len, 0);
}

static inline uint64_t mos_random_u64(void) {
    uint64_t v = 0;
    mos_random_bytes(&v, sizeof(v));
    return v;
}

static inline long mos_vprintf(const char* format, va_list args) {
    return (long)vprintf(format, args);
}

#endif
