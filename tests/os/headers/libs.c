/* Compile-only: the vendored C libraries the engine uses, built against the
 * runtime's headers (the link check is in mk/runtime.mk). */
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"
#define CGLTF_IMPLEMENTATION
#include "cgltf/cgltf.h"
#include "fast_obj/fast_obj.c"

int fe_headers_libs(const char* path) {
    int w, h, n;
    unsigned char* px = stbi_load(path, &w, &h, &n, 4);
    if (px) { stbi_write_png("0:/out.png", w, h, 4, px, w * 4); stbi_image_free(px); }
    cgltf_options o = { 0 };
    cgltf_data* d = 0;
    if (cgltf_parse_file(&o, path, &d) == cgltf_result_success) cgltf_free(d);
    fastObjMesh* m = fast_obj_read(path);
    if (m) fast_obj_destroy(m);
    stbtt_fontinfo f;
    return stbtt_InitFont(&f, (const unsigned char*)path, 0);
}
