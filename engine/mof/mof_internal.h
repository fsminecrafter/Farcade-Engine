// Internal helpers shared by mof_read.cpp and mof_write.cpp (not public API).
// Freestanding: no libc headers; memcpy/memset/memcmp come from the compiler
// builtins (the target must provide them, as with any freestanding C++ code).
#pragma once
#include <stddef.h>
#include <stdint.h>

#define MOF_MEMCPY(d, s, n) __builtin_memcpy((d), (s), (n))
#define MOF_MEMSET(d, v, n) __builtin_memset((d), (v), (n))
#define MOF_MEMCMP(a, b, n) __builtin_memcmp((a), (b), (n))

namespace mofi {

// Writes "msg" or "msg #idx" into err (always NUL terminated when errlen > 0).
// Always returns false so callers can `return fail(...)`.
inline bool fail(char* err, size_t errlen, const char* msg, uint32_t idx = 0xFFFFFFFFu) {
    if (!err || errlen == 0) return false;
    size_t n = 0;
    while (msg[n] && n + 1 < errlen) { err[n] = msg[n]; n++; }
    if (idx != 0xFFFFFFFFu) {
        char tmp[12];
        int t = 0;
        uint32_t v = idx;
        do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
        if (n + 2 < errlen) { err[n++] = ' '; err[n++] = '#'; }
        while (t > 0 && n + 1 < errlen) err[n++] = tmp[--t];
    }
    err[n] = 0;
    return false;
}

inline bool finite(float x) { return x - x == 0.0f; }  // false for NaN and +-inf

}  // namespace mofi
