/* Implementation translation unit for the single-header libraries the
 * importers use (host / libc only). */
#define CGLTF_IMPLEMENTATION
#include "cgltf/cgltf.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb/stb_image_write.h"
