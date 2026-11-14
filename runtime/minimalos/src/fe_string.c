/* str* functions. */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

size_t strlen(const char* s) {
    const char* p = s;
    while (((uintptr_t)p & 7) && *p) p++;
    if (!*p && ((uintptr_t)p & 7) == 0 && p != s) return (size_t)(p - s);
    if (((uintptr_t)p & 7) == 0) {
        const uint64_t* w = (const uint64_t*)p;
        for (;; w++) {
            uint64_t v = *w;
            if ((v - 0x0101010101010101ULL) & ~v & 0x8080808080808080ULL) break;
        }
        p = (const char*)w;
    }
    while (*p) p++;
    return (size_t)(p - s);
}

size_t strnlen(const char* s, size_t n) {
    size_t i = 0;
    while (i < n && s[i]) i++;
    return i;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x != y) return (int)x - (int)y;
        if (!x) return 0;
    }
    return 0;
}

int strcoll(const char* a, const char* b) { return strcmp(a, b); }

size_t strxfrm(char* __restrict dst, const char* __restrict src, size_t n) {
    size_t l = strlen(src);
    if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(dst, src, c); dst[c] = 0; }
    return l;
}

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int strcasecmp(const char* a, const char* b) {
    for (;; a++, b++) {
        int x = lc((unsigned char)*a), y = lc((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}

int strncasecmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int x = lc((unsigned char)a[i]), y = lc((unsigned char)b[i]);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}

char* strcpy(char* __restrict dst, const char* __restrict src) {
    memcpy(dst, src, strlen(src) + 1);
    return dst;
}

char* stpcpy(char* __restrict dst, const char* __restrict src) {
    size_t n = strlen(src);
    memcpy(dst, src, n + 1);
    return dst + n;
}

char* strncpy(char* __restrict dst, const char* __restrict src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char* strcat(char* __restrict dst, const char* __restrict src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

char* strncat(char* __restrict dst, const char* __restrict src, size_t n) {
    char* d = dst + strlen(dst);
    size_t i = 0;
    for (; i < n && src[i]; i++) d[i] = src[i];
    d[i] = 0;
    return dst;
}

size_t strlcpy(char* dst, const char* src, size_t size) {
    size_t l = strlen(src);
    if (size) { size_t c = l < size - 1 ? l : size - 1; memcpy(dst, src, c); dst[c] = 0; }
    return l;
}

size_t strlcat(char* dst, const char* src, size_t size) {
    size_t dl = strnlen(dst, size);
    if (dl == size) return size + strlen(src);
    return dl + strlcpy(dst + dl, src, size - dl);
}

char* strchr(const char* s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char*)s;
        if (!*s) return 0;
    }
}

char* strchrnul(const char* s, int c) {
    while (*s && *s != (char)c) s++;
    return (char*)s;
}

char* strrchr(const char* s, int c) {
    const char* last = 0;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char*)last;
    }
}

char* strstr(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    if (!nl) return (char*)hay;
    for (; *hay; hay++)
        if (*hay == *needle && strncmp(hay, needle, nl) == 0) return (char*)hay;
    return 0;
}

char* strcasestr(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    if (!nl) return (char*)hay;
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, nl) == 0) return (char*)hay;
    return 0;
}

size_t strspn(const char* s, const char* accept) {
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) n++;
    return n;
}

size_t strcspn(const char* s, const char* reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) n++;
    return n;
}

char* strpbrk(const char* s, const char* accept) {
    for (; *s; s++) if (strchr(accept, *s)) return (char*)s;
    return 0;
}

char* strtok_r(char* __restrict s, const char* __restrict delim, char** __restrict save) {
    if (!s) s = *save;
    if (!s) return 0;
    s += strspn(s, delim);
    if (!*s) { *save = 0; return 0; }
    char* end = s + strcspn(s, delim);
    if (*end) { *end = 0; *save = end + 1; } else *save = 0;
    return s;
}

char* strtok(char* __restrict s, const char* __restrict delim) {
    static char* save;
    return strtok_r(s, delim, &save);
}

char* strsep(char** sp, const char* delim) {
    char* s = *sp;
    if (!s) return 0;
    char* end = s + strcspn(s, delim);
    if (*end) { *end = 0; *sp = end + 1; } else *sp = 0;
    return s;
}

char* strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char* strndup(const char* s, size_t n) {
    size_t l = strnlen(s, n);
    char* p = (char*)malloc(l + 1);
    if (p) { memcpy(p, s, l); p[l] = 0; }
    return p;
}

char* strerror(int err) {
    switch (err) {
    case 0: return (char*)"Success";
    case ENOENT: return (char*)"No such file or directory";
    case EIO: return (char*)"Input/output error";
    case EBADF: return (char*)"Bad file descriptor";
    case ENOMEM: return (char*)"Out of memory";
    case EACCES: return (char*)"Permission denied";
    case EEXIST: return (char*)"File exists";
    case EINVAL: return (char*)"Invalid argument";
    case ENOSPC: return (char*)"No space left on device";
    case ERANGE: return (char*)"Result too large";
    case EDOM: return (char*)"Numerical argument out of domain";
    case ENOSYS: return (char*)"Function not implemented";
    default: return (char*)"Unknown error";
    }
}
