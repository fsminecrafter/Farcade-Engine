#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 16384
// stb_image keeps its failure reason in a thread-local variable by default.
// Programs on Minimal-OS have no thread-local storage set up (FS base 0), so
// the first failed format probe (every non-JPEG image is tried as JPEG
// first) read address 0 and killed the program: textures and particle
// images in exported games. One global string is enough here.
#define STBI_NO_THREAD_LOCALS
#include "stb/stb_image.h"
