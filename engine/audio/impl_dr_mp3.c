// dr_mp3 implementation unit (external/dr_libs), memory decoding only, no
// SIMD paths (portable between the host and Minimal-OS).
#define DR_MP3_NO_STDIO
#define DR_MP3_NO_SIMD
#define DR_MP3_IMPLEMENTATION
#include "dr_libs/dr_mp3.h"
