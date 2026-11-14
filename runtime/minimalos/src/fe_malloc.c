/*
 * malloc for Minimal-OS programs, on SYS_VM (vm.h).
 *
 * The SDK's malloc() is one system call per block on the kernel heap. This
 * one asks the kernel for memory in chunks of 4 MiB (demand-zero pages: a
 * chunk costs nothing until it is used) and serves blocks from them:
 *
 *   - up to 64 KiB: 44 size classes (16-byte steps up to 128, then four per
 *     doubling). Each class has its own lock, free list and bump region
 *     ("span") cut from the current chunk. Freed blocks go back to the
 *     class's list and are reused; the memory stays with the program.
 *   - bigger: a mapping of its own (mos_vm_map), given back on free().
 *
 * Every block has a 16-byte header in front (class or mapping length, and a
 * check word that catches most bad pointers passed to free()). Blocks are
 * 16-byte aligned. All functions are safe to call from several threads;
 * the locks are futexes (thread.h), so a waiting thread sleeps.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <malloc.h>
#include "minimalos.h"
#include "vm.h"

#define HDR        16u
#define MAGIC      0xFE4D10C5u
#define NCLASS     44
#define SMALL_MAX  65536u
#define CLS_LARGE  0xFFFFFFFEu
#define CLS_FWD    0xFFFFFFFDu
#define CHUNK      (4u << 20)

typedef struct {
    uint32_t cls;      /* size class, CLS_LARGE or CLS_FWD */
    uint32_t magic;
    uint64_t size;     /* LARGE: mapping length; FWD: distance back to the real block */
} hdr_t;

typedef struct { volatile uint32_t v; } lock_t;   /* 0 free, 1 held, 2 held + waiters */

typedef struct {
    lock_t lock;
    void* free_list;
    unsigned char* cur;
    unsigned char* end;
} class_t;

static class_t g_class[NCLASS];
static lock_t g_chunk_lock;
static unsigned char* g_chunk_cur;
static unsigned char* g_chunk_end;
static volatile size_t g_mapped, g_small_live, g_large_live, g_large_bytes;

static inline long futex(long op, volatile uint32_t* addr, long val) {
    return mos_syscall4(SYS_FUTEX, op, (long)(uintptr_t)addr, val, 0);
}

static void lock_slow(lock_t* l) {
    uint32_t c;
    for (int i = 0; i < 64; i++) {
        __asm__ volatile("pause");
        c = 0;
        if (__atomic_compare_exchange_n(&l->v, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    }
    c = __atomic_exchange_n(&l->v, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        futex(SYS_FUTEX_WAIT, &l->v, 2);
        c = __atomic_exchange_n(&l->v, 2, __ATOMIC_ACQUIRE);
    }
}

static inline void lock(lock_t* l) {
    uint32_t c = 0;
    if (__atomic_compare_exchange_n(&l->v, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    lock_slow(l);
}

static inline void unlock(lock_t* l) {
    if (__atomic_exchange_n(&l->v, 0, __ATOMIC_RELEASE) == 2) futex(SYS_FUTEX_WAKE, &l->v, 1);
}

static inline unsigned class_of(size_t size) {
    if (size <= 128) return (unsigned)((size + 15) >> 4) - 1;
    size_t n = size - 1;
    unsigned msb = 63u - (unsigned)__builtin_clzll(n);
    return 8u + (msb - 7u) * 4u + (unsigned)((n >> (msb - 2u)) & 3u);
}

static inline size_t class_cap(unsigned cls) {
    if (cls < 8) return 16u * (cls + 1u);
    unsigned j = cls - 8u, e = j >> 2, r = j & 3u;
    return ((size_t)1 << (7u + e)) + (size_t)(r + 1u) * ((size_t)1 << (5u + e));
}

static void* vm_get(size_t len) {
    long err = 0;
    void* p = mos_vm_map_ex(NULL, len, MOS_VM_PROT_RW, 0, &err);
    if (p) __atomic_add_fetch(&g_mapped, len, __ATOMIC_RELAXED);
    return p;
}

/* A span of `len` bytes (a multiple of 4096) for a size class. */
static unsigned char* span_get(size_t len) {
    unsigned char* p;
    lock(&g_chunk_lock);
    if ((size_t)(g_chunk_end - g_chunk_cur) < len) {
        size_t want = len > CHUNK ? len : CHUNK;
        unsigned char* c = (unsigned char*)vm_get(want);
        if (!c && want > len) { want = len; c = (unsigned char*)vm_get(want); }
        if (!c) { unlock(&g_chunk_lock); return NULL; }
        g_chunk_cur = c;
        g_chunk_end = c + want;
    }
    p = g_chunk_cur;
    g_chunk_cur += len;
    unlock(&g_chunk_lock);
    return p;
}

static void* small_alloc(unsigned cls) {
    class_t* c = &g_class[cls];
    size_t blk = class_cap(cls) + HDR;
    unsigned char* b;
    lock(&c->lock);
    if (c->free_list) {
        b = (unsigned char*)c->free_list - HDR;
        c->free_list = *(void**)c->free_list;
    } else {
        if ((size_t)(c->end - c->cur) < blk) {
            size_t span = blk * 4 > (128u << 10) ? blk * 4 : (128u << 10);
            span = (span + 4095u) & ~(size_t)4095u;
            unsigned char* s = span_get(span);
            if (!s) { unlock(&c->lock); return NULL; }
            c->cur = s;
            c->end = s + span;
        }
        b = c->cur;
        c->cur += blk;
    }
    unlock(&c->lock);
    hdr_t* h = (hdr_t*)b;
    h->cls = cls;
    h->magic = MAGIC;
    h->size = 0;
    __atomic_add_fetch(&g_small_live, 1, __ATOMIC_RELAXED);
    return b + HDR;
}

static void* large_alloc(size_t size) {
    if (size > (size_t)1 << 46) return NULL;
    size_t len = (size + HDR + 4095u) & ~(size_t)4095u;
    unsigned char* b = (unsigned char*)vm_get(len);
    if (!b) return NULL;
    hdr_t* h = (hdr_t*)b;
    h->cls = CLS_LARGE;
    h->magic = MAGIC;
    h->size = len;
    __atomic_add_fetch(&g_large_live, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_large_bytes, len, __ATOMIC_RELAXED);
    return b + HDR;
}

void* malloc(size_t size) {
    void* p;
    if (size == 0) size = 1;
    if (size <= SMALL_MAX) p = small_alloc(class_of(size));
    else p = large_alloc(size);
    if (!p) errno = ENOMEM;
    return p;
}

static __attribute__((noreturn)) void bad_pointer(const char* what) {
    mos_write(2, what, strlen(what));
    abort();
}

/* The header of the block `p` really belongs to (follows a forward header). */
static inline hdr_t* real_hdr(void* p, void** real) {
    hdr_t* h = (hdr_t*)((unsigned char*)p - HDR);
    if (h->magic != MAGIC) bad_pointer("free/realloc: not a heap block (or it was overwritten)\n");
    if (h->cls == CLS_FWD) {
        p = (unsigned char*)p - h->size;
        h = (hdr_t*)((unsigned char*)p - HDR);
        if (h->magic != MAGIC) bad_pointer("free/realloc: damaged aligned block\n");
    }
    *real = p;
    return h;
}

void free(void* ptr) {
    if (!ptr) return;
    void* p;
    hdr_t* h = real_hdr(ptr, &p);
    if (h->cls == CLS_LARGE) {
        size_t len = h->size;
        __atomic_sub_fetch(&g_large_live, 1, __ATOMIC_RELAXED);
        __atomic_sub_fetch(&g_large_bytes, len, __ATOMIC_RELAXED);
        __atomic_sub_fetch(&g_mapped, len, __ATOMIC_RELAXED);
        mos_vm_unmap(h, len);
        return;
    }
    if (h->cls >= NCLASS) bad_pointer("free: damaged block header\n");
    class_t* c = &g_class[h->cls];
    h->magic = MAGIC ^ 0xFFFFFFFFu;          /* a second free() of it is caught */
    lock(&c->lock);
    *(void**)p = c->free_list;
    c->free_list = p;
    unlock(&c->lock);
    __atomic_sub_fetch(&g_small_live, 1, __ATOMIC_RELAXED);
}

static inline size_t block_cap(const hdr_t* h) {
    return h->cls == CLS_LARGE ? (size_t)h->size - HDR : class_cap(h->cls);
}

size_t malloc_usable_size(void* ptr) {
    if (!ptr) return 0;
    void* p;
    hdr_t* h = real_hdr(ptr, &p);
    return block_cap(h) - (size_t)((unsigned char*)ptr - (unsigned char*)p);
}

void* calloc(size_t count, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(count, size, &total)) { errno = ENOMEM; return NULL; }
    void* p = malloc(total);
    if (!p) return NULL;
    /* a fresh mapping reads as zero already */
    if (((hdr_t*)((unsigned char*)p - HDR))->cls != CLS_LARGE) memset(p, 0, total);
    return p;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    void* p;
    hdr_t* h = real_hdr(ptr, &p);
    size_t cap = block_cap(h) - (size_t)((unsigned char*)ptr - (unsigned char*)p);
    /* keep the block when it fits and is not mostly wasted */
    if (size <= cap && (cap <= 256 || size > cap / 2)) return ptr;
    void* n = malloc(size);
    if (!n) return NULL;
    memcpy(n, ptr, size < cap ? size : cap);
    free(ptr);
    return n;
}

void* aligned_alloc(size_t alignment, size_t size) {
    if (alignment <= 16) return malloc(size);
    if (alignment & (alignment - 1)) { errno = EINVAL; return NULL; }
    if (size > ((size_t)1 << 46) || alignment > ((size_t)1 << 30)) { errno = ENOMEM; return NULL; }
    unsigned char* p = (unsigned char*)malloc(size + alignment + HDR);
    if (!p) return NULL;
    unsigned char* q = (unsigned char*)(((uintptr_t)p + HDR + alignment - 1) & ~(uintptr_t)(alignment - 1));
    hdr_t* f = (hdr_t*)(q - HDR);
    f->cls = CLS_FWD;
    f->magic = MAGIC;
    f->size = (uint64_t)(q - p);
    return q;
}

void* memalign(size_t alignment, size_t size) { return aligned_alloc(alignment, size); }

int posix_memalign(void** out, size_t alignment, size_t size) {
    if (alignment < sizeof(void*) || (alignment & (alignment - 1))) return EINVAL;
    void* p = aligned_alloc(alignment, size);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}

void fe_malloc_stats(fe_malloc_stats_t* out) {
    out->mapped_bytes = g_mapped;
    out->small_allocs = g_small_live;
    out->large_allocs = g_large_live;
    out->large_bytes = g_large_bytes;
}
