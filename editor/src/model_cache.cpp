#include "model_cache.h"
#include "core/vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe {

namespace {

constexpr int kMaxModels = 256;              // foliage layers with LOD files use three each
Model* g_models[kMaxModels];
int g_count = 0;
ModelReader g_reader = nullptr;

void* default_read(const char* path, size_t* size) { return vfs::read(path, size); }

void free_model(Model* m) {
    for (int i = 0; i < m->part_count; i++) {
        free(m->parts[i].positions);
        free(m->parts[i].normals);
        free(m->parts[i].uvs);
    }
    free(m->parts);
    free(m->mesh.positions);
    free(m->mesh.normals);
    free(m->block);
    delete m;
}

uint32_t to_srgb8(float linear) {
    float v = linear < 0 ? 0 : (linear > 1 ? 1 : linear);
    return (uint32_t)(fsqrt(v) * 255.0f + 0.5f);       // gamma 2.0: close enough to sRGB for a tint
}

Mat4 node_local(const mof_node& n) {
    Quat q;
    q.x = n.rotation[0]; q.y = n.rotation[1]; q.z = n.rotation[2]; q.w = n.rotation[3];
    return Mat4::trs(Vec3(n.translation[0], n.translation[1], n.translation[2]), q, Vec3(n.scale[0], n.scale[1], n.scale[2]));
}

struct Instance { uint32_t mesh; int node; Mat4 world; };

bool build(Model* m, const uint8_t* data, size_t size) {
    char err[128];
    if (!mof_open(data, size, &m->mof, err, sizeof err)) return false;
    const mof_file& f = m->mof;
    // Instances: nodes that carry a mesh (skinned ones are not moved by their node), or every mesh.
    Instance* inst = (Instance*)malloc(sizeof(Instance) * (f.node_count + f.mesh_count + 1));
    Mat4* world = (Mat4*)malloc(sizeof(Mat4) * (f.node_count + 1));
    if (!inst || !world) { free(inst); free(world); return false; }
    int ni = 0;
    if (f.node_count) {
        for (uint32_t i = 0; i < f.node_count; i++) {
            Mat4 local = node_local(f.nodes[i]);
            world[i] = f.nodes[i].parent >= 0 ? world[f.nodes[i].parent] * local : local;
            int mesh = f.nodes[i].mesh;
            if (mesh >= 0 && (uint32_t)mesh < f.mesh_count)
                inst[ni++] = Instance{ (uint32_t)mesh, (int)i, (f.nodes[i].flags & MOF_NODE_SKINNED) ? Mat4() : world[i] };
        }
    }
    if (ni == 0)
        for (uint32_t i = 0; i < f.mesh_count; i++) inst[ni++] = Instance{ i, -1, Mat4() };
    // Parts.
    int cap = 0;
    for (int k = 0; k < ni; k++) cap += f.meshes[inst[k].mesh].submesh_count ? (int)f.meshes[inst[k].mesh].submesh_count : 1;
    m->parts = (ModelPart*)calloc((size_t)(cap ? cap : 1), sizeof(ModelPart));
    uint64_t total = 0;
    for (int k = 0; k < ni && m->parts; k++) {
        const mof_mesh& mesh = f.meshes[inst[k].mesh];
        const float* pos = mof_mesh_positions(&f, &mesh);
        const float* nrm = mof_mesh_normals(&f, &mesh);
        const float* uv = mof_mesh_uv0(&f, &mesh);
        if (!pos) continue;
        uint32_t subs = mesh.submesh_count ? mesh.submesh_count : 1;
        for (uint32_t s = 0; s < subs; s++) {
            uint32_t first = 0, count = mesh.index_count, material = MOF_NONE;
            if (mesh.submesh_count) {
                const mof_submesh& sm = f.submeshes[mesh.first_submesh + s];
                first = sm.first_index; count = sm.index_count; material = sm.material;
            }
            count -= count % 3;
            if (!count) continue;
            ModelPart& p = m->parts[m->part_count++];
            p = ModelPart();
            p.vertex_count = count;
            p.mesh_index = inst[k].mesh;
            p.submesh_index = mesh.submesh_count ? mesh.first_submesh + s : 0;
            p.material = material;
            p.node = inst[k].node;
            p.positions = (Vec3*)malloc(sizeof(Vec3) * count);
            p.normals = (Vec3*)malloc(sizeof(Vec3) * count);
            p.uvs = uv ? (float*)malloc(sizeof(float) * 2 * count) : nullptr;
            if (!p.positions || !p.normals || (uv && !p.uvs)) return false;
            const Mat4& w = inst[k].world;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t v = mof_mesh_index(&f, &mesh, first + i);
                if (v >= mesh.vertex_count) v = 0;
                p.positions[i] = transform_point(w, Vec3(pos[v * 3], pos[v * 3 + 1], pos[v * 3 + 2]));
                if (nrm) p.normals[i] = normalize(transform_dir(w, Vec3(nrm[v * 3], nrm[v * 3 + 1], nrm[v * 3 + 2])));
                if (uv) { p.uvs[i * 2] = uv[v * 2]; p.uvs[i * 2 + 1] = uv[v * 2 + 1]; }
            }
            if (!nrm)
                for (uint32_t i = 0; i < count; i += 3) {
                    Vec3 n = normalize(cross(p.positions[i + 1] - p.positions[i], p.positions[i + 2] - p.positions[i]));
                    p.normals[i] = p.normals[i + 1] = p.normals[i + 2] = n;
                }
            if (material != MOF_NONE && material < f.material_count) {
                const mof_material& mat = f.materials[material];
                p.color = Vec3(to_srgb8(mat.base_color[0]) / 255.0f, to_srgb8(mat.base_color[1]) / 255.0f, to_srgb8(mat.base_color[2]) / 255.0f);
                uint32_t t = mat.tex[MOF_SLOT_BASE_COLOR].texture;
                if (t != MOF_NONE && t < f.texture_count && uv) snprintf(p.texture, sizeof p.texture, "mof:%s#%u", m->path, (unsigned)t);
                // Leaf cards: an alpha mask (blending is drawn as a mask too: there is no sorting).
                uint32_t am = (mat.flags & MOF_MATERIAL_ALPHA_MASK) >> MOF_MATERIAL_ALPHA_SHIFT;
                if (p.texture[0] && am == MOF_ALPHA_MASK) p.alpha_cut = mat.alpha_cutoff > 0.0f ? mat.alpha_cutoff : 0.5f;
                else if (p.texture[0] && am == MOF_ALPHA_BLEND) p.alpha_cut = 0.5f;
                p.double_sided = (mat.flags & MOF_MATERIAL_DOUBLE_SIDED) != 0;
            }
            total += count;
        }
    }
    free(inst);
    free(world);
    // The merged mesh.
    if (total == 0 || total > 0x7fffffffu) return false;
    m->mesh.positions = (Vec3*)malloc(sizeof(Vec3) * total);
    m->mesh.normals = (Vec3*)malloc(sizeof(Vec3) * total);
    if (!m->mesh.positions || !m->mesh.normals) return false;
    uint32_t at = 0;
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (int i = 0; i < m->part_count; i++) {
        const ModelPart& p = m->parts[i];
        memcpy(m->mesh.positions + at, p.positions, sizeof(Vec3) * p.vertex_count);
        memcpy(m->mesh.normals + at, p.normals, sizeof(Vec3) * p.vertex_count);
        for (uint32_t v = 0; v < p.vertex_count; v++) {
            const Vec3& q = p.positions[v];
            lo = vmin(lo, q);
            hi = vmax(hi, q);
        }
        at += p.vertex_count;
    }
    m->mesh.vertex_count = at;
    m->mesh.bounds_min = lo;
    m->mesh.bounds_max = hi;
    return true;
}

}  // namespace

void model_set_reader(ModelReader reader) { g_reader = reader; }

void model_cache_clear() {
    for (int i = 0; i < g_count; i++) free_model(g_models[i]);
    g_count = 0;
}

const Model* model_load(const char* path) {
    if (!path || !*path) return nullptr;
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_models[i]->path, path) == 0) return g_models[i]->part_count ? g_models[i] : nullptr;
    if (g_count == kMaxModels) model_cache_clear();
    Model* m = new Model();
    snprintf(m->path, sizeof m->path, "%s", path);
    size_t size = 0;
    void* bytes = (g_reader ? g_reader : default_read)(path, &size);
    bool ok = false;
    if (bytes) {
        // mof_open wants a 16 byte aligned buffer.
        m->block = (uint8_t*)malloc(size + 16);
        if (m->block) {
            uint8_t* aligned = (uint8_t*)(((uintptr_t)m->block + 15) & ~(uintptr_t)15);
            memcpy(aligned, bytes, size);
            ok = build(m, aligned, size);
        }
        free(bytes);
    }
    // A failed load is cached too (an empty entry), so a missing file is not retried every frame.
    if (!ok) {
        for (int i = 0; i < m->part_count; i++) { free(m->parts[i].positions); free(m->parts[i].normals); free(m->parts[i].uvs); }
        free(m->parts);
        m->parts = nullptr;
        m->part_count = 0;
        free(m->mesh.positions);
        free(m->mesh.normals);
        m->mesh = MeshData();
    }
    g_models[g_count++] = m;
    return ok ? m : nullptr;
}

void* model_asset_reader(const char* path, size_t* size, void* user) {
    const ModelAssetReader* r = (const ModelAssetReader*)user;
    if (strncmp(path, "mof:", 4) != 0) return r && r->inner ? r->inner(path, size, r->user) : nullptr;
    const char* hash = strrchr(path, '#');
    if (!hash) return nullptr;
    char model_path[256];
    size_t n = (size_t)(hash - (path + 4));
    if (n >= sizeof model_path) return nullptr;
    memcpy(model_path, path + 4, n);
    model_path[n] = 0;
    const Model* m = model_load(model_path);
    uint32_t t = (uint32_t)atoi(hash + 1);
    if (!m || t >= m->mof.texture_count) return nullptr;
    const mof_texture& tex = m->mof.textures[t];
    uint64_t bytes = 0;
    const uint8_t* data = mof_texture_bytes(&m->mof, t, &bytes);
    if (!data) return nullptr;
    if (tex.codec == MOF_TEX_PNG || tex.codec == MOF_TEX_JPEG) {
        uint8_t* out = (uint8_t*)malloc((size_t)bytes ? (size_t)bytes : 1);
        if (!out) return nullptr;
        memcpy(out, data, (size_t)bytes);
        if (size) *size = (size_t)bytes;
        return out;
    }
    // Raw RGBA8 (the first mip): an uncompressed 32-bit TGA, which the image decoder reads.
    if (tex.codec == MOF_TEX_RAW_RGBA8 && tex.width && tex.height && bytes >= (uint64_t)tex.width * tex.height * 4 && tex.width < 65536 && tex.height < 65536) {
        size_t px = (size_t)tex.width * tex.height;
        uint8_t* out = (uint8_t*)malloc(18 + px * 4);
        if (!out) return nullptr;
        memset(out, 0, 18);
        out[2] = 2;
        out[12] = (uint8_t)tex.width; out[13] = (uint8_t)(tex.width >> 8);
        out[14] = (uint8_t)tex.height; out[15] = (uint8_t)(tex.height >> 8);
        out[16] = 32;
        out[17] = 0x28;                 // 8 alpha bits, top-left origin
        for (size_t i = 0; i < px; i++) {
            out[18 + i * 4] = data[i * 4 + 2]; out[18 + i * 4 + 1] = data[i * 4 + 1];
            out[18 + i * 4 + 2] = data[i * 4]; out[18 + i * 4 + 3] = data[i * 4 + 3];
        }
        if (size) *size = 18 + px * 4;
        return out;
    }
    return nullptr;
}

}  // namespace fe
