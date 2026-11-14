// mofgen: regenerates the sample .mof files.  usage: mofgen [output_dir]  (default assets/samples)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mof/mof.h"
#include "samples.h"

// The generator links only the core library, so it carries its own allocator.
static void* gen_realloc(void*, void* p, size_t, size_t n) {
    if (!n) { free(p); return nullptr; }
    return realloc(p, n);
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "assets/samples";
    mof_alloc alloc = {gen_realloc, nullptr};
    for (unsigned i = 0; i < mof_sample_count; i++) {
        mof_buffer buf;
        char err[256];
        if (!mof_sample_build(i, alloc, &buf, err, sizeof err)) {
            fprintf(stderr, "mofgen: %s: %s\n", mof_samples[i].file, err);
            return 1;
        }
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, mof_samples[i].file);
        FILE* f = fopen(path, "wb");
        if (!f || fwrite(buf.data, 1, buf.size, f) != buf.size) {
            fprintf(stderr, "mofgen: cannot write %s\n", path);
            return 1;
        }
        fclose(f);
        printf("wrote %s (%zu bytes): %s\n", path, buf.size, mof_samples[i].description);
        mof_buffer_free(&buf);
    }
    return 0;
}
