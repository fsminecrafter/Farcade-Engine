// Imported models (.mof) as scene meshes: loaded through the VFS (so the
// editor reads project files and an exported game its package), cached by
// path, flattened into triangle lists the editor, the renderers, picking and
// physics use, and kept as the original .mof view for code that needs more
// (the skeleton, joint weights: an Animator).
//
//   const Model* m = Scene::model_for(object);   // null unless a Model object whose file loaded
//   m->mesh                                      // all parts merged (bounds, picking, CPU preview)
//   m->parts[i]                                  // one per node mesh x submesh, node transform applied: what FarcadeGL draws
//   m->mof.bones / m->mof.nodes                  // the skeleton in the file's own terms (mof.h)
//   mof_mesh_joints(&m->mof, &m->mof.meshes[p.mesh_index]) ...   // skin streams, bind pose vertices
//
// The data stays valid until model_cache_clear() (Play and Import clear it).
#pragma once

#include "scene.h"
#include "mof/mof.h"
#include <stddef.h>

namespace fe {

struct ModelPart {
    Vec3* positions = nullptr;          // triangle list in model space (the bind pose), node transform applied
    Vec3* normals = nullptr;
    float* uvs = nullptr;               // 2 per vertex, null when the mesh has none
    uint32_t vertex_count = 0;
    Vec3 color = Vec3(1, 1, 1);         // the material's base color factor (sRGB-ish)
    char texture[320] = {};             // "mof:<model path>#<texture index>" for model_asset_reader, or ""
    float alpha_cut = 0.0f;             // the material's alpha mask cutoff (MASK, or BLEND drawn as a 0.5 mask); 0 = opaque
    bool double_sided = false;
    uint32_t mesh_index = 0;            // into mof.meshes
    uint32_t submesh_index = 0;         // into mof.submeshes
    uint32_t material = 0xFFFFFFFFu;    // into mof.materials, or MOF_NONE
    int node = -1;                      // mof.nodes index of the instance, -1 when the file has no nodes
};

struct Model {
    char path[256] = {};
    uint8_t* block = nullptr;           // owns the file bytes (data is block aligned up to 16)
    mof_file mof = {};
    MeshData mesh;                      // every part, merged
    ModelPart* parts = nullptr;
    int part_count = 0;
};

// Loads (or finds) the model. Null when the file cannot be read or is not a valid .mof.
const Model* model_load(const char* path);
void model_cache_clear();
// How model files are read: default is vfs::read (core/vfs.h).
typedef void* (*ModelReader)(const char* path, size_t* size);
void model_set_reader(ModelReader reader);

// An asset reader for gfx::RenderScene::read_asset that also serves the
// "mof:" texture paths of model parts and passes every other path to `inner`.
struct ModelAssetReader {
    void* (*inner)(const char* path, size_t* size, void* user) = nullptr;
    void* user = nullptr;
};
void* model_asset_reader(const char* path, size_t* size, void* reader /* const ModelAssetReader* */);

}  // namespace fe
