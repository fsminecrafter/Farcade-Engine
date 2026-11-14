/* memcpy & co. SSE2 (16-byte unaligned vectors), rep movsb/stosb for big
 * blocks. Built with -fno-builtin -fno-tree-loop-distribute-patterns so the
 * compiler does not turn these loops into calls to themselves. */
#include <string.h>
#include <stdint.h>

typedef long long v2di __attribute__((vector_size(16), aligned(1), may_alias));
typedef uint64_t u64u __attribute__((aligned(1), may_alias));
typedef uint32_t u32u __attribute__((aligned(1), may_alias));
typedef uint16_t u16u __attribute__((aligned(1), may_alias));

static inline void copy_small(unsigned char* d, const unsigned char* s, size_t n) {
    /* n <= 32; all loads happen before the stores, so overlap is fine */
    if (n >= 16) {
        v2di a = *(const v2di*)s, b = *(const v2di*)(s + n - 16);
        *(v2di*)d = a; *(v2di*)(d + n - 16) = b;
    } else if (n >= 8) {
        uint64_t a = *(const u64u*)s, b = *(const u64u*)(s + n - 8);
        *(u64u*)d = a; *(u64u*)(d + n - 8) = b;
    } else if (n >= 4) {
        uint32_t a = *(const u32u*)s, b = *(const u32u*)(s + n - 4);
        *(u32u*)d = a; *(u32u*)(d + n - 4) = b;
    } else if (n >= 2) {
        uint16_t a = *(const u16u*)s, b = *(const u16u*)(s + n - 2);
        *(u16u*)d = a; *(u16u*)(d + n - 2) = b;
    } else if (n) *d = *s;
}

static inline void copy_fwd(unsigned char* d, const unsigned char* s, size_t n) {
    if (n <= 32) {
        if (n > 16) {
            v2di a = *(const v2di*)s, b = *(const v2di*)(s + n - 16);
            *(v2di*)d = a; *(v2di*)(d + n - 16) = b;
        } else copy_small(d, s, n);
        return;
    }
    v2di tail = *(const v2di*)(s + n - 16);
    unsigned char* dend = d + n - 16;
    if (n >= 512) {
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
        *(v2di*)dend = tail;
        return;
    }
    while (n > 16) {
        *(v2di*)d = *(const v2di*)s;
        d += 16; s += 16; n -= 16;
    }
    *(v2di*)dend = tail;
}

void* memcpy(void* __restrict dst, const void* __restrict src, size_t n) {
    copy_fwd((unsigned char*)dst, (const unsigned char*)src, n);
    return dst;
}

void* mempcpy(void* __restrict dst, const void* __restrict src, size_t n) {
    copy_fwd((unsigned char*)dst, (const unsigned char*)src, n);
    return (unsigned char*)dst + n;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || !n) return dst;
    if (d < s || d >= s + n) {
        if (d < s && (size_t)(s - d) < 64 && n > 32) {
            /* overlapping forward copy of a big block: plain vector loop */
            v2di tail = *(const v2di*)(s + n - 16);
            unsigned char* dend = d + n - 16;
            size_t m = n;
            while (m > 16) { *(v2di*)d = *(const v2di*)s; d += 16; s += 16; m -= 16; }
            *(v2di*)dend = tail;
            return dst;
        }
        copy_fwd(d, s, n);
        return dst;
    }
    /* d > s and overlapping: copy backwards */
    if (n <= 32) { copy_small(d, s, n); return dst; }
    v2di head = *(const v2di*)s;
    unsigned char* d0 = d;
    d += n; s += n;
    size_t m = n;
    while (m > 16) {
        d -= 16; s -= 16; m -= 16;
        *(v2di*)d = *(const v2di*)s;
    }
    *(v2di*)d0 = head;
    return dst;
}

void* memset(void* dst, int value, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    unsigned char c = (unsigned char)value;
    if (n <= 16) {
        if (n >= 8) {
            uint64_t v = 0x0101010101010101ULL * c;
            *(u64u*)d = v; *(u64u*)(d + n - 8) = v;
        } else if (n >= 4) {
            uint32_t v = 0x01010101u * c;
            *(u32u*)d = v; *(u32u*)(d + n - 4) = v;
        } else if (n) {
            d[0] = c; d[n - 1] = c; d[n >> 1] = c;
        }
        return dst;
    }
    v2di v = { (long long)(0x0101010101010101ULL * c), (long long)(0x0101010101010101ULL * c) };
    unsigned char* dend = d + n - 16;
    if (n >= 512) {
        __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
        *(v2di*)dend = v;
        return dst;
    }
    while (n > 16) { *(v2di*)d = v; d += 16; n -= 16; }
    *(v2di*)dend = v;
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    while (n >= 8) {
        uint64_t p = *(const u64u*)x, q = *(const u64u*)y;
        if (p != q) {
            p = __builtin_bswap64(p); q = __builtin_bswap64(q);
            return p < q ? -1 : 1;
        }
        x += 8; y += 8; n -= 8;
    }
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

void* memchr(const void* s, int c, size_t n) {
    const unsigned char* p = (const unsigned char*)s;
    unsigned char ch = (unsigned char)c;
    for (; n; n--, p++) if (*p == ch) return (void*)p;
    return 0;
}

void* memrchr(const void* s, int c, size_t n) {
    const unsigned char* p = (const unsigned char*)s + n;
    unsigned char ch = (unsigned char)c;
    while (n--) if (*--p == ch) return (void*)p;
    return 0;
}

void* memccpy(void* __restrict dst, const void* __restrict src, int c, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
        if (s[i] == (unsigned char)c) return d + i + 1;
    }
    return 0;
}

void* memmem(const void* hay, size_t hn, const void* needle, size_t nn) {
    const unsigned char* h = (const unsigned char*)hay;
    const unsigned char* nd = (const unsigned char*)needle;
    if (!nn) return (void*)h;
    if (nn > hn) return 0;
    for (size_t i = 0; i + nn <= hn; i++)
        if (h[i] == nd[0] && memcmp(h + i, nd, nn) == 0) return (void*)(h + i);
    return 0;
}
