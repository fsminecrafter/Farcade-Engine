// Core .mof tests: writer/reader round trip, forward compatibility, targeted
// corruption, truncation at every byte, fixed-seed mutation fuzzing, writer
// allocation-failure handling, and the generated sample files.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mof/mof.h"
#include "mof_check.h"
#include "../../tools/mofconv/mof_info.h"
#include "../../tools/mofconv/samples.h"

// ------------------------------------------------------------------ allocator

struct CountingAlloc {
    long live = 0;       // outstanding allocations
    long calls = 0;      // allocation calls so far
    long fail_after = -1;  // fail every allocation call after this many (<0: never)
};

static void* counting_realloc(void* user, void* ptr, size_t, size_t n) {
    CountingAlloc* a = (CountingAlloc*)user;
    if (n == 0) { if (ptr) a->live--; free(ptr); return nullptr; }
    if (a->fail_after >= 0 && a->calls >= a->fail_after) return nullptr;
    a->calls++;
    void* p = realloc(ptr, n);
    if (p && !ptr) a->live++;
    return p;
}

static mof_alloc make_alloc(CountingAlloc* a) { return mof_alloc{counting_realloc, a}; }

// ------------------------------------------------------------------ the model

static const float kPos0[5 * 3] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0.5f, 0.5f, 2};
static const float kNrm0[5 * 3] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
static const float kTan0[5 * 4] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, -1, 1, 0, 0, 1, 1, 0, 0, 1};
static const float kUv0[5 * 2] = {0, 0, 1, 0, 1, 1, 0, 1, 0.5f, 0.5f};
static const float kUv1[5 * 2] = {0, 1, 1, 1, 1, 0, 0, 0, 0.25f, 0.75f};
static const uint8_t kCol0[5 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 1, 2, 3, 4, 5, 6, 7, 8};
static const uint16_t kJoints0[5 * 4] = {0, 1, 2, 0, 0, 1, 0, 0, 1, 2, 0, 0, 2, 2, 2, 2, 0, 0, 0, 0};
static const float kWeights0[5 * 4] = {0.5f, 0.25f, 0.25f, 0, 1, 0, 0, 0, 0.5f, 0.5f, 0, 0, 0.25f, 0.25f, 0.25f, 0.25f, 1, 0, 0, 0};
static const uint32_t kIdx0[12] = {0, 1, 2, 0, 2, 3, 0, 1, 4, 1, 2, 4};
static const mof_wsubmesh kSub0[2] = {{0, 6, 0}, {6, 6, 1}};

static const float kPos1[4 * 3] = {-1, -2, -3, 4, 5, 6, 0, 0, 0, 2, 2, 2};
static const float kUv1_0[4 * 2] = {0, 0, 1, 0, 1, 1, 0, 1};
static const uint16_t kJoints1[4 * 4] = {0, 0, 0, 0, 1, 0, 0, 0, 2, 1, 0, 0, 2, 2, 2, 2};
static const uint16_t kWeights1[4 * 4] = {65535, 0, 0, 0, 32768, 32767, 0, 0, 65535, 0, 0, 0, 16384, 16384, 16384, 16383};
static const uint32_t kIdx1[6] = {0, 1, 2, 2, 1, 3};

static const uint8_t kRaw[4 * 4 * 4 + 2 * 2 * 4 + 4] = {
#define R16 10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255,
    R16 R16 R16 R16  // 4x4 mip 0 (64 bytes)
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,  // 2x2
    77, 88, 99, 111};                                       // 1x1
static const uint8_t kPng[24] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 4, 0, 0, 0, 2};
static const uint8_t kJpg[10] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 4, 1, 2, 0xFF, 0xD9};

struct Model {
    mof_wmesh meshes[3];
    mof_wmaterial materials[2];
    mof_wtexture textures[4];
    mof_wbone bones[3];
    mof_wnode nodes[4];
    mof_wchunk extra[1];
    mof_wdesc desc;
};

static void build_model(Model* m, mof_alloc alloc) {
    memset(m, 0, sizeof *m);
    mof_wmesh* a = &m->meshes[0];
    mof_wmesh_default(a);
    a->name = "quad_pyramid"; a->vertex_count = 5;
    a->positions = kPos0; a->normals = kNrm0; a->tangents = kTan0; a->uv0 = kUv0; a->uv1 = kUv1; a->colors = kCol0;
    a->joints = kJoints0; a->weights_f32 = kWeights0;
    a->index_count = 12; a->indices = kIdx0; a->force_index32 = 1; a->submeshes = kSub0; a->submesh_count = 2;
    mof_wmesh* b = &m->meshes[1];
    mof_wmesh_default(b);
    b->name = "shared_name"; b->vertex_count = 4;
    b->positions = kPos1; b->uv0 = kUv1_0; b->joints = kJoints1; b->weights_u16 = kWeights1;
    b->index_count = 6; b->indices = kIdx1;  // default: u16, one submesh without material
    mof_wmesh_default(&m->meshes[2]);
    m->meshes[2].name = "empty";

    mof_wmaterial* x = &m->materials[0];
    mof_wmaterial_default(x);
    x->name = "shared_name";
    x->base_color[0] = 0.1f; x->base_color[1] = 0.2f; x->base_color[2] = 0.3f; x->base_color[3] = 0.4f;
    x->emissive[0] = 1; x->emissive[1] = 2; x->emissive[2] = 3;
    x->metallic = 0.25f; x->roughness = 0.75f; x->normal_scale = 2.0f; x->occlusion_strength = 0.5f;
    x->alpha_mode = MOF_ALPHA_MASK; x->alpha_cutoff = 0.33f; x->double_sided = 1;
    for (uint32_t s = 0; s < MOF_SLOT_COUNT; s++) { x->tex[s].texture = (int32_t)(s % 3); x->tex[s].uv_set = s & 1; }
    mof_wmaterial_default(&m->materials[1]);  // all defaults, no name

    mof_wtexture* t = m->textures;
    for (int i = 0; i < 4; i++) mof_wtexture_default(&t[i]);
    t[0].name = "raw"; t[0].codec = MOF_TEX_RAW_RGBA8; t[0].width = t[0].height = 4; t[0].mip_count = 3;
    t[0].data = kRaw; t[0].data_size = sizeof kRaw;
    t[0].wrap_s = MOF_WRAP_CLAMP; t[0].wrap_t = MOF_WRAP_MIRROR; t[0].min_filter = MOF_FILTER_NEAREST;
    t[0].mag_filter = MOF_FILTER_LINEAR; t[0].mip_filter = MOF_MIP_LINEAR;
    t[1].name = "png"; t[1].codec = MOF_TEX_PNG; t[1].width = 4; t[1].height = 2; t[1].data = kPng; t[1].data_size = sizeof kPng;
    t[2].name = "jpg"; t[2].codec = MOF_TEX_JPEG; t[2].data = kJpg; t[2].data_size = sizeof kJpg;
    t[3] = t[0]; t[3].name = "raw_again";  // same payload pointer: stored once

    mof_wbone* bn = m->bones;
    for (int i = 0; i < 3; i++) mof_wbone_default(&bn[i]);
    bn[0].name = "root"; bn[0].node = 0;
    bn[1].name = "mid"; bn[1].parent = 0; bn[1].node = 3; bn[1].translation[1] = 1; bn[1].rotation[0] = 0.5f; bn[1].rotation[1] = 0.5f; bn[1].rotation[2] = 0.5f; bn[1].rotation[3] = 0.5f;
    bn[1].scale[0] = 2; bn[1].inverse_bind[12] = -7;
    bn[2].name = "tip"; bn[2].parent = 1; bn[2].translation[2] = 5; bn[2].inverse_bind[13] = 9;

    mof_wnode* nd = m->nodes;
    for (int i = 0; i < 4; i++) mof_wnode_default(&nd[i]);
    nd[0].name = "scene_root";
    nd[1].name = "part_a"; nd[1].parent = 0; nd[1].mesh = 0; nd[1].translation[0] = 3;
    nd[2].name = "part_b"; nd[2].parent = 0; nd[2].mesh = 1; nd[2].skinned = 1; nd[2].scale[1] = 0.5f;
    nd[3].name = "leaf"; nd[3].parent = 2;

    m->desc.alloc = alloc;
    m->desc.mesh_count = 3; m->desc.meshes = m->meshes;
    m->desc.material_count = 2; m->desc.materials = m->materials;
    m->desc.texture_count = 4; m->desc.textures = m->textures;
    m->desc.bone_count = 3; m->desc.bones = m->bones;
    m->desc.node_count = 4; m->desc.nodes = m->nodes;
}

// ----------------------------------------------------------------- utilities

// Copy into an allocation of exactly `size` bytes, so ASan sees any over-read.
static uint8_t* exact_copy(const void* src, size_t size) {
    uint8_t* p = (uint8_t*)malloc(size ? size : 1);
    if (size) memcpy(p, src, size);
    return p;
}

// Touches every byte a consumer could follow from a validated view; every
// invariant the reader promises is checked again here. Returns a checksum.
static uint64_t walk_all(const mof_file& f) {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < f.chunk_count; i++) sum += f.chunks[i].type;
    for (const char* s = mof_string(&f, 0); *s; s++) sum += (uint8_t)*s;
    if (f.strings) for (uint32_t i = 0; i < f.strings_size; i++) sum += (uint8_t)f.strings[i];
    for (uint32_t i = 0; i < f.mesh_count; i++) {
        const mof_mesh& m = f.meshes[i];
        for (const char* s = mof_string(&f, m.name); *s; s++) sum += (uint8_t)*s;
        const float* p = mof_mesh_positions(&f, &m);
        CHECK(p != nullptr || m.vertex_count == 0);
        for (uint32_t v = 0; v < m.vertex_count * 3u; v++) sum += (uint32_t)p[v] & 1;
        const float* n = mof_mesh_normals(&f, &m);
        if (n) for (uint32_t v = 0; v < m.vertex_count * 3u; v++) sum += (uint32_t)n[v] & 1;
        const float* t = mof_mesh_tangents(&f, &m);
        if (t) for (uint32_t v = 0; v < m.vertex_count * 4u; v++) sum += (uint32_t)t[v] & 1;
        const float* u0 = mof_mesh_uv0(&f, &m);
        if (u0) for (uint32_t v = 0; v < m.vertex_count * 2u; v++) sum += (uint32_t)u0[v] & 1;
        const float* u1 = mof_mesh_uv1(&f, &m);
        if (u1) for (uint32_t v = 0; v < m.vertex_count * 2u; v++) sum += (uint32_t)u1[v] & 1;
        const uint8_t* c = mof_mesh_colors(&f, &m);
        if (c) for (uint32_t v = 0; v < m.vertex_count * 4u; v++) sum += c[v];
        const uint16_t* j = mof_mesh_joints(&f, &m);
        if (j) {
            for (uint32_t v = 0; v < m.vertex_count * 4u; v++) { CHECK(j[v] < f.bone_count); sum += j[v]; }
            const uint8_t* w = (const uint8_t*)mof_mesh_weights(&f, &m);
            uint32_t wb = m.vertex_count * (m.weight_format == MOF_WEIGHTS_F32 ? 16u : 8u);
            for (uint32_t v = 0; v < wb; v++) sum += w[v];
        }
        for (uint32_t k = 0; k < m.index_count; k++) {
            uint32_t ix = mof_mesh_index(&f, &m, k);
            CHECK(ix < m.vertex_count);
            sum += ix;
        }
        for (uint32_t k = 0; k < m.submesh_count; k++) {
            const mof_submesh& s = f.submeshes[m.first_submesh + k];
            CHECK(s.first_index + (uint64_t)s.index_count <= m.index_count);
            CHECK(s.material == MOF_NONE || s.material < f.material_count);
            sum += s.first_index + s.material;
        }
    }
    for (uint32_t i = 0; i < f.material_count; i++) {
        for (const char* s = mof_string(&f, f.materials[i].name); *s; s++) sum += (uint8_t)*s;
        for (uint32_t k = 0; k < MOF_SLOT_COUNT; k++) CHECK(f.materials[i].tex[k].texture == MOF_NONE || f.materials[i].tex[k].texture < f.texture_count);
    }
    for (uint32_t i = 0; i < f.texture_count; i++) {
        for (const char* s = mof_string(&f, f.textures[i].name); *s; s++) sum += (uint8_t)*s;
        uint64_t n;
        const uint8_t* b = mof_texture_bytes(&f, i, &n);
        CHECK(b != nullptr);
        for (uint64_t k = 0; k < n; k++) sum += b[k];
    }
    for (uint32_t i = 0; i < f.bone_count; i++) {
        for (const char* s = mof_string(&f, f.bones[i].name); *s; s++) sum += (uint8_t)*s;
        CHECK(f.bones[i].parent < (int32_t)i);
        sum += (uint32_t)f.bones[i].parent;
    }
    for (uint32_t i = 0; i < f.node_count; i++) {
        for (const char* s = mof_string(&f, f.nodes[i].name); *s; s++) sum += (uint8_t)*s;
        CHECK(f.nodes[i].parent < (int32_t)i);
        CHECK(f.nodes[i].mesh < (int32_t)f.mesh_count);
    }
    return sum;
}

static mof_chunk_entry* find_chunk(uint8_t* buf, uint32_t type) {
    mof_header* h = (mof_header*)buf;
    mof_chunk_entry* e = (mof_chunk_entry*)(buf + h->table_offset);
    for (uint32_t i = 0; i < h->chunk_count; i++)
        if (e[i].type == type) return &e[i];
    return nullptr;
}

static bool opens(const uint8_t* buf, size_t size, char* err = nullptr, size_t errlen = 0) {
    mof_file f;
    return mof_open(buf, size, &f, err, errlen);
}

// ---------------------------------------------------------------------- tests

static void test_roundtrip() {
    CountingAlloc ca;
    Model* m = (Model*)malloc(sizeof(Model));
    build_model(m, make_alloc(&ca));
    mof_buffer buf;
    char err[256] = {0};
    bool ok = mof_write(&m->desc, &buf, err, sizeof err);
    CHECK_MSG(ok, err);
    if (!ok) { free(m); return; }
    CHECK(buf.size % 16 == 0);
    CHECK(((uintptr_t)buf.data & 15) == 0);

    mof_file f;
    ok = mof_open(buf.data, buf.size, &f, err, sizeof err);
    CHECK_MSG(ok, err);
    if (!ok) { mof_buffer_free(&buf); free(m); return; }
    CHECK_EQ(f.version_major, 1);
    CHECK_EQ(f.version_minor, 0);
    CHECK_EQ(f.size, buf.size);
    CHECK_EQ(f.mesh_count, 3); CHECK_EQ(f.material_count, 2); CHECK_EQ(f.texture_count, 4);
    CHECK_EQ(f.bone_count, 3); CHECK_EQ(f.node_count, 4); CHECK_EQ(f.submesh_count, 2 + 1);
    for (uint32_t i = 0; i < f.chunk_count; i++) CHECK(f.chunks[i].offset % 16 == 0);

    // mesh 0: every stream
    const mof_mesh& a = f.meshes[0];
    CHECK_STR(mof_string(&f, a.name), "quad_pyramid");
    CHECK_EQ(a.vertex_count, 5); CHECK_EQ(a.index_count, 12); CHECK_EQ(a.index_format, MOF_INDEX_U32);
    CHECK_EQ(a.stream_mask, MOF_STREAM_NORMAL | MOF_STREAM_TANGENT | MOF_STREAM_UV0 | MOF_STREAM_UV1 | MOF_STREAM_COLOR | MOF_STREAM_SKIN);
    CHECK_EQ(a.weight_format, MOF_WEIGHTS_F32);
    CHECK(memcmp(mof_mesh_positions(&f, &a), kPos0, sizeof kPos0) == 0);
    CHECK(memcmp(mof_mesh_normals(&f, &a), kNrm0, sizeof kNrm0) == 0);
    CHECK(memcmp(mof_mesh_tangents(&f, &a), kTan0, sizeof kTan0) == 0);
    CHECK(memcmp(mof_mesh_uv0(&f, &a), kUv0, sizeof kUv0) == 0);
    CHECK(memcmp(mof_mesh_uv1(&f, &a), kUv1, sizeof kUv1) == 0);
    CHECK(memcmp(mof_mesh_colors(&f, &a), kCol0, sizeof kCol0) == 0);
    CHECK(memcmp(mof_mesh_joints(&f, &a), kJoints0, sizeof kJoints0) == 0);
    CHECK(memcmp(mof_mesh_weights(&f, &a), kWeights0, sizeof kWeights0) == 0);
    CHECK(memcmp(mof_mesh_indices(&f, &a), kIdx0, sizeof kIdx0) == 0);
    for (uint32_t i = 0; i < 12; i++) CHECK_EQ(mof_mesh_index(&f, &a, i), kIdx0[i]);
    CHECK_EQ(a.submesh_count, 2);
    CHECK_EQ(f.submeshes[a.first_submesh + 1].first_index, 6);
    CHECK_EQ(f.submeshes[a.first_submesh + 1].material, 1);
    // bounds computed by the writer
    CHECK_NEAR(a.bounds_min[0], 0); CHECK_NEAR(a.bounds_min[2], 0);
    CHECK_NEAR(a.bounds_max[0], 1); CHECK_NEAR(a.bounds_max[1], 1); CHECK_NEAR(a.bounds_max[2], 2);
    CHECK_NEAR(a.sphere_center[0], 0.5f); CHECK_NEAR(a.sphere_center[2], 1.0f);
    CHECK(fabsf(a.sphere_radius - sqrtf(1.5f)) < 1e-4f);

    // mesh 1: unorm16 weights, u16 indices, missing streams are null, default submesh
    const mof_mesh& b = f.meshes[1];
    CHECK_STR(mof_string(&f, b.name), "shared_name");
    CHECK_EQ(b.index_format, MOF_INDEX_U16);
    CHECK_EQ(b.weight_format, MOF_WEIGHTS_UNORM16);
    CHECK(mof_mesh_normals(&f, &b) == nullptr && mof_mesh_tangents(&f, &b) == nullptr && mof_mesh_uv1(&f, &b) == nullptr && mof_mesh_colors(&f, &b) == nullptr);
    CHECK(memcmp(mof_mesh_weights(&f, &b), kWeights1, sizeof kWeights1) == 0);
    CHECK(mof_mesh_uv0(&f, &b) != nullptr);
    CHECK_EQ(b.submesh_count, 1);
    CHECK_EQ(f.submeshes[b.first_submesh].index_count, 6);
    CHECK_EQ(f.submeshes[b.first_submesh].material, MOF_NONE);
    CHECK_NEAR(b.bounds_min[0], -1); CHECK_NEAR(b.bounds_max[1], 5); CHECK_NEAR(b.bounds_min[2], -3);
    // empty mesh
    CHECK_EQ(f.meshes[2].vertex_count, 0);
    CHECK_STR(mof_string(&f, f.meshes[2].name), "empty");
    // string dedupe: mesh 1 and material 0 share one string
    CHECK_EQ(f.meshes[1].name, f.materials[0].name);
    CHECK_EQ(f.materials[1].name, 0);
    CHECK_STR(mof_string(&f, 0), "");
    CHECK_STR(mof_string(&f, 0xFFFFFFF0u), "");

    // materials
    const mof_material& x = f.materials[0];
    CHECK_NEAR(x.base_color[3], 0.4f); CHECK_NEAR(x.emissive[2], 3); CHECK_NEAR(x.metallic, 0.25f); CHECK_NEAR(x.roughness, 0.75f);
    CHECK_NEAR(x.normal_scale, 2); CHECK_NEAR(x.occlusion_strength, 0.5f); CHECK_NEAR(x.alpha_cutoff, 0.33f);
    CHECK_EQ((x.flags & MOF_MATERIAL_ALPHA_MASK) >> MOF_MATERIAL_ALPHA_SHIFT, MOF_ALPHA_MASK);
    CHECK(x.flags & MOF_MATERIAL_DOUBLE_SIDED);
    for (uint32_t s = 0; s < MOF_SLOT_COUNT; s++) { CHECK_EQ(x.tex[s].texture, s % 3); CHECK_EQ(x.tex[s].uv_set, s & 1); }
    CHECK_EQ(f.materials[1].tex[0].texture, MOF_NONE);
    CHECK_NEAR(f.materials[1].base_color[0], 1); CHECK_NEAR(f.materials[1].roughness, 1);
    CHECK_EQ(f.materials[1].flags, 0);

    // textures
    const mof_texture& t0 = f.textures[0];
    CHECK_EQ(t0.codec, MOF_TEX_RAW_RGBA8); CHECK_EQ(t0.width, 4); CHECK_EQ(t0.mip_count, 3);
    CHECK_EQ(t0.wrap_s, MOF_WRAP_CLAMP); CHECK_EQ(t0.wrap_t, MOF_WRAP_MIRROR); CHECK_EQ(t0.min_filter, MOF_FILTER_NEAREST); CHECK_EQ(t0.mip_filter, MOF_MIP_LINEAR);
    uint64_t n;
    const uint8_t* bytes = mof_texture_bytes(&f, 0, &n);
    CHECK(n == sizeof kRaw && memcmp(bytes, kRaw, n) == 0);
    bytes = mof_texture_bytes(&f, 1, &n);
    CHECK(n == sizeof kPng && memcmp(bytes, kPng, n) == 0);
    CHECK_EQ(f.textures[1].codec, MOF_TEX_PNG); CHECK_EQ(f.textures[1].height, 2);
    bytes = mof_texture_bytes(&f, 2, &n);
    CHECK(n == sizeof kJpg && memcmp(bytes, kJpg, n) == 0);
    CHECK_EQ(f.textures[3].data_offset, f.textures[0].data_offset);  // shared payload
    CHECK(mof_texture_bytes(&f, 4, &n) == nullptr && n == 0);

    // bones
    CHECK_STR(mof_string(&f, f.bones[2].name), "tip");
    CHECK_EQ(f.bones[0].parent, -1); CHECK_EQ(f.bones[1].parent, 0); CHECK_EQ(f.bones[2].parent, 1);
    CHECK_EQ(f.bones[0].node, 0); CHECK_EQ(f.bones[1].node, 3); CHECK_EQ(f.bones[2].node, MOF_NONE);
    CHECK_NEAR(f.bones[1].rotation[3], 0.5f); CHECK_NEAR(f.bones[1].scale[0], 2); CHECK_NEAR(f.bones[1].translation[1], 1);
    CHECK_NEAR(f.bones[1].inverse_bind[12], -7); CHECK_NEAR(f.bones[2].inverse_bind[13], 9); CHECK_NEAR(f.bones[0].inverse_bind[15], 1);
    // nodes
    CHECK_STR(mof_string(&f, f.nodes[3].name), "leaf");
    CHECK_EQ(f.nodes[1].parent, 0); CHECK_EQ(f.nodes[1].mesh, 0); CHECK_EQ(f.nodes[2].mesh, 1);
    CHECK(f.nodes[2].flags & MOF_NODE_SKINNED); CHECK(!(f.nodes[1].flags & MOF_NODE_SKINNED));
    CHECK_NEAR(f.nodes[1].translation[0], 3); CHECK_NEAR(f.nodes[2].scale[1], 0.5f); CHECK_NEAR(f.nodes[0].rotation[3], 1);

    walk_all(f);

    // info printer smoke test
    FILE* tmp = tmpfile();
    if (tmp) {
        mof_print_info(&f, tmp);
        long len = ftell(tmp);
        rewind(tmp);
        char* text = (char*)calloc(1, (size_t)len + 1);
        size_t got = fread(text, 1, (size_t)len, tmp);
        (void)got;
        CHECK(strstr(text, "quad_pyramid") && strstr(text, "meshes: 3") && strstr(text, "bones: 3") && strstr(text, "\"tip\"") && strstr(text, "SKIN(unorm16)"));
        free(text);
        fclose(tmp);
    }
    mof_buffer_free(&buf);
    CHECK_EQ(ca.live, 0);
    free(m);
}

static void test_auto_index32() {
    CountingAlloc ca;
    const uint32_t nv = 70000;
    float* pos = (float*)calloc(nv * 3, sizeof(float));
    for (uint32_t i = 0; i < nv; i++) pos[i * 3] = (float)i;
    uint32_t idx[6] = {0, 1, 2, nv - 1, nv - 2, nv - 3};
    mof_wmesh me;
    mof_wmesh_default(&me);
    me.vertex_count = nv; me.positions = pos; me.index_count = 6; me.indices = idx;
    mof_wdesc d;
    memset(&d, 0, sizeof d);
    d.alloc = make_alloc(&ca); d.mesh_count = 1; d.meshes = &me;
    mof_buffer buf;
    char err[256];
    CHECK(mof_write(&d, &buf, err, sizeof err));
    mof_file f;
    CHECK(mof_open(buf.data, buf.size, &f, err, sizeof err));
    CHECK_EQ(f.meshes[0].index_format, MOF_INDEX_U32);
    CHECK_EQ(mof_mesh_index(&f, &f.meshes[0], 3), nv - 1);
    CHECK_NEAR(f.meshes[0].bounds_max[0], nv - 1);
    mof_buffer_free(&buf);
    CHECK_EQ(ca.live, 0);
    free(pos);
}

static void test_forward_compat() {
    CountingAlloc ca;
    Model* m = (Model*)malloc(sizeof(Model));
    build_model(m, make_alloc(&ca));
    static const uint8_t blob[40] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    m->extra[0].type = MOF_FOURCC('A', 'N', 'I', 'M'); m->extra[0].flags = 0; m->extra[0].data = blob; m->extra[0].size = sizeof blob;
    m->desc.extra_chunk_count = 1; m->desc.extra_chunks = m->extra;
    m->desc.minor_version = 7;  // a future minor version
    mof_buffer buf;
    char err[256];
    CHECK(mof_write(&m->desc, &buf, err, sizeof err));
    mof_file f;
    CHECK_MSG(mof_open(buf.data, buf.size, &f, err, sizeof err), err);
    CHECK_EQ(f.version_minor, 7);
    CHECK_EQ(f.mesh_count, 3);  // everything we know is still read
    bool seen = false;
    for (uint32_t i = 0; i < f.chunk_count; i++) seen |= f.chunks[i].type == MOF_FOURCC('A', 'N', 'I', 'M');
    CHECK(seen);
    walk_all(f);

    // an unknown chunk that is marked required must be rejected
    uint8_t* c = exact_copy(buf.data, buf.size);
    find_chunk(c, MOF_FOURCC('A', 'N', 'I', 'M'))->flags = MOF_CHUNK_FLAG_REQUIRED;
    CHECK(!opens(c, buf.size, err, sizeof err));
    CHECK(strstr(err, "required"));
    free(c);
    // future major version is rejected
    c = exact_copy(buf.data, buf.size);
    ((mof_header*)c)->version_major = 2;
    CHECK(!opens(c, buf.size));
    free(c);
    // a unknown chunk type with a different, unknown fourcc and non-zero reserved header bytes / flags are ignored
    c = exact_copy(buf.data, buf.size);
    ((mof_header*)c)->flags = 0xF0F0;
    ((mof_header*)c)->reserved[3] = 9;
    find_chunk(c, MOF_FOURCC('A', 'N', 'I', 'M'))->type = MOF_FOURCC('Z', 'Z', 'Z', 'Z');
    CHECK_MSG(opens(c, buf.size, err, sizeof err), err);
    free(c);
    mof_buffer_free(&buf);
    CHECK_EQ(ca.live, 0);
    free(m);
}

// A cook tool reads one texture / one mesh using only the head of the file
// (header + chunk table) and seeks; nothing else is parsed. `file` plays the disk.
static void test_partial_access() {
    CountingAlloc ca;
    Model* m = (Model*)malloc(sizeof(Model));
    build_model(m, make_alloc(&ca));
    mof_buffer buf;
    char err[256];
    CHECK(mof_write(&m->desc, &buf, err, sizeof err));
    const uint8_t* file = buf.data;

    uint8_t* first = exact_copy(file, 64);
    uint64_t hs = mof_head_size(first, 64);
    free(first);
    CHECK(hs >= 64 + 32 && hs < buf.size);
    uint8_t* head = exact_copy(file, (size_t)hs);  // exactly header + table: ASan guards the rest
    mof_chunk_entry text, texd, mesh, vert, indx, none;
    CHECK(mof_find_chunk(head, (size_t)hs, MOF_CHUNK_TEXTURES, &text, err, sizeof err));
    CHECK(mof_find_chunk(head, (size_t)hs, MOF_CHUNK_TEXDATA, &texd, err, sizeof err));
    CHECK(mof_find_chunk(head, (size_t)hs, MOF_CHUNK_MESHES, &mesh, err, sizeof err));
    CHECK(mof_find_chunk(head, (size_t)hs, MOF_CHUNK_VERTICES, &vert, err, sizeof err));
    CHECK(mof_find_chunk(head, (size_t)hs, MOF_CHUNK_INDICES, &indx, err, sizeof err));
    CHECK(!mof_find_chunk(head, (size_t)hs, MOF_FOURCC('N', 'O', 'P', 'E'), &none, err, sizeof err) && strstr(err, "not present"));
    CHECK_EQ(text.count, 4); CHECK_EQ(mesh.count, 3);
    // texture 1 on its own: one 64-byte record + its payload
    mof_texture t;
    memcpy(&t, file + text.offset + 1 * sizeof(mof_texture), sizeof t);
    CHECK(t.data_offset <= texd.size && t.data_size <= texd.size - t.data_offset);
    CHECK(t.data_size == sizeof kPng && memcmp(file + texd.offset + t.data_offset, kPng, sizeof kPng) == 0);
    // mesh 0 on its own: one 128-byte record, then its streams and indices
    mof_mesh me;
    memcpy(&me, file + mesh.offset, sizeof me);
    CHECK(me.off_position + (uint64_t)me.vertex_count * 12 <= vert.size);
    CHECK(memcmp(file + vert.offset + me.off_position, kPos0, sizeof kPos0) == 0);
    CHECK(memcmp(file + indx.offset + me.off_indices, kIdx0, sizeof kIdx0) == 0);
    free(head);

    // truncated heads are rejected without over-reading
    int ok = 0;
    for (size_t len = 0; len < (size_t)hs; len++) {
        uint8_t* c = exact_copy(file, len);
        if (mof_find_chunk(c, len, MOF_CHUNK_TEXTURES, &none, err, sizeof err)) ok++;
        uint64_t r = mof_head_size(c, len);
        CHECK(len < 64 ? r == 0 : r == hs);
        free(c);
    }
    CHECK_EQ(ok, 0);
    // a corrupt table is rejected by the partial functions too
    uint8_t* c = exact_copy(file, (size_t)hs);
    ((mof_chunk_entry*)(c + 64))[1].offset += 8;
    CHECK(!mof_find_chunk(c, (size_t)hs, MOF_CHUNK_TEXTURES, &none, err, sizeof err));
    ((mof_header*)c)->chunk_count = 0xFFFFFFFFu;
    CHECK_EQ(mof_head_size(c, (size_t)hs), 0);
    free(c);
    mof_buffer_free(&buf);
    free(m);
}


static mof_mesh* mesh_rec(uint8_t* b, int i) { return (mof_mesh*)(b + find_chunk(b, MOF_CHUNK_MESHES)->offset) + i; }
static mof_material* mat_rec(uint8_t* b, int i) { return (mof_material*)(b + find_chunk(b, MOF_CHUNK_MATERIALS)->offset) + i; }
static mof_texture* tex_rec(uint8_t* b, int i) { return (mof_texture*)(b + find_chunk(b, MOF_CHUNK_TEXTURES)->offset) + i; }
static mof_bone* bone_rec(uint8_t* b, int i) { return (mof_bone*)(b + find_chunk(b, MOF_CHUNK_SKELETON)->offset) + i; }
static mof_node* node_rec(uint8_t* b, int i) { return (mof_node*)(b + find_chunk(b, MOF_CHUNK_NODES)->offset) + i; }
static mof_submesh* sub_rec(uint8_t* b, int i) { return (mof_submesh*)(b + find_chunk(b, MOF_CHUNK_SUBMESHES)->offset) + i; }
static uint8_t* strs(uint8_t* b) { return b + find_chunk(b, MOF_CHUNK_STRINGS)->offset; }

static void test_targeted_corruption() {
    CountingAlloc ca;
    Model* m = (Model*)malloc(sizeof(Model));
    build_model(m, make_alloc(&ca));
    mof_buffer buf;
    char err[256];
    CHECK(mof_write(&m->desc, &buf, err, sizeof err));
    const size_t size = buf.size;
    const uint64_t BIG = 0xFFFFFFFFFFFFFFFFull;

#define MUT(NAME, CODE) \
    do { \
        uint8_t* b = exact_copy(buf.data, size); \
        mof_header* H = (mof_header*)b; (void)H; \
        mof_chunk_entry* E = (mof_chunk_entry*)(b + H->table_offset); (void)E; \
        CODE; \
        char e2[256] = {0}; \
        bool acc = opens(b, size, e2, sizeof e2); \
        g_checks++; \
        if (acc) { g_failures++; fprintf(stderr, "%s:%d: corruption accepted: %s\n", __FILE__, __LINE__, NAME); } \
        else if (!e2[0]) { g_failures++; fprintf(stderr, "no error message for %s\n", NAME); } \
        free(b); \
    } while (0)

    MUT("bad magic", b[0] = 'X');
    MUT("major 0", H->version_major = 0);
    MUT("major 99", H->version_major = 99);
    MUT("header_size small", H->header_size = 32);
    MUT("header_size unaligned", H->header_size = 72);
    MUT("entry size", H->chunk_entry_size = 48);
    MUT("file_size beyond buffer", H->file_size = size + 16);
    MUT("file_size huge", H->file_size = BIG);
    MUT("file_size zero", H->file_size = 0);
    MUT("chunk_count huge", H->chunk_count = 0xFFFFFFFFu);
    MUT("chunk_count 257", H->chunk_count = 257);
    MUT("chunk_count overshoot", H->chunk_count = H->chunk_count + 40);
    MUT("table_offset unaligned", H->table_offset = 72);
    MUT("table_offset in header", H->table_offset = 16);
    MUT("table_offset huge", H->table_offset = BIG - 7);
    MUT("table_offset at end", H->table_offset = size);
    MUT("chunk offset unaligned", E[1].offset += 4);
    MUT("chunk offset huge", E[1].offset = BIG & ~15ull);
    MUT("chunk offset in table", E[0].offset = 64);
    MUT("chunk offset wraps", { E[2].offset = BIG - 15; E[2].size = 32; });
    MUT("chunk size huge", E[1].size = BIG);
    MUT("chunk size past end", E[3].size = size);
    MUT("chunks overlap", E[2].offset = E[1].offset);
    MUT("duplicate chunk", E[1].type = E[0].type);
    MUT("mesh count mismatch", find_chunk(b, MOF_CHUNK_MESHES)->count++);
    MUT("mesh count huge", find_chunk(b, MOF_CHUNK_MESHES)->count = 0xFFFFFFFFu);
    MUT("material count mismatch", find_chunk(b, MOF_CHUNK_MATERIALS)->count = 3);
    MUT("string table no NUL end", strs(b)[find_chunk(b, MOF_CHUNK_STRINGS)->size - 1] = 'x');
    MUT("string table no NUL start", strs(b)[0] = 'x');
    MUT("string table empty", find_chunk(b, MOF_CHUNK_STRINGS)->size = 0);
    MUT("mesh name offset", mesh_rec(b, 0)->name = find_chunk(b, MOF_CHUNK_STRINGS)->size);
    MUT("mesh name offset huge", mesh_rec(b, 1)->name = 0xFFFFFFFFu - 1);
    MUT("material name offset", mat_rec(b, 0)->name = 0x7FFFFFFF);
    MUT("texture name offset", tex_rec(b, 1)->name = 0x80000000u);
    MUT("bone name offset", bone_rec(b, 2)->name = 0xFFFFFF00u);
    MUT("node name offset", node_rec(b, 3)->name = 0x10000000);
    MUT("mesh vertex_count huge", mesh_rec(b, 0)->vertex_count = 0xFFFFFFFFu);
    MUT("mesh vertex_count +1", mesh_rec(b, 0)->vertex_count += 1000);
    MUT("mesh index_count huge", mesh_rec(b, 0)->index_count = 0xFFFFFFFFu);
    MUT("mesh index_count not tri", mesh_rec(b, 0)->index_count = 11);
    MUT("mesh bad index format", mesh_rec(b, 0)->index_format = 2);
    MUT("mesh bad weight format", mesh_rec(b, 1)->weight_format = 7);
    MUT("position offset unaligned", mesh_rec(b, 0)->off_position += 4);
    MUT("position offset huge", mesh_rec(b, 0)->off_position = 0xFFFFFFF0u);
    MUT("normal offset oob", mesh_rec(b, 0)->off_normal = 0x7FFFFFF0u);
    MUT("tangent offset oob", mesh_rec(b, 0)->off_tangent = 0x7FFFFFF0u);
    MUT("uv0 offset oob", mesh_rec(b, 0)->off_uv0 = 0x7FFFFFF0u);
    MUT("uv1 offset oob", mesh_rec(b, 0)->off_uv1 = 0x7FFFFFF0u);
    MUT("color offset oob", mesh_rec(b, 0)->off_color = 0x7FFFFFF0u);
    MUT("joint offset oob", mesh_rec(b, 0)->off_joints = 0x7FFFFFF0u);
    MUT("weight offset oob", mesh_rec(b, 1)->off_weights = 0x7FFFFFF0u);
    MUT("index offset oob", mesh_rec(b, 0)->off_indices = 0xFFFFFFF0u);
    MUT("index offset unaligned", mesh_rec(b, 1)->off_indices += 2);
    MUT("index value oob (u32)", ((uint32_t*)(b + find_chunk(b, MOF_CHUNK_INDICES)->offset + mesh_rec(b, 0)->off_indices))[4] = 5);
    MUT("index value huge (u32)", ((uint32_t*)(b + find_chunk(b, MOF_CHUNK_INDICES)->offset + mesh_rec(b, 0)->off_indices))[0] = 0xFFFFFFFFu);
    MUT("index value oob (u16)", ((uint16_t*)(b + find_chunk(b, MOF_CHUNK_INDICES)->offset + mesh_rec(b, 1)->off_indices))[5] = 4);
    MUT("joint value oob", ((uint16_t*)(b + find_chunk(b, MOF_CHUNK_VERTICES)->offset + mesh_rec(b, 0)->off_joints))[7] = 3);
    MUT("joint value huge", ((uint16_t*)(b + find_chunk(b, MOF_CHUNK_VERTICES)->offset + mesh_rec(b, 1)->off_joints))[0] = 0xFFFF);
    MUT("skin without skeleton", find_chunk(b, MOF_CHUNK_SKELETON)->type = MOF_FOURCC('Z', 'Z', 'Z', 'Z'));
    MUT("submesh first_submesh oob", mesh_rec(b, 0)->first_submesh = 3);
    MUT("submesh_count oob", mesh_rec(b, 0)->submesh_count = 0xFFFFFFFFu);
    MUT("submesh range oob", sub_rec(b, 0)->index_count = 15);
    MUT("submesh first oob", sub_rec(b, 1)->first_index = 0xFFFFFFFFu);
    MUT("submesh not tri", sub_rec(b, 0)->index_count = 4);
    MUT("submesh material oob", sub_rec(b, 0)->material = 2);
    MUT("bounds NaN", mesh_rec(b, 0)->bounds_min[1] = nanf(""));
    MUT("bounds inf", mesh_rec(b, 0)->bounds_max[0] = INFINITY);
    MUT("bounds min>max", mesh_rec(b, 0)->bounds_min[0] = 5);
    MUT("negative radius", mesh_rec(b, 0)->sphere_radius = -1);
    MUT("material tex oob", mat_rec(b, 0)->tex[2].texture = 4);
    MUT("material uv set", mat_rec(b, 0)->tex[0].uv_set = 2);
    MUT("material alpha mode", mat_rec(b, 0)->flags |= 3u << 1);
    MUT("material NaN", mat_rec(b, 1)->roughness = nanf(""));
    MUT("texture codec", tex_rec(b, 0)->codec = 3);
    MUT("texture wrap", tex_rec(b, 0)->wrap_s = 3);
    MUT("texture filter", tex_rec(b, 0)->mag_filter = 2);
    MUT("texture mip filter", tex_rec(b, 0)->mip_filter = 3);
    MUT("texture offset unaligned", tex_rec(b, 0)->data_offset += 8);
    MUT("texture offset huge", tex_rec(b, 0)->data_offset = BIG & ~15ull);
    MUT("texture size huge", tex_rec(b, 0)->data_size = BIG);
    MUT("texture size mismatch", tex_rec(b, 0)->data_size -= 4);
    MUT("texture dims zero", tex_rec(b, 0)->width = 0);
    MUT("texture dims huge", tex_rec(b, 0)->width = 0xFFFFFFFFu);
    MUT("texture dims overflow", { tex_rec(b, 0)->width = 16384; tex_rec(b, 0)->height = 16384; });
    MUT("texture mips too many", tex_rec(b, 0)->mip_count = 4);
    MUT("texture mips zero", tex_rec(b, 0)->mip_count = 0);
    MUT("texture mips huge", tex_rec(b, 0)->mip_count = 0xFFFFFFFFu);
    MUT("png mips", tex_rec(b, 1)->mip_count = 2);
    MUT("png bad signature", b[find_chunk(b, MOF_CHUNK_TEXDATA)->offset + tex_rec(b, 1)->data_offset + 1] = 'X');
    MUT("jpeg bad signature", b[find_chunk(b, MOF_CHUNK_TEXDATA)->offset + tex_rec(b, 2)->data_offset] = 0);
    MUT("bone parent self", bone_rec(b, 1)->parent = 1);
    MUT("bone parent forward", bone_rec(b, 0)->parent = 2);
    MUT("bone parent -2", bone_rec(b, 1)->parent = -2);
    MUT("bone parent huge", bone_rec(b, 2)->parent = 0x7FFFFFFF);
    MUT("bone node oob", bone_rec(b, 0)->node = 4);
    MUT("bone NaN", bone_rec(b, 0)->inverse_bind[5] = nanf(""));
    MUT("node parent self", node_rec(b, 2)->parent = 2);
    MUT("node parent forward", node_rec(b, 0)->parent = 3);
    MUT("node mesh oob", node_rec(b, 1)->mesh = 3);
    MUT("node mesh -2", node_rec(b, 1)->mesh = -2);
    MUT("node NaN", node_rec(b, 1)->scale[2] = INFINITY);

    // buffer alignment: reader needs a 16-byte aligned base
    uint8_t* mis = (uint8_t*)malloc(size + 32);
    memcpy(mis + 8, buf.data, size);
    CHECK(!opens(mis + 8, size, err, sizeof err));
    CHECK(strstr(err, "aligned"));
    free(mis);
    // trivial bad arguments
    mof_file f;
    CHECK(!mof_open(nullptr, 100, &f, err, sizeof err));
    CHECK(!mof_open(buf.data, 0, &f, err, sizeof err));
    CHECK(!mof_open(buf.data, 63, &f, err, sizeof err));
    CHECK(!mof_open(buf.data, size, nullptr, err, sizeof err));
    CHECK(mof_open(buf.data, size, &f, nullptr, 0));  // err may be null
    // trailing bytes after file_size are tolerated (file embedded in a larger buffer)
    uint8_t* longer = (uint8_t*)aligned_alloc(16, size + 64);
    memset(longer, 0xAB, size + 64);
    memcpy(longer, buf.data, size);
    CHECK(mof_open(longer, size + 64, &f, err, sizeof err));
    CHECK_EQ(f.size, size);
    free(longer);

    mof_buffer_free(&buf);
    free(m);
}

static void test_truncation(const char* name, const uint8_t* data, size_t size) {
    int accepted = 0;
    for (size_t len = 0; len < size; len++) {
        uint8_t* c = exact_copy(data, len);  // exact-size allocation: ASan flags any over-read
        mof_file f;
        char err[128];
        if (mof_open(c, len, &f, err, sizeof err)) { accepted++; walk_all(f); }
        free(c);
    }
    CHECK_MSG(accepted == 0, name);
    uint8_t* c = exact_copy(data, size);
    CHECK(opens(c, size));  // the complete file is fine
    free(c);
}

static uint32_t fuzz_iters() {
    const char* e = getenv("MOF_FUZZ_ITERS");
    return e ? (uint32_t)atoi(e) : 20000;
}

static void fuzz(const char* name, const uint8_t* data, size_t size, uint32_t iters, unsigned long long seed) {
    Rng rng(seed);
    const mof_header* h = (const mof_header*)data;
    const mof_chunk_entry* ce = (const mof_chunk_entry*)(data + h->table_offset);
    uint32_t accepted = 0;
    uint8_t* c = (uint8_t*)malloc(size);
    uint64_t sink = 0;
    for (uint32_t it = 0; it < iters; it++) {
        free(c);
        c = exact_copy(data, size);  // fresh exact-size allocation each time
        uint32_t nmut = 1 + rng.below(4);
        for (uint32_t k = 0; k < nmut; k++) {
            size_t pos;
            switch (rng.below(4)) {
                case 0: pos = rng.below((uint32_t)size); break;                                           // anywhere
                case 1: pos = rng.below((uint32_t)(h->table_offset + 32 * h->chunk_count)); break;        // header + table
                default: {                                                                              // inside a record chunk
                    const mof_chunk_entry& e = ce[rng.below(h->chunk_count)];
                    pos = e.size ? (size_t)e.offset + rng.below((uint32_t)(e.size > 4096 ? 4096 : e.size)) : 0;
                }
            }
            if (pos + 4 > size) pos = size - 4;
            uint32_t v;
            switch (rng.below(8)) {
                case 0: v = 0; break;
                case 1: v = 0xFFFFFFFFu; break;
                case 2: v = 0x80000000u; break;
                case 3: memcpy(&v, c + pos, 4); v += 1; break;
                case 4: memcpy(&v, c + pos, 4); v -= 1; break;
                case 5: v = 0x7FFFFFFFu; break;
                case 6: c[pos] ^= (uint8_t)(1u << rng.below(8)); continue;  // single bit flip
                default: v = (uint32_t)rng.next(); break;
            }
            memcpy(c + pos, &v, 4);
        }
        mof_file f;
        char err[128];
        if (mof_open(c, size, &f, err, sizeof err)) { accepted++; sink += walk_all(f); }
    }
    free(c);
    printf("  fuzz %-10s %u mutations, %u accepted (and walked), %u rejected%s\n", name, iters, accepted, iters - accepted, sink == 1 ? " " : "");
    CHECK(accepted < iters);  // the loop must exercise the rejection paths
}

static void test_writer_errors() {
    CountingAlloc ca;
    mof_buffer buf;
    char err[256];
    mof_wdesc d;
    memset(&d, 0, sizeof d);
    CHECK(!mof_write(&d, &buf, err, sizeof err));  // no allocator
    d.alloc = make_alloc(&ca);

    float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    uint32_t idx[3] = {0, 1, 2}, bad[3] = {0, 1, 3};
    mof_wmesh me;
    mof_wmesh_default(&me);
    me.vertex_count = 3; me.index_count = 3; me.indices = idx;
    d.mesh_count = 1; d.meshes = &me;
    CHECK(!mof_write(&d, &buf, err, sizeof err) && strstr(err, "positions"));  // no positions
    me.positions = pos; me.indices = bad;
    CHECK(!mof_write(&d, &buf, err, sizeof err) && strstr(err, "index"));
    me.indices = idx; me.index_count = 2;  // not a triangle list: caught by the verification pass
    CHECK(!mof_write(&d, &buf, err, sizeof err) && strstr(err, "multiple of 3"));
    me.index_count = 3;
    uint16_t j[12] = {0};
    me.joints = j;  // joints without weights
    CHECK(!mof_write(&d, &buf, err, sizeof err) && strstr(err, "weights"));
    float w[12] = {1};
    me.weights_f32 = w;  // skin but no skeleton: caught by verification
    CHECK(!mof_write(&d, &buf, err, sizeof err) && strstr(err, "skeleton"));
    me.joints = nullptr; me.weights_f32 = nullptr;
    mof_wsubmesh sub = {0, 3, 5};  // material index with no materials
    me.submeshes = &sub; me.submesh_count = 1;
    CHECK(!mof_write(&d, &buf, err, sizeof err) && strstr(err, "material"));
    me.submeshes = nullptr; me.submesh_count = 0;
    CHECK(mof_write(&d, &buf, err, sizeof err));
    mof_buffer_free(&buf);
    CHECK_EQ(ca.live, 0);

    // allocation failure at every point: never crashes, never leaks, reports an error
    Model* m = (Model*)malloc(sizeof(Model));
    int failed = 0, succeeded = 0;
    for (long n = 0; n < 200; n++) {
        CountingAlloc fa;
        fa.fail_after = n;
        build_model(m, make_alloc(&fa));
        bool ok = mof_write(&m->desc, &buf, err, sizeof err);
        if (ok) { succeeded++; mof_buffer_free(&buf); } else { failed++; CHECK(buf.data == nullptr); CHECK(err[0] != 0); }
        CHECK_EQ(fa.live, 0);
    }
    CHECK(failed >= 5 && succeeded > 0);
    printf("  alloc-failure sweep: %d failing points, %d succeeding\n", failed, succeeded);
    free(m);
}

static void test_samples() {
    for (unsigned i = 0; i < mof_sample_count; i++) {
        CountingAlloc ca;
        mof_buffer gen;
        char err[256];
        CHECK_MSG(mof_sample_build(i, make_alloc(&ca), &gen, err, sizeof err), err);
        char path[256];
        snprintf(path, sizeof path, "assets/samples/%s", mof_samples[i].file);
        FILE* fp = fopen(path, "rb");
        CHECK_MSG(fp != nullptr, path);
        if (!fp) { mof_buffer_free(&gen); continue; }
        fseek(fp, 0, SEEK_END);
        long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        uint8_t* disk = (uint8_t*)aligned_alloc(16, ((size_t)n + 15) & ~(size_t)15);
        size_t got = fread(disk, 1, (size_t)n, fp);
        fclose(fp);
        CHECK(got == (size_t)n);
        CHECK_MSG((size_t)n == gen.size && memcmp(disk, gen.data, gen.size) == 0, "sample file is stale: run `make mof-samples`");
        mof_file f;
        CHECK_MSG(mof_open(disk, (size_t)n, &f, err, sizeof err), err);
        walk_all(f);
        // triangles must face outwards: geometric normal agrees with the vertex normal of the first corner
        const mof_mesh& me = f.meshes[0];
        const float *P = mof_mesh_positions(&f, &me), *N = mof_mesh_normals(&f, &me);
        for (uint32_t t = 0; t + 2 < me.index_count; t += 3) {
            if (i == 1 && (mof_mesh_index(&f, &me, t) < 4 || mof_mesh_index(&f, &me, t) >= 16) && mof_mesh_index(&f, &me, t + 1) / 4 == mof_mesh_index(&f, &me, t) / 4) continue;  // caps
            const float *a = P + 3 * mof_mesh_index(&f, &me, t), *b = P + 3 * mof_mesh_index(&f, &me, t + 1), *c = P + 3 * mof_mesh_index(&f, &me, t + 2);
            float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            float g[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const float* nn = N + 3 * mof_mesh_index(&f, &me, t);
            CHECK(g[0] * nn[0] + g[1] * nn[1] + g[2] * nn[2] > 0);
        }
        if (i == 0) {
            CHECK_EQ(me.vertex_count, 24); CHECK_EQ(me.index_count, 36);
            CHECK_EQ(f.textures[0].mip_count, 5);
            CHECK_NEAR(me.bounds_min[0], -0.5f); CHECK_NEAR(me.bounds_max[2], 0.5f);
            // tangent convention: bitangent = cross(n, t) * w is image-up, i.e. points to decreasing v
            const float *T = mof_mesh_tangents(&f, &me), *UV = mof_mesh_uv0(&f, &me);
            for (uint32_t t = 0; t + 2 < me.index_count; t += 3) {
                uint32_t i0 = mof_mesh_index(&f, &me, t), i1 = mof_mesh_index(&f, &me, t + 1), i2 = mof_mesh_index(&f, &me, t + 2);
                float du = UV[i1 * 2] - UV[i0 * 2];
                float e[3] = {P[i1 * 3] - P[i0 * 3], P[i1 * 3 + 1] - P[i0 * 3 + 1], P[i1 * 3 + 2] - P[i0 * 3 + 2]};
                CHECK(du != 0 ? (e[0] * T[i0 * 4] + e[1] * T[i0 * 4 + 1] + e[2] * T[i0 * 4 + 2]) * du > 0 : true);  // t follows +u
                (void)i2;
            }
        } else {
            CHECK_EQ(f.bone_count, 2); CHECK_EQ(f.node_count, 4);
            const uint16_t* J = mof_mesh_joints(&f, &me);
            const float* W = (const float*)mof_mesh_weights(&f, &me);
            for (uint32_t v = 0; v < me.vertex_count; v++) {
                CHECK_NEAR(W[v * 4] + W[v * 4 + 1] + W[v * 4 + 2] + W[v * 4 + 3], 1.0f);
                (void)J;
            }
            // skinning at the bind pose reproduces the mesh: inverse_bind * (world bind) = identity
            CHECK_NEAR(f.bones[1].translation[1] + f.bones[1].inverse_bind[13], 0);
        }
        // fuzzing the samples too
        fuzz(mof_samples[i].file, disk, (size_t)n, 3000, 1000 + i);
        free(disk);
        mof_buffer_free(&gen);
    }
}

int main() {
    printf("mof_tests\n");
    test_roundtrip();
    test_auto_index32();
    test_forward_compat();
    test_partial_access();
    test_targeted_corruption();
    test_writer_errors();

    // truncation + fuzz on the full-featured synthetic model
    CountingAlloc ca;
    Model* m = (Model*)malloc(sizeof(Model));
    build_model(m, make_alloc(&ca));
    mof_buffer buf;
    char err[256];
    CHECK_MSG(mof_write(&m->desc, &buf, err, sizeof err), err);
    if (buf.data) {
        test_truncation("synthetic", buf.data, buf.size);
        fuzz("synthetic", buf.data, buf.size, fuzz_iters(), 0x1234abcdULL);
        mof_buffer_free(&buf);
    }
    free(m);
    test_samples();
    return finish_tests("mof_tests");
}
