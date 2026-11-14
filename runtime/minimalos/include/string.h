/* string.h of the Minimal-OS runtime. Replaces the SDK's inline versions. */
#ifndef FE_STRING_H
#define FE_STRING_H
#define MINIMALOS_STRING_H 1   /* keeps the SDK's string.h from being read */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void*  memcpy(void* __restrict dst, const void* __restrict src, size_t n);
void*  memmove(void* dst, const void* src, size_t n);
void*  memset(void* dst, int value, size_t n);
int    memcmp(const void* a, const void* b, size_t n);
void*  memchr(const void* s, int c, size_t n);
void*  memrchr(const void* s, int c, size_t n);
void*  memccpy(void* __restrict dst, const void* __restrict src, int c, size_t n);
void*  mempcpy(void* __restrict dst, const void* __restrict src, size_t n);
void*  memmem(const void* hay, size_t hn, const void* needle, size_t nn);

size_t strlen(const char* s);
size_t strnlen(const char* s, size_t n);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
int    strcoll(const char* a, const char* b);
size_t strxfrm(char* __restrict dst, const char* __restrict src, size_t n);
int    strcasecmp(const char* a, const char* b);
int    strncasecmp(const char* a, const char* b, size_t n);
char*  strcpy(char* __restrict dst, const char* __restrict src);
char*  strncpy(char* __restrict dst, const char* __restrict src, size_t n);
char*  stpcpy(char* __restrict dst, const char* __restrict src);
char*  strcat(char* __restrict dst, const char* __restrict src);
char*  strncat(char* __restrict dst, const char* __restrict src, size_t n);
size_t strlcpy(char* dst, const char* src, size_t size);
size_t strlcat(char* dst, const char* src, size_t size);
char*  strchr(const char* s, int c);
char*  strrchr(const char* s, int c);
char*  strchrnul(const char* s, int c);
char*  strstr(const char* hay, const char* needle);
char*  strcasestr(const char* hay, const char* needle);
size_t strspn(const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);
char*  strpbrk(const char* s, const char* accept);
char*  strtok(char* __restrict s, const char* __restrict delim);
char*  strtok_r(char* __restrict s, const char* __restrict delim, char** __restrict save);
char*  strsep(char** s, const char* delim);
char*  strdup(const char* s);
char*  strndup(const char* s, size_t n);
char*  strerror(int err);

/* SDK helpers that its string.h defined (kept for source compatibility). */
static inline char* strlcpy_safe(char* dst, const char* src, size_t size) {
    strlcpy(dst, src, size);
    return dst;
}
static inline int mos_memcmp(const void* a, const void* b, size_t n) { return memcmp(a, b, n); }
static inline int mos_memeq_ct(const void* a, const void* b, size_t n) {
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)(x[i] ^ y[i]);
    return diff == 0;
}

#ifdef __cplusplus
}
#endif

#endif
