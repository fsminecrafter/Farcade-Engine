// glTF 2.0 (.gltf / .glb) -> mof_model, via cgltf. See docs/mof-format.md section "Importers".
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cgltf/cgltf.h"
#include "mof/import_internal.h"

namespace {

struct Imp {
    mof_model* m;
    cgltf_data* g = nullptr;
    cgltf_options opt;
    const char* path;
    char* err;
    size_t errlen;
    char dir[1024];

    // per-image cache (so textures sharing an image share its bytes)
    struct Img { bool tried = false, ok = false; mof_wtexture t; };
    Img* imgs = nullptr;
    int32_t* tex_map = nullptr;        // glTF texture index -> mof texture index or -1 (dropped)
    int32_t* bone_of_node = nullptr;   // node index -> bone index or -1
    uint32_t* node_out = nullptr;      // glTF node index -> mof node index
    int32_t* mesh_skin = nullptr;      // glTF mesh index -> skin index or -1

    bool fail(const char* msg) { snprintf(err, errlen, "%s", msg); return false; }
    bool oom() { return fail("out of memory"); }
    template <class T> T* make(size_t n) { return (T*)mof_model_alloc(m, sizeof(T) * (n ? n : 1)); }
};

#define TRY(x) do { if (!(x)) return false; } while (0)

uint32_t wrap_of(int v) { return v == 33071 ? MOF_WRAP_CLAMP : v == 33648 ? MOF_WRAP_MIRROR : MOF_WRAP_REPEAT; }

void trs_of(const cgltf_node* n, float t[3], float r[4], float s[3]) {
    if (n->has_matrix) { mof_mat4_decompose(n->matrix, t, r, s); return; }
    t[0] = t[1] = t[2] = 0; r[0] = r[1] = r[2] = 0; r[3] = 1; s[0] = s[1] = s[2] = 1;
    if (n->has_translation) memcpy(t, n->translation, 12);
    if (n->has_rotation) memcpy(r, n->rotation, 16);
    if (n->has_scale) memcpy(s, n->scale, 12);
}

// Loads (once) the bytes of an image: buffer view, data: URI, or external file.
bool load_image(Imp& I, size_t ii, bool* ok) {
    Imp::Img& im = I.imgs[ii];
    if (im.tried) { *ok = im.ok; return true; }
    im.tried = true;
    *ok = false;
    cgltf_image* img = &I.g->images[ii];
    const uint8_t* bytes = nullptr;
    size_t n = 0;
    char e[256] = {0};
    if (img->buffer_view) {
        const cgltf_buffer_view* bv = img->buffer_view;
        if (!bv->buffer || !bv->buffer->data || bv->offset + bv->size > bv->buffer->size) {
            mof_model_warn(I.m, "image %zu: bad buffer view, texture dropped", ii);
            return true;
        }
        // copy: the buffer belongs to cgltf and is freed before mof_write runs
        uint8_t* own = I.make<uint8_t>(bv->size);
        if (!own) return I.oom();
        memcpy(own, (const uint8_t*)bv->buffer->data + bv->offset, bv->size);
        bytes = own;
        n = bv->size;
    } else if (img->uri && strncmp(img->uri, "data:", 5) == 0) {
        const char* comma = strchr(img->uri, ',');
        if (!comma || comma - img->uri < 7 || strncmp(comma - 7, ";base64", 7) != 0) {
            mof_model_warn(I.m, "image %zu: unsupported data URI, texture dropped", ii);
            return true;
        }
        const char* b64 = comma + 1;
        size_t len = strlen(b64);
        while (len && b64[len - 1] == '=') len--;
        size_t dec = len / 4 * 3 + (len % 4 == 3 ? 2 : len % 4 == 2 ? 1 : 0);
        void* data = nullptr;
        if (cgltf_load_buffer_base64(&I.opt, dec, b64, &data) != cgltf_result_success) {
            mof_model_warn(I.m, "image %zu: bad base64, texture dropped", ii);
            return true;
        }
        uint8_t* own = I.make<uint8_t>(dec);
        if (!own) { free(data); return I.oom(); }
        memcpy(own, data, dec);
        free(data);
        bytes = own; n = dec;
    } else if (img->uri && !strstr(img->uri, "://")) {
        char rel[1024];
        snprintf(rel, sizeof rel, "%s", img->uri);
        cgltf_decode_uri(rel);  // percent-decoding
        char full[2200];
        snprintf(full, sizeof full, "%s%s", I.dir, rel);
        uint8_t* d = nullptr;
        if (!mof_read_file(I.m, full, &d, &n)) {
            mof_model_warn(I.m, "image %zu: cannot read '%s', texture dropped", ii, full);
            return true;
        }
        bytes = d;
    } else {
        mof_model_warn(I.m, "image %zu: no usable source, texture dropped", ii);
        return true;
    }
    mof_wtexture t;
    mof_wtexture_default(&t);
    if (!mof_make_texture_data(I.m, bytes, n, &t, e, sizeof e)) {
        mof_model_warn(I.m, "image %zu: %s, texture dropped", ii, e);
        return true;
    }
    im.t = t;
    im.ok = true;
    *ok = true;
    return true;
}

bool import_textures(Imp& I) {
    cgltf_data* g = I.g;
    I.imgs = I.make<Imp::Img>(g->images_count);
    I.tex_map = I.make<int32_t>(g->textures_count);
    mof_wtexture* out = I.make<mof_wtexture>(g->textures_count);
    if (!I.imgs || !I.tex_map || !out) return I.oom();
    uint32_t count = 0;  // textures whose image cannot be used are dropped, the rest keep their order
    for (size_t gi = 0; gi < g->textures_count; gi++) {
        const cgltf_texture& t = g->textures[gi];
        const uint32_t i = count;
        I.tex_map[gi] = -1;
        mof_wtexture_default(&out[i]);
        if (!t.image) { mof_model_warn(I.m, "texture %zu has no PNG/JPEG image source (webp/basisu not supported), dropped", gi); continue; }
        bool ok = false;
        TRY(load_image(I, cgltf_image_index(g, t.image), &ok));
        if (!ok) continue;
        const mof_wtexture& src = I.imgs[cgltf_image_index(g, t.image)].t;
        out[i] = src;
        out[i].name = t.name ? mof_model_strdup(I.m, t.name) : (t.image->name ? mof_model_strdup(I.m, t.image->name) : nullptr);
        if (t.sampler) {
            out[i].wrap_s = wrap_of(t.sampler->wrap_s);
            out[i].wrap_t = wrap_of(t.sampler->wrap_t);
            out[i].mag_filter = t.sampler->mag_filter == cgltf_filter_type_nearest ? MOF_FILTER_NEAREST : MOF_FILTER_LINEAR;
            switch (t.sampler->min_filter) {
                case cgltf_filter_type_nearest: out[i].min_filter = MOF_FILTER_NEAREST; out[i].mip_filter = MOF_MIP_NONE; break;
                case cgltf_filter_type_nearest_mipmap_nearest: out[i].min_filter = MOF_FILTER_NEAREST; out[i].mip_filter = MOF_MIP_NEAREST; break;
                case cgltf_filter_type_nearest_mipmap_linear: out[i].min_filter = MOF_FILTER_NEAREST; out[i].mip_filter = MOF_MIP_LINEAR; break;
                case cgltf_filter_type_linear_mipmap_nearest: out[i].min_filter = MOF_FILTER_LINEAR; out[i].mip_filter = MOF_MIP_NEAREST; break;
                case cgltf_filter_type_linear_mipmap_linear: out[i].min_filter = MOF_FILTER_LINEAR; out[i].mip_filter = MOF_MIP_LINEAR; break;
                default: out[i].min_filter = MOF_FILTER_LINEAR; out[i].mip_filter = MOF_MIP_NONE; break;
            }
        }
        I.tex_map[gi] = (int32_t)count++;
    }
    I.m->desc.texture_count = count;
    I.m->desc.textures = out;
    return true;
}

void set_slot(Imp& I, mof_wmaterial* w, uint32_t slot, const cgltf_texture_view& v, size_t mat) {
    if (!v.texture) return;
    size_t ti = cgltf_texture_index(I.g, v.texture);
    if (I.tex_map[ti] < 0) return;
    w->tex[slot].texture = I.tex_map[ti];
    int uv = v.texcoord;
    if (uv < 0 || uv > 1) { mof_model_warn(I.m, "material %zu slot %u uses TEXCOORD_%d, only 0 and 1 are supported; using 0", mat, slot, uv); uv = 0; }
    w->tex[slot].uv_set = (uint32_t)uv;
}

bool import_materials(Imp& I) {
    cgltf_data* g = I.g;
    mof_wmaterial* out = I.make<mof_wmaterial>(g->materials_count);
    if (!out) return I.oom();
    for (size_t i = 0; i < g->materials_count; i++) {
        const cgltf_material& s = g->materials[i];
        mof_wmaterial& w = out[i];
        mof_wmaterial_default(&w);
        w.name = s.name ? mof_model_strdup(I.m, s.name) : nullptr;
        if (s.has_pbr_metallic_roughness) {
            const cgltf_pbr_metallic_roughness& p = s.pbr_metallic_roughness;
            memcpy(w.base_color, p.base_color_factor, 16);
            w.metallic = p.metallic_factor;
            w.roughness = p.roughness_factor;
            set_slot(I, &w, MOF_SLOT_BASE_COLOR, p.base_color_texture, i);
            set_slot(I, &w, MOF_SLOT_METAL_ROUGH, p.metallic_roughness_texture, i);
        } else if (s.has_pbr_specular_glossiness) {
            const cgltf_pbr_specular_glossiness& p = s.pbr_specular_glossiness;
            memcpy(w.base_color, p.diffuse_factor, 16);
            w.metallic = 0.0f;
            w.roughness = 1.0f - p.glossiness_factor;
            set_slot(I, &w, MOF_SLOT_BASE_COLOR, p.diffuse_texture, i);
            mof_model_warn(I.m, "material %zu uses KHR_materials_pbrSpecularGlossiness: approximated as metallic-roughness", i);
        }
        float es = s.has_emissive_strength ? s.emissive_strength.emissive_strength : 1.0f;
        for (int k = 0; k < 3; k++) w.emissive[k] = s.emissive_factor[k] * es;
        set_slot(I, &w, MOF_SLOT_NORMAL, s.normal_texture, i);
        set_slot(I, &w, MOF_SLOT_OCCLUSION, s.occlusion_texture, i);
        set_slot(I, &w, MOF_SLOT_EMISSIVE, s.emissive_texture, i);
        if (s.normal_texture.texture) w.normal_scale = s.normal_texture.scale;
        if (s.occlusion_texture.texture) w.occlusion_strength = s.occlusion_texture.scale;
        w.alpha_mode = s.alpha_mode == cgltf_alpha_mode_mask ? MOF_ALPHA_MASK : s.alpha_mode == cgltf_alpha_mode_blend ? MOF_ALPHA_BLEND : MOF_ALPHA_OPAQUE;
        w.alpha_cutoff = s.alpha_cutoff;
        w.double_sided = s.double_sided ? 1 : 0;
    }
    I.m->desc.material_count = (uint32_t)g->materials_count;
    I.m->desc.materials = out;
    return true;
}

// node order: parents before children (stable by depth, then index)
bool order_nodes(Imp& I) {
    cgltf_data* g = I.g;
    size_t n = g->nodes_count;
    uint32_t* depth = I.make<uint32_t>(n);
    I.node_out = I.make<uint32_t>(n);
    if (!depth || !I.node_out) return I.oom();
    uint32_t maxd = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t d = 0;
        for (const cgltf_node* p = g->nodes[i].parent; p; p = p->parent) {
            if (++d > n) return I.fail("glTF node hierarchy has a cycle");
        }
        depth[i] = d;
        if (d > maxd) maxd = d;
    }
    uint32_t next = 0;
    for (uint32_t d = 0; d <= maxd; d++)
        for (size_t i = 0; i < n; i++)
            if (depth[i] == d) I.node_out[i] = next++;
    return true;
}

bool import_skeleton(Imp& I) {
    cgltf_data* g = I.g;
    size_t n = g->nodes_count;
    I.bone_of_node = I.make<int32_t>(n);
    uint8_t* is_joint = I.make<uint8_t>(n);
    if (!I.bone_of_node || !is_joint) return I.oom();
    for (size_t i = 0; i < n; i++) I.bone_of_node[i] = -1;
    uint32_t nb = 0;
    for (size_t s = 0; s < g->skins_count; s++)
        for (size_t j = 0; j < g->skins[s].joints_count; j++) {
            size_t ni = cgltf_node_index(g, g->skins[s].joints[j]);
            if (!is_joint[ni]) { is_joint[ni] = 1; nb++; }
        }
    if (nb == 0) return true;
    if (nb > MOF_MAX_BONES) return I.fail("too many joints");
    // bone order: by node order (parents before children)
    uint32_t* by_order = I.make<uint32_t>(n);  // mof node index -> glTF node
    if (!by_order) return I.oom();
    for (size_t i = 0; i < n; i++) by_order[I.node_out[i]] = (uint32_t)i;
    uint32_t b = 0;
    uint32_t* node_of_bone = I.make<uint32_t>(nb);
    if (!node_of_bone) return I.oom();
    for (size_t k = 0; k < n; k++) {
        uint32_t ni = by_order[k];
        if (is_joint[ni]) { I.bone_of_node[ni] = (int32_t)b; node_of_bone[b] = ni; b++; }
    }
    mof_wbone* bones = I.make<mof_wbone>(nb);
    bool* ibm_set = I.make<bool>(nb);
    if (!bones || !ibm_set) return I.oom();
    for (uint32_t k = 0; k < nb; k++) {
        mof_wbone_default(&bones[k]);
        const cgltf_node* node = &g->nodes[node_of_bone[k]];
        bones[k].name = node->name ? mof_model_strdup(I.m, node->name) : nullptr;
        bones[k].node = (int32_t)I.node_out[node_of_bone[k]];
        const cgltf_node* pj = node->parent;
        while (pj && I.bone_of_node[cgltf_node_index(g, pj)] < 0) pj = pj->parent;
        bones[k].parent = pj ? I.bone_of_node[cgltf_node_index(g, pj)] : -1;
        if (node->parent == pj) {
            trs_of(node, bones[k].translation, bones[k].rotation, bones[k].scale);
        } else {  // non-joint nodes in between: express relative to the parent bone via world matrices
            float w[16], pw[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, inv[16], loc[16];
            cgltf_node_transform_world(node, w);
            if (pj) cgltf_node_transform_world(pj, pw);
            if (mof_mat4_inverse_affine(pw, inv)) { mof_mat4_mul(inv, w, loc); mof_mat4_decompose(loc, bones[k].translation, bones[k].rotation, bones[k].scale); }
            else trs_of(node, bones[k].translation, bones[k].rotation, bones[k].scale);
        }
    }
    // inverse bind matrices: first skin that contains the joint wins
    for (size_t s = 0; s < g->skins_count; s++) {
        const cgltf_skin& sk = g->skins[s];
        for (size_t j = 0; j < sk.joints_count; j++) {
            int32_t bi = I.bone_of_node[cgltf_node_index(g, sk.joints[j])];
            if (ibm_set[bi]) continue;
            ibm_set[bi] = true;
            if (sk.inverse_bind_matrices && j < sk.inverse_bind_matrices->count)
                cgltf_accessor_read_float(sk.inverse_bind_matrices, j, bones[bi].inverse_bind, 16);
        }
    }
    I.m->desc.bone_count = nb;
    I.m->desc.bones = bones;
    return true;
}

// ------------------------------------------------------------------ meshes

const cgltf_accessor* find_attr(const cgltf_primitive& p, cgltf_attribute_type t, int index) {
    for (size_t i = 0; i < p.attributes_count; i++)
        if (p.attributes[i].type == t && p.attributes[i].index == index) return p.attributes[i].data;
    return nullptr;
}

// Triangle list for a primitive (indices local to the primitive); count 0 if unsupported.
bool prim_indices(Imp& I, const cgltf_primitive& p, uint32_t nv, uint32_t** out, uint32_t* count) {
    *out = nullptr; *count = 0;
    size_t src = p.indices ? p.indices->count : nv;
    uint32_t* raw = I.make<uint32_t>(src);
    if (!raw) return I.oom();
    for (size_t k = 0; k < src; k++) {
        size_t v = p.indices ? cgltf_accessor_read_index(p.indices, k) : k;
        if (v >= nv) return I.fail("glTF primitive index out of range");
        raw[k] = (uint32_t)v;
    }
    if (p.type == cgltf_primitive_type_triangles) {
        *out = raw; *count = (uint32_t)(src / 3 * 3);
    } else if (p.type == cgltf_primitive_type_triangle_strip || p.type == cgltf_primitive_type_triangle_fan) {
        if (src < 3) return true;
        uint32_t nt = (uint32_t)(src - 2);
        if ((size_t)nt * 3 > 0x7FFFFFFF) return I.fail("primitive too large");
        uint32_t* t = I.make<uint32_t>((size_t)nt * 3);
        if (!t) return I.oom();
        for (uint32_t k = 0; k < nt; k++) {
            if (p.type == cgltf_primitive_type_triangle_strip) {
                bool odd = k & 1;
                t[k * 3] = raw[k]; t[k * 3 + 1] = raw[k + 1 + (odd ? 1 : 0)]; t[k * 3 + 2] = raw[k + 1 + (odd ? 0 : 1)];
            } else {
                t[k * 3] = raw[0]; t[k * 3 + 1] = raw[k + 1]; t[k * 3 + 2] = raw[k + 2];
            }
        }
        *out = t; *count = nt * 3;
    } else {
        mof_model_warn(I.m, "non-triangle primitive skipped");
    }
    return true;
}

bool import_meshes(Imp& I) {
    cgltf_data* g = I.g;
    I.mesh_skin = I.make<int32_t>(g->meshes_count);
    if (!I.mesh_skin) return I.oom();
    for (size_t i = 0; i < g->meshes_count; i++) I.mesh_skin[i] = -1;
    for (size_t i = 0; i < g->nodes_count; i++) {
        const cgltf_node& n = g->nodes[i];
        if (!n.mesh || !n.skin) continue;
        size_t mi = cgltf_mesh_index(g, n.mesh), si = cgltf_skin_index(g, n.skin);
        if (I.mesh_skin[mi] < 0) I.mesh_skin[mi] = (int32_t)si;
        else if (I.mesh_skin[mi] != (int32_t)si) mof_model_warn(I.m, "mesh %zu is used with several skins; the first one is used", mi);
    }

    mof_wmesh* out = I.make<mof_wmesh>(g->meshes_count);
    if (!out) return I.oom();
    for (size_t mi = 0; mi < g->meshes_count; mi++) {
        const cgltf_mesh& sm = g->meshes[mi];
        mof_wmesh& w = out[mi];
        mof_wmesh_default(&w);
        w.name = sm.name ? mof_model_strdup(I.m, sm.name) : nullptr;

        // pass 1: usable primitives, totals, stream union
        struct Prim { const cgltf_primitive* p; uint32_t nv, base, ni, *idx; };
        Prim* prims = I.make<Prim>(sm.primitives_count);
        if (!prims) return I.oom();
        size_t np = 0;
        uint64_t total_v = 0, total_i = 0;
        bool any_uv0 = false, any_uv1 = false, any_col = false, any_tan = false, any_skin = false;
        for (size_t pi = 0; pi < sm.primitives_count; pi++) {
            const cgltf_primitive& p = sm.primitives[pi];
            if (p.has_draco_mesh_compression) { mof_model_warn(I.m, "mesh %zu: Draco compressed primitive skipped", mi); continue; }
            const cgltf_accessor* pos = find_attr(p, cgltf_attribute_type_position, 0);
            if (!pos || pos->type != cgltf_type_vec3 || pos->count == 0) { mof_model_warn(I.m, "mesh %zu: primitive without POSITION skipped", mi); continue; }
            if (pos->count > 0x7FFFFFFF) return I.fail("primitive too large");
            Prim& pr = prims[np];
            pr.p = &p; pr.nv = (uint32_t)pos->count; pr.base = (uint32_t)total_v;
            TRY(prim_indices(I, p, pr.nv, &pr.idx, &pr.ni));
            if (pr.ni == 0) continue;
            total_v += pr.nv; total_i += pr.ni;
            if (total_v > 0x7FFFFFFF || total_i > 0x7FFFFFFF) return I.fail("mesh too large");
            any_uv0 |= find_attr(p, cgltf_attribute_type_texcoord, 0) != nullptr;
            any_uv1 |= find_attr(p, cgltf_attribute_type_texcoord, 1) != nullptr;
            any_col |= find_attr(p, cgltf_attribute_type_color, 0) != nullptr;
            any_tan |= p.material && p.material->normal_texture.texture;
            any_skin |= find_attr(p, cgltf_attribute_type_joints, 0) && find_attr(p, cgltf_attribute_type_weights, 0);
            np++;
        }
        if (any_skin && (I.mesh_skin[mi] < 0 || g->skins[I.mesh_skin[mi]].joints_count == 0)) {
            mof_model_warn(I.m, "mesh %zu has joints but no node with a skin uses it; skin data dropped", mi);
            any_skin = false;
        }
        if (np == 0) continue;  // empty mesh keeps its index

        uint32_t nv = (uint32_t)total_v, ni = (uint32_t)total_i;
        float* P = I.make<float>((size_t)nv * 3);
        float* N = I.make<float>((size_t)nv * 3);
        float* T = any_tan ? I.make<float>((size_t)nv * 4) : nullptr;
        float* U0 = any_uv0 ? I.make<float>((size_t)nv * 2) : nullptr;
        float* U1 = any_uv1 ? I.make<float>((size_t)nv * 2) : nullptr;
        uint8_t* C = any_col ? I.make<uint8_t>((size_t)nv * 4) : nullptr;
        uint16_t* J = any_skin ? I.make<uint16_t>((size_t)nv * 4) : nullptr;
        float* W = any_skin ? I.make<float>((size_t)nv * 4) : nullptr;
        uint32_t* IX = I.make<uint32_t>(ni);
        mof_wsubmesh* subs = I.make<mof_wsubmesh>(np);
        if (!P || !N || !IX || !subs || (any_tan && !T) || (any_uv0 && !U0) || (any_uv1 && !U1) || (any_col && !C) || (any_skin && (!J || !W))) return I.oom();
        if (C) memset(C, 255, (size_t)nv * 4);

        uint32_t icur = 0;
        for (size_t k = 0; k < np; k++) {
            const Prim& pr = prims[k];
            const cgltf_primitive& p = *pr.p;
            const cgltf_accessor* a;
            cgltf_accessor_unpack_floats(find_attr(p, cgltf_attribute_type_position, 0), P + 3 * (size_t)pr.base, (size_t)pr.nv * 3);
            // indices (offset by base) and submesh
            for (uint32_t q = 0; q < pr.ni; q++) IX[icur + q] = pr.idx[q] + pr.base;
            subs[k].first_index = icur; subs[k].index_count = pr.ni;
            subs[k].material = p.material ? (int32_t)cgltf_material_index(g, p.material) : -1;
            // normals
            if ((a = find_attr(p, cgltf_attribute_type_normal, 0)) && a->type == cgltf_type_vec3 && a->count >= pr.nv) {
                cgltf_accessor_unpack_floats(a, N + 3 * (size_t)pr.base, (size_t)pr.nv * 3);
            } else {
                mof_generate_normals(P + 3 * (size_t)pr.base, pr.nv, pr.idx, pr.ni, N + 3 * (size_t)pr.base);
            }
            // uv sets
            if (U0 && (a = find_attr(p, cgltf_attribute_type_texcoord, 0)) && a->type == cgltf_type_vec2 && a->count >= pr.nv)
                cgltf_accessor_unpack_floats(a, U0 + 2 * (size_t)pr.base, (size_t)pr.nv * 2);
            if (U1 && (a = find_attr(p, cgltf_attribute_type_texcoord, 1)) && a->type == cgltf_type_vec2 && a->count >= pr.nv)
                cgltf_accessor_unpack_floats(a, U1 + 2 * (size_t)pr.base, (size_t)pr.nv * 2);
            // colors
            if (C && (a = find_attr(p, cgltf_attribute_type_color, 0)) && (a->type == cgltf_type_vec3 || a->type == cgltf_type_vec4) && a->count >= pr.nv) {
                size_t nc = cgltf_num_components(a->type);
                for (uint32_t v = 0; v < pr.nv; v++) {
                    float c[4] = {1, 1, 1, 1};
                    cgltf_accessor_read_float(a, v, c, nc);
                    for (int q = 0; q < 4; q++) {
                        float x = c[q] < 0 ? 0 : c[q] > 1 ? 1 : c[q];
                        C[4 * (size_t)(pr.base + v) + q] = (uint8_t)(x * 255.0f + 0.5f);
                    }
                }
            }
            // tangents: only if this primitive's material has a normal map (any_tan makes the stream exist for all)
            if (T) {
                float* Tp = T + 4 * (size_t)pr.base;
                const cgltf_accessor* ta = find_attr(p, cgltf_attribute_type_tangent, 0);
                if (ta && ta->type == cgltf_type_vec4 && ta->count >= pr.nv) {
                    cgltf_accessor_unpack_floats(ta, Tp, (size_t)pr.nv * 4);
                } else {
                    int set = p.material && p.material->normal_texture.texture ? p.material->normal_texture.texcoord : 0;
                    float* U = set == 1 ? U1 : U0;
                    if (U) {
                        mof_generate_tangents(P + 3 * (size_t)pr.base, N + 3 * (size_t)pr.base, U + 2 * (size_t)pr.base, pr.nv, pr.idx, pr.ni, Tp);
                    } else {
                        float* zu = I.make<float>((size_t)pr.nv * 2);
                        if (!zu) return I.oom();
                        mof_generate_tangents(P + 3 * (size_t)pr.base, N + 3 * (size_t)pr.base, zu, pr.nv, pr.idx, pr.ni, Tp);
                        mof_model_warn(I.m, "mesh %zu: normal map without UVs; tangents are arbitrary", mi);
                    }
                }
            }
            // skin
            if (J) {
                const cgltf_accessor* ja = find_attr(p, cgltf_attribute_type_joints, 0);
                const cgltf_accessor* wa = find_attr(p, cgltf_attribute_type_weights, 0);
                const cgltf_skin& sk = g->skins[I.mesh_skin[mi]];
                if (ja && wa && ja->count >= pr.nv && wa->count >= pr.nv) {
                    for (uint32_t v = 0; v < pr.nv; v++) {
                        cgltf_uint j4[4] = {0, 0, 0, 0};
                        float w4[4] = {0, 0, 0, 0};
                        cgltf_accessor_read_uint(ja, v, j4, 4);
                        cgltf_accessor_read_float(wa, v, w4, 4);
                        size_t o = 4 * (size_t)(pr.base + v);
                        for (int q = 0; q < 4; q++) {
                            // remap skin-local joint index to the model's bone index
                            if (j4[q] < sk.joints_count) {
                                J[o + q] = (uint16_t)I.bone_of_node[cgltf_node_index(g, sk.joints[j4[q]])];
                                W[o + q] = w4[q];
                            } else { J[o + q] = 0; W[o + q] = 0; }
                        }
                    }
                } else {  // primitive without skin data: rigidly bound to the first joint
                    for (uint32_t v = 0; v < pr.nv; v++) {
                        size_t o = 4 * (size_t)(pr.base + v);
                        J[o] = (uint16_t)I.bone_of_node[cgltf_node_index(g, sk.joints[0])];
                        W[o] = 1.0f;
                    }
                }
            }
            icur += pr.ni;
        }
        if (find_attr(*prims[0].p, cgltf_attribute_type_joints, 1)) mof_model_warn(I.m, "mesh %zu: more than 4 joint influences per vertex, extra ones ignored", mi);

        w.vertex_count = nv; w.positions = P; w.normals = N; w.tangents = T; w.uv0 = U0; w.uv1 = U1;
        w.colors = C; w.joints = J; w.weights_f32 = W;
        w.index_count = ni; w.indices = IX; w.submeshes = subs; w.submesh_count = (uint32_t)np;
    }
    I.m->desc.mesh_count = (uint32_t)g->meshes_count;
    I.m->desc.meshes = out;
    return true;
}

bool import_nodes(Imp& I) {
    cgltf_data* g = I.g;
    size_t n = g->nodes_count;
    mof_wnode* out = I.make<mof_wnode>(n);
    if (!out) return I.oom();
    for (size_t i = 0; i < n; i++) {
        const cgltf_node& s = g->nodes[i];
        mof_wnode& w = out[I.node_out[i]];
        mof_wnode_default(&w);
        w.name = s.name ? mof_model_strdup(I.m, s.name) : nullptr;
        w.parent = s.parent ? (int32_t)I.node_out[cgltf_node_index(g, s.parent)] : -1;
        w.mesh = s.mesh ? (int32_t)cgltf_mesh_index(g, s.mesh) : -1;
        w.skinned = (s.skin && I.m->desc.bone_count && s.mesh &&
                     I.m->desc.meshes[cgltf_mesh_index(g, s.mesh)].joints) ? 1 : 0;  // only if skin data survived
        trs_of(&s, w.translation, w.rotation, w.scale);
    }
    I.m->desc.node_count = (uint32_t)n;
    I.m->desc.nodes = out;
    return true;
}

}  // namespace

bool mof_import_gltf(const char* path, mof_model* out, char* err, size_t errlen) {
    mof_model_init(out);
    if (err && errlen) err[0] = 0;
    Imp I;
    I.m = out; I.path = path; I.err = err; I.errlen = errlen;
    memset(&I.opt, 0, sizeof I.opt);
    const char* slash = strrchr(path, '/');
    const char* bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    size_t dl = slash ? (size_t)(slash - path + 1) : 0;
    if (dl >= sizeof I.dir) { snprintf(err, errlen, "path too long"); return false; }
    memcpy(I.dir, path, dl);
    I.dir[dl] = 0;

    bool ok = false;
    cgltf_result r = cgltf_parse_file(&I.opt, path, &I.g);
    if (r != cgltf_result_success) {
        snprintf(err, errlen, "cannot parse glTF '%s' (cgltf error %d)", path, (int)r);
    } else if ((r = cgltf_load_buffers(&I.opt, I.g, path)) != cgltf_result_success) {
        snprintf(err, errlen, "cannot load glTF buffers (cgltf error %d)", (int)r);
    } else if ((r = cgltf_validate(I.g)) != cgltf_result_success) {
        snprintf(err, errlen, "invalid glTF (cgltf validation error %d)", (int)r);
    } else {
        ok = import_textures(I) && import_materials(I) && order_nodes(I) && import_skeleton(I) && import_meshes(I) && import_nodes(I);
    }
    if (I.g) cgltf_free(I.g);
    if (!ok) mof_model_free(out);
    return ok;
}
