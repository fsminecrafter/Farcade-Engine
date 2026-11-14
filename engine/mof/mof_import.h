// .mof importers: glTF 2.0 (.gltf/.glb) and Wavefront OBJ (+ .mtl) -> mof_wdesc.
//
// Unlike mof.h / mof_read.cpp / mof_write.cpp this part uses libc (stdio,
// stdlib, math) because cgltf, fast_obj and stb_image need it. It is
// plain C-library code (no POSIX calls) so it can later run on Minimal-OS.
#pragma once
#include "mof/mof.h"

// An imported model. `desc` points into memory owned by the model; it stays
// valid until mof_model_free. desc.alloc is already set to the libc allocator
// (mof_libc_alloc), so desc can be passed straight to mof_write.
struct mof_model {
    mof_wdesc desc;
    void** blocks;
    size_t block_count, block_cap;
    char warnings[2048];  // newline separated, truncated when full
};

bool mof_import_gltf(const char* path, mof_model* out, char* err, size_t errlen);  // .gltf and .glb
bool mof_import_obj(const char* path, mof_model* out, char* err, size_t errlen);
// Picks the importer from the extension (.gltf .glb .obj, case-insensitive).
bool mof_import_file(const char* path, mof_model* out, char* err, size_t errlen);
void mof_model_free(mof_model* m);

// malloc/free backed allocator for mof_write and mof_buffer.
mof_alloc mof_libc_alloc();

// ---- helpers (also used by the tests)

// Area weighted vertex normals. Zero-area vertices get (0,1,0).
void mof_generate_normals(const float* positions, uint32_t vertex_count, const uint32_t* indices,
                          uint32_t index_count, float* out_normals);

// Per-vertex tangents with a simple per-triangle method (Lengyel): tangent and
// bitangent are accumulated per vertex from the UV gradients, the tangent is
// Gram-Schmidt orthogonalised against the normal, w = handedness. UVs use the
// glTF convention (origin top-left, v down); w is chosen so that
// bitangent = cross(normal, tangent.xyz) * w points along image-up (+Y of a
// tangent space normal map), as glTF requires. Not MikkTSpace.
void mof_generate_tangents(const float* positions, const float* normals, const float* uv,
                           uint32_t vertex_count, const uint32_t* indices, uint32_t index_count,
                           float* out_tangents);
