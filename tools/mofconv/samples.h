// Deterministic generators for the sample models in assets/samples.
#pragma once
#include "mof/mof.h"

struct mof_sample_info { const char* file; const char* description; };
extern const mof_sample_info mof_samples[];
extern const unsigned mof_sample_count;

// Builds sample `index` (0 <= index < mof_sample_count) into `out` using the libc-free
// allocator hook `alloc`. Output is byte-for-byte reproducible.
bool mof_sample_build(unsigned index, mof_alloc alloc, mof_buffer* out, char* err, size_t errlen);
