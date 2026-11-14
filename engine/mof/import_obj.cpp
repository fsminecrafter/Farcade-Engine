// Wavefront OBJ (+ MTL) -> mof_model, via fast_obj. See docs/mof-format.md section "Importers".
//
// One mof mesh with one submesh per material, one root node. Vertices are
// unique (position, texcoord, normal) triples. OBJ UV origin is bottom-left,
// .mof's is top-left, so v is flipped (v' = 1 - v). Polygons are fan
// triangulated. MTL -> PBR: Kd = base color (d = alpha), Ke = emissive,
// roughness = 1 - sqrt(Ns / 1000) (the convention of Blender's OBJ importer),
// metallic = 0; map_Kd / map_Ke / map_bump -> base color / emissive / normal.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fast_obj/fast_obj.h"
#include "mof/import_internal.h"

namespace {

struct Key { uint32_t p, t, n; };

uint32_t hash_key(const Key& k) {
    uint32_t h = k.p * 0x9E3779B1u;
    h ^= (k.t + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= (k.n + 0x165667B1u) * 0xC2B2AE3Du;
    h ^= h >> 15;
    return h;
}

const char* base_name(const char* path, char* buf, size_t n) {
    const char* s = strrchr(path, '/');
    const char* b = strrchr(path, '\\');
    if (b && (!s || b > s)) s = b;
    s = s ? s + 1 : path;
    snprintf(buf, n, "%s", s);
    char* dot = strrchr(buf, '.');
    if (dot) *dot = 0;
    return buf;
}

float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

}  // namespace

bool mof_import_obj(const char* path, mof_model* out, char* err, size_t errlen) {
    mof_model_init(out);
    if (err && errlen) err[0] = 0;
    fastObjMesh* o = fast_obj_read(path);
    if (!o) { snprintf(err, errlen, "cannot read OBJ '%s'", path); return false; }

    bool ok = false;
    do {
#define NEED(p) do { if (!(p)) { snprintf(err, errlen, "out of memory"); goto done; } } while (0)
        // --- unique vertices and triangles per face
        uint64_t max_tris = 0;
        for (uint32_t f = 0; f < o->face_count; f++)
            if (o->face_vertices[f] >= 3 && !(o->face_lines && o->face_lines[f])) max_tris += o->face_vertices[f] - 2;
        if (max_tris == 0) { snprintf(err, errlen, "OBJ contains no faces"); goto done; }
        if (max_tris * 3 > 0x7FFFFFFFull) { snprintf(err, errlen, "OBJ too large"); goto done; }
        uint32_t cap_v = (uint32_t)(o->index_count ? o->index_count : 1);
        uint32_t hsize = 16;
        while (hsize < cap_v * 2u) hsize <<= 1;
        uint32_t* table = (uint32_t*)mof_model_alloc(out, sizeof(uint32_t) * hsize);
        Key* keys = (Key*)mof_model_alloc(out, sizeof(Key) * cap_v);
        uint32_t* tri = (uint32_t*)mof_model_alloc(out, sizeof(uint32_t) * (size_t)max_tris * 3);
        uint32_t* tri_mat = (uint32_t*)mof_model_alloc(out, sizeof(uint32_t) * (size_t)max_tris);
        NEED(table && keys && tri && tri_mat);
        memset(table, 0xFF, sizeof(uint32_t) * hsize);
        uint32_t nv = 0, nt = 0;
        bool any_n = false, any_t = false, any_c = o->color_count == o->position_count && o->color_count > 1;
        bool missing_n = false;
        uint32_t ioff = 0;
        for (uint32_t f = 0; f < o->face_count; f++) {
            uint32_t fv = o->face_vertices[f];
            bool skip = fv < 3 || (o->face_lines && o->face_lines[f]);
            if (!skip) {
                uint32_t vi[3], first = 0, prev = 0;
                for (uint32_t k = 0; k < fv; k++) {
                    const fastObjIndex& ix = o->indices[ioff + k];
                    Key key{ix.p, ix.t, ix.n};
                    if (ix.p == 0 || ix.p >= o->position_count) { snprintf(err, errlen, "OBJ face references a missing position"); goto done; }
                    uint32_t h = hash_key(key) & (hsize - 1), v;
                    for (;;) {
                        if (table[h] == 0xFFFFFFFFu) { v = nv++; keys[v] = key; table[h] = v; break; }
                        const Key& kk = keys[table[h]];
                        if (kk.p == key.p && kk.t == key.t && kk.n == key.n) { v = table[h]; break; }
                        h = (h + 1) & (hsize - 1);
                    }
                    if (ix.t >= o->texcoord_count) { snprintf(err, errlen, "OBJ texcoord index out of range"); goto done; }
                    if (ix.n >= o->normal_count) { snprintf(err, errlen, "OBJ normal index out of range"); goto done; }
                    if (k == 0) first = v;
                    if (k >= 2) {
                        vi[0] = first; vi[1] = prev; vi[2] = v;
                        memcpy(tri + (size_t)nt * 3, vi, 12);
                        tri_mat[nt] = o->face_materials ? o->face_materials[f] : 0;
                        nt++;
                    }
                    prev = v;
                }
            }
            ioff += fv;
        }
        // --- vertex streams
        float* P = (float*)mof_model_alloc(out, sizeof(float) * 3 * (size_t)nv);
        float* N = (float*)mof_model_alloc(out, sizeof(float) * 3 * (size_t)nv);
        float* U = (float*)mof_model_alloc(out, sizeof(float) * 2 * (size_t)nv);
        uint8_t* C = any_c ? (uint8_t*)mof_model_alloc(out, 4 * (size_t)nv) : nullptr;
        NEED(P && N && U && (!any_c || C));
        for (uint32_t v = 0; v < nv; v++) {
            const Key& k = keys[v];
            memcpy(P + 3 * (size_t)v, o->positions + 3 * (size_t)k.p, 12);
            if (k.t) { U[2 * (size_t)v] = o->texcoords[2 * (size_t)k.t]; U[2 * (size_t)v + 1] = 1.0f - o->texcoords[2 * (size_t)k.t + 1]; any_t = true; }
            if (k.n) any_n = true; else missing_n = true;
            if (C) for (int q = 0; q < 3; q++) C[4 * (size_t)v + q] = (uint8_t)(clamp01(o->colors[3 * (size_t)k.p + q]) * 255.0f + 0.5f), C[4 * (size_t)v + 3] = 255;
        }
        if (missing_n || !any_n) {  // area weighted generated normals fill in where the file has none
            mof_generate_normals(P, nv, tri, nt * 3, N);
            if (!any_n) mof_model_warn(out, "OBJ has no normals: generated area weighted normals");
        }
        for (uint32_t v = 0; v < nv; v++)
            if (keys[v].n) memcpy(N + 3 * (size_t)v, o->normals + 3 * (size_t)keys[v].n, 12);

        // --- materials (fast_obj has no default entry: face_materials index the list directly;
        // faces before the first usemtl therefore show up as the first material of the MTL)
        uint32_t nm = o->material_count;
        mof_wmaterial* mats = (mof_wmaterial*)mof_model_alloc(out, sizeof(mof_wmaterial) * (nm ? nm : 1));
        int32_t* tex_map = (int32_t*)mof_model_alloc(out, sizeof(int32_t) * (o->texture_count ? o->texture_count : 1));
        mof_wtexture* texs = (mof_wtexture*)mof_model_alloc(out, sizeof(mof_wtexture) * (o->texture_count ? o->texture_count : 1));
        NEED(mats && tex_map && texs);
        uint32_t ntex = 0;
        bool need_tangents = false;
        for (uint32_t i = 0; i < o->texture_count; i++) tex_map[i] = 0;  // 0 = untried, -1 = failed, n+1 = texture n
        auto get_tex = [&](unsigned idx) -> int32_t {
            if (idx == 0 || idx >= o->texture_count) return -1;
            if (tex_map[idx] == -1) return -1;
            if (tex_map[idx] > 0) return tex_map[idx] - 1;
            const fastObjTexture& ft = o->textures[idx];
            uint8_t* bytes = nullptr; size_t n = 0;
            char e[256] = {0};
            mof_wtexture t;
            mof_wtexture_default(&t);
            if (!ft.path || !mof_read_file(out, ft.path, &bytes, &n)) {
                mof_model_warn(out, "texture '%s' not found, dropped", ft.path ? ft.path : "?");
            } else if (!mof_make_texture_data(out, bytes, n, &t, e, sizeof e)) {
                mof_model_warn(out, "texture '%s': %s, dropped", ft.path, e);
            } else {
                t.name = mof_model_strdup(out, ft.name);
                texs[ntex] = t;
                tex_map[idx] = (int32_t)ntex + 1;
                return (int32_t)ntex++;
            }
            tex_map[idx] = -1;
            return -1;
        };
        for (uint32_t i = 0; i < nm; i++) {
            const fastObjMaterial& s = o->materials[i];
            mof_wmaterial& w = mats[i];
            mof_wmaterial_default(&w);
            w.name = mof_model_strdup(out, s.name);
            w.base_color[0] = s.Kd[0]; w.base_color[1] = s.Kd[1]; w.base_color[2] = s.Kd[2]; w.base_color[3] = s.d;
            for (int k = 0; k < 3; k++) w.emissive[k] = s.Ke[k];
            w.metallic = 0.0f;
            w.roughness = clamp01(1.0f - sqrtf(s.Ns > 0 ? s.Ns / 1000.0f : 0.0f));
            if (s.d < 1.0f) w.alpha_mode = MOF_ALPHA_BLEND;
            w.tex[MOF_SLOT_BASE_COLOR].texture = get_tex(s.map_Kd);
            w.tex[MOF_SLOT_EMISSIVE].texture = get_tex(s.map_Ke);
            w.tex[MOF_SLOT_NORMAL].texture = get_tex(s.map_bump);
            if (w.tex[MOF_SLOT_NORMAL].texture >= 0) need_tangents = true;
            // OBJ has no metal/rough map: leave the slot empty
        }
        // --- group triangles by material into submeshes
        uint32_t* counts = (uint32_t*)mof_model_alloc(out, sizeof(uint32_t) * (nm + 1));
        uint32_t* starts = (uint32_t*)mof_model_alloc(out, sizeof(uint32_t) * (nm + 2));
        uint32_t* IX = (uint32_t*)mof_model_alloc(out, sizeof(uint32_t) * (size_t)nt * 3);
        NEED(counts && starts && IX);
        auto slot = [&](uint32_t mat) -> uint32_t { return mat >= o->material_count ? 0 : mat + 1; };  // 0 = none
        for (uint32_t t = 0; t < nt; t++) counts[slot(tri_mat[t])]++;
        uint32_t nsub = 0;
        for (uint32_t s = 0; s <= nm; s++) if (counts[s]) nsub++;
        mof_wsubmesh* subs = (mof_wsubmesh*)mof_model_alloc(out, sizeof(mof_wsubmesh) * (nsub ? nsub : 1));
        NEED(subs);
        uint32_t run = 0, si = 0;
        for (uint32_t s = 0; s <= nm; s++) {
            starts[s] = run;
            if (counts[s]) { subs[si].first_index = run * 3; subs[si].index_count = counts[s] * 3; subs[si].material = (int32_t)s - 1; si++; }
            run += counts[s];
        }
        for (uint32_t s = 0; s <= nm; s++) counts[s] = 0;
        for (uint32_t t = 0; t < nt; t++) {
            uint32_t s = slot(tri_mat[t]);
            memcpy(IX + (size_t)(starts[s] + counts[s]) * 3, tri + (size_t)t * 3, 12);
            counts[s]++;
        }
        float* T = nullptr;
        if (need_tangents) {
            T = (float*)mof_model_alloc(out, sizeof(float) * 4 * (size_t)nv);
            NEED(T);
            if (any_t) mof_generate_tangents(P, N, U, nv, IX, nt * 3, T);
            else mof_model_warn(out, "normal map without texcoords; tangents are arbitrary"), mof_generate_tangents(P, N, U, nv, IX, nt * 3, T);
        }

        mof_wmesh* mesh = (mof_wmesh*)mof_model_alloc(out, sizeof(mof_wmesh));
        mof_wnode* node = (mof_wnode*)mof_model_alloc(out, sizeof(mof_wnode));
        NEED(mesh && node);
        char nm_buf[256];
        mof_wmesh_default(mesh);
        mesh->name = mof_model_strdup(out, base_name(path, nm_buf, sizeof nm_buf));
        mesh->vertex_count = nv; mesh->positions = P; mesh->normals = N; mesh->tangents = T;
        mesh->uv0 = any_t ? U : nullptr; mesh->colors = C;
        mesh->index_count = nt * 3; mesh->indices = IX; mesh->submeshes = subs; mesh->submesh_count = nsub;
        mof_wnode_default(node);
        node->name = mesh->name;
        node->mesh = 0;
        out->desc.mesh_count = 1; out->desc.meshes = mesh;
        out->desc.material_count = nm; out->desc.materials = mats;
        out->desc.texture_count = ntex; out->desc.textures = texs;
        out->desc.node_count = 1; out->desc.nodes = node;
        ok = true;
    } while (0);
done:
    fast_obj_destroy(o);
    if (!ok) mof_model_free(out);
    return ok;
}
