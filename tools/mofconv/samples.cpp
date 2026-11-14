// Sample models: a textured cube and a skinned two-bone bar. Built with the
// public writer API only, so they also serve as a usage example.
#include "samples.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

const mof_sample_info mof_samples[] = {
    {"cube_textured.mof", "unit cube, normals + tangents + UV0, checker texture (raw RGBA8, 5 mips)"},
    {"skinned_bar.mof", "bar 2 units tall with two bones (bend at y = 1): skin weights, skeleton, node hierarchy"},
};
const unsigned mof_sample_count = sizeof(mof_samples) / sizeof(mof_samples[0]);

namespace {

// ---- cube: 24 vertices (4 per face), one material with a base color texture
struct CubeData {
    float pos[24 * 3], nrm[24 * 3], tan[24 * 4], uv[24 * 2];
    uint32_t idx[36];
    uint8_t tex[(16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1) * 4];
};

void build_cube(CubeData& c) {
    // face: normal, tangent (u direction); bitangent = cross(n, t) is the image-up direction (+v is down)
    static const float faces[6][6] = {
        {1, 0, 0, 0, 0, -1},  {-1, 0, 0, 0, 0, 1}, {0, 1, 0, 1, 0, 0},
        {0, -1, 0, 1, 0, 0},  {0, 0, 1, 1, 0, 0},  {0, 0, -1, -1, 0, 0},
    };
    for (int f = 0; f < 6; f++) {
        const float *n = faces[f], *t = faces[f] + 3;
        float b[3] = {n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0]};  // image-up
        for (int k = 0; k < 4; k++) {
            float su = (k == 1 || k == 2) ? 1.0f : -1.0f;  // corners: (-,-) (+,-) (+,+) (-,+) in (u, up)
            float sv = (k >= 2) ? 1.0f : -1.0f;
            int v = f * 4 + k;
            for (int a = 0; a < 3; a++) {
                c.pos[v * 3 + a] = 0.5f * (n[a] + su * t[a] + sv * b[a]);
                c.nrm[v * 3 + a] = n[a];
                c.tan[v * 4 + a] = t[a];
            }
            c.tan[v * 4 + 3] = 1.0f;
            c.uv[v * 2] = 0.5f + 0.5f * su;
            c.uv[v * 2 + 1] = 0.5f - 0.5f * sv;  // v points down the image
        }
        uint32_t o = (uint32_t)f * 4, *i = c.idx + f * 6;
        i[0] = o; i[1] = o + 1; i[2] = o + 2; i[3] = o; i[4] = o + 2; i[5] = o + 3;  // counter-clockwise from outside
    }
    // checker texture, level 0 is 16x16 with 4x4 pixel squares; mips are box filtered
    uint8_t* p = c.tex;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            bool a = ((x / 4) + (y / 4)) & 1;
            *p++ = a ? 230 : 40; *p++ = a ? 200 : 60; *p++ = a ? 60 : 160; *p++ = 255;
        }
    const uint8_t* prev = c.tex;
    for (int w = 8; w >= 1; w /= 2) {
        for (int y = 0; y < w; y++)
            for (int x = 0; x < w; x++)
                for (int ch = 0; ch < 4; ch++) {
                    int s = 0;
                    for (int dy = 0; dy < 2; dy++)
                        for (int dx = 0; dx < 2; dx++) s += prev[(((y * 2 + dy) * (w * 2)) + x * 2 + dx) * 4 + ch];
                    *p++ = (uint8_t)((s + 2) / 4);
                }
        prev = p - (size_t)w * w * 4;
    }
}

bool sample_cube(mof_alloc alloc, mof_buffer* out, char* err, size_t errlen) {
    static CubeData c;
    build_cube(c);
    mof_wmesh mesh;
    mof_wmesh_default(&mesh);
    mesh.name = "cube";
    mesh.vertex_count = 24; mesh.positions = c.pos; mesh.normals = c.nrm; mesh.tangents = c.tan; mesh.uv0 = c.uv;
    mesh.index_count = 36; mesh.indices = c.idx;
    mof_wsubmesh sub = {0, 36, 0};
    mesh.submeshes = &sub; mesh.submesh_count = 1;
    mof_wtexture tex;
    mof_wtexture_default(&tex);
    tex.name = "checker"; tex.codec = MOF_TEX_RAW_RGBA8; tex.width = tex.height = 16; tex.mip_count = 5;
    tex.data = c.tex; tex.data_size = sizeof c.tex;
    tex.min_filter = MOF_FILTER_LINEAR; tex.mip_filter = MOF_MIP_LINEAR; tex.mag_filter = MOF_FILTER_NEAREST;
    mof_wmaterial mat;
    mof_wmaterial_default(&mat);
    mat.name = "checker_material"; mat.metallic = 0.0f; mat.roughness = 0.8f;
    mat.tex[MOF_SLOT_BASE_COLOR].texture = 0;
    mof_wnode node;
    mof_wnode_default(&node);
    node.name = "cube"; node.mesh = 0;
    mof_wdesc d;
    memset(&d, 0, sizeof d);
    d.alloc = alloc;
    d.mesh_count = 1; d.meshes = &mesh;
    d.material_count = 1; d.materials = &mat;
    d.texture_count = 1; d.textures = &tex;
    d.node_count = 1; d.nodes = &node;
    return mof_write(&d, out, err, errlen);
}

// ---- bar: 5 rings of 4 vertices along Y (0 .. 2), weights blend between two bones around y = 1
struct BarData {
    float pos[20 * 3], nrm[20 * 3];
    uint8_t col[20 * 4];
    uint16_t joints[20 * 4];
    float weights[20 * 4];
    uint32_t idx[(4 * 4 * 2 + 2 * 2) * 3];
};

bool sample_bar(mof_alloc alloc, mof_buffer* out, char* err, size_t errlen) {
    static BarData b;
    memset(&b, 0, sizeof b);
    static const float cx[4] = {-0.25f, 0.25f, 0.25f, -0.25f}, cz[4] = {-0.25f, -0.25f, 0.25f, 0.25f};
    static const float w1[5] = {0.0f, 0.0f, 0.5f, 1.0f, 1.0f};  // weight of bone 1 per ring
    for (int r = 0; r < 5; r++)
        for (int k = 0; k < 4; k++) {
            int v = r * 4 + k;
            b.pos[v * 3] = cx[k]; b.pos[v * 3 + 1] = 0.5f * (float)r; b.pos[v * 3 + 2] = cz[k];
            b.nrm[v * 3] = cx[k] * 2.0f * 0.70710678f; b.nrm[v * 3 + 2] = cz[k] * 2.0f * 0.70710678f;
            b.col[v * 4] = (uint8_t)(255 - 60 * r); b.col[v * 4 + 1] = (uint8_t)(80 + 40 * r); b.col[v * 4 + 2] = 128; b.col[v * 4 + 3] = 255;
            b.joints[v * 4] = 0; b.joints[v * 4 + 1] = 1;
            b.weights[v * 4] = 1.0f - w1[r]; b.weights[v * 4 + 1] = w1[r];
        }
    uint32_t* i = b.idx;
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) {
            uint32_t a = (uint32_t)(r * 4 + k), bb = (uint32_t)(r * 4 + (k + 1) % 4), c = a + 4, d = bb + 4;
            *i++ = a; *i++ = d; *i++ = bb; *i++ = a; *i++ = c; *i++ = d;  // outward facing (counter-clockwise)
        }
    *i++ = 0; *i++ = 1; *i++ = 2; *i++ = 0; *i++ = 2; *i++ = 3;      // bottom cap (faces -Y)
    *i++ = 16; *i++ = 18; *i++ = 17; *i++ = 16; *i++ = 19; *i++ = 18; // top cap (faces +Y)

    mof_wmesh mesh;
    mof_wmesh_default(&mesh);
    mesh.name = "bar";
    mesh.vertex_count = 20; mesh.positions = b.pos; mesh.normals = b.nrm; mesh.colors = b.col;
    mesh.joints = b.joints; mesh.weights_f32 = b.weights;
    mesh.index_count = (uint32_t)(i - b.idx); mesh.indices = b.idx;
    mof_wmaterial mat;
    mof_wmaterial_default(&mat);
    mat.name = "bar_material"; mat.metallic = 0.0f; mat.roughness = 0.6f; mat.double_sided = 1;
    mat.base_color[0] = 0.8f; mat.base_color[1] = 0.8f; mat.base_color[2] = 0.9f;
    mof_wsubmesh sub = {0, mesh.index_count, 0};
    mesh.submeshes = &sub; mesh.submesh_count = 1;

    mof_wbone bones[2];
    mof_wbone_default(&bones[0]);
    mof_wbone_default(&bones[1]);
    bones[0].name = "root_bone"; bones[0].node = 1;
    bones[1].name = "tip_bone"; bones[1].parent = 0; bones[1].node = 2; bones[1].translation[1] = 1.0f;
    bones[1].inverse_bind[13] = -1.0f;  // inverse of translate(0,1,0), column-major
    mof_wnode nodes[4];
    for (int k = 0; k < 4; k++) mof_wnode_default(&nodes[k]);
    nodes[0].name = "armature";
    nodes[1].name = "root_bone"; nodes[1].parent = 0;
    nodes[2].name = "tip_bone"; nodes[2].parent = 1; nodes[2].translation[1] = 1.0f;
    nodes[3].name = "bar"; nodes[3].parent = 0; nodes[3].mesh = 0; nodes[3].skinned = 1;

    mof_wdesc d;
    memset(&d, 0, sizeof d);
    d.alloc = alloc;
    d.mesh_count = 1; d.meshes = &mesh;
    d.material_count = 1; d.materials = &mat;
    d.bone_count = 2; d.bones = bones;
    d.node_count = 4; d.nodes = nodes;
    return mof_write(&d, out, err, errlen);
}

}  // namespace

bool mof_sample_build(unsigned index, mof_alloc alloc, mof_buffer* out, char* err, size_t errlen) {
    switch (index) {
        case 0: return sample_cube(alloc, out, err, errlen);
        case 1: return sample_bar(alloc, out, err, errlen);
    }
    if (err && errlen) snprintf(err, errlen, "no such sample");
    return false;
}
