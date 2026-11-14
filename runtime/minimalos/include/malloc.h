#ifndef FE_MALLOC_H
#define FE_MALLOC_H
#include <stdlib.h>
#include <alloca.h>
#ifdef __cplusplus
extern "C" {
#endif
size_t malloc_usable_size(void* p);
void*  memalign(size_t alignment, size_t size);
/* Statistics of the runtime allocator. */
typedef struct {
    size_t mapped_bytes;     /* address space taken from the kernel */
    size_t small_allocs;     /* live blocks served from size classes */
    size_t large_allocs;     /* live direct mappings */
    size_t large_bytes;
} fe_malloc_stats_t;
void fe_malloc_stats(fe_malloc_stats_t* out);
#ifdef __cplusplus
}
#endif
#endif
