// Internal helpers shared by the importers (libc allowed here).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "mof/mof_import.h"

void* mof_model_alloc(mof_model* m, size_t bytes);  // zeroed, owned by the model; null on failure
char* mof_model_strdup(mof_model* m, const char* s);
void mof_model_warn(mof_model* m, const char* fmt, ...);
void mof_model_init(mof_model* m);

// Reads a whole file with fopen. Result owned by the model.
bool mof_read_file(mof_model* m, const char* path, uint8_t** data, size_t* size);

// Fills `t` (data, size, codec, width, height) from encoded image bytes: PNG
// and JPEG are kept as-is, anything else stb_image can decode becomes RAW_RGBA8.
// `bytes` must stay valid; decoded pixels are owned by the model.
bool mof_make_texture_data(mof_model* m, const uint8_t* bytes, size_t size, mof_wtexture* t, char* err, size_t errlen);

// Quaternion/matrix helpers (column-major 4x4: m[col*4+row]).
void mof_mat4_decompose(const float m[16], float t[3], float r[4], float s[3]);
void mof_mat4_mul(const float a[16], const float b[16], float out[16]);
bool mof_mat4_inverse_affine(const float m[16], float out[16]);
