#include "mof/simplify.h"
#include "meshoptimizer/meshoptimizer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

void* keep(mof_simplified* s, size_t bytes) {
    void* p = calloc(1, bytes ? bytes : 1);
    if (!p) return nullptr;
    if (s->block_count == s->block_cap) {
        size_t nc = s->block_cap ? s->block_cap * 2 : 32;
        void** nb = (void**)realloc(s->blocks, nc * sizeof(void*));
        if (!nb) { free(p); return nullptr; }
        s->blocks = nb;
        s->block_cap = nc;
    }
    s->blocks[s->block_count++] = p;
    return p;
}

template <typename T> T* copy_remapped(mof_simplified* s, const T* src, uint32_t comps, const uint32_t* old_of, uint32_t n) {
    if (!src) return nullptr;
    T* d = (T*)keep(s, sizeof(T) * comps * n);
    if (!d) return nullptr;
    for (uint32_t i = 0; i < n; i++) memcpy(d + (size_t)i * comps, src + (size_t)old_of[i] * comps, sizeof(T) * comps);
    return d;
}

}  // namespace

uint64_t mof_desc_triangles(const mof_wdesc* d) {
    uint64_t t = 0;
    for (uint32_t m = 0; m < d->mesh_count; m++) t += d->meshes[m].index_count / 3;
    return t;
}

void mof_simplified_free(mof_simplified* s) {
    for (size_t i = 0; i < s->block_count; i++) free(s->blocks[i]);
    free(s->blocks);
    memset(s, 0, sizeof *s);
}

bool mof_simplify(const mof_wdesc* in, float ratio, mof_simplified* out, char* err, size_t errlen) {
    memset(out, 0, sizeof *out);
    out->desc = *in;
    if (!(ratio > 0.0f)) ratio = 0.0001f;
    if (ratio > 1.0f) ratio = 1.0f;
    out->triangles_before = mof_desc_triangles(in);
    mof_wmesh* meshes = (mof_wmesh*)keep(out, sizeof(mof_wmesh) * (in->mesh_count ? in->mesh_count : 1));
    if (!meshes) { snprintf(err, errlen, "out of memory"); return false; }
    for (uint32_t mi = 0; mi < in->mesh_count; mi++) {
        const mof_wmesh& src = in->meshes[mi];
        mof_wmesh& dst = meshes[mi];
        dst = src;
        if (!src.indices || src.index_count < 3 || ratio >= 1.0f) continue;
        mof_wsubmesh whole = { 0, src.index_count, -1 };
        const mof_wsubmesh* subs = src.submesh_count ? src.submeshes : &whole;
        uint32_t nsub = src.submesh_count ? src.submesh_count : 1;
        uint32_t* idx = (uint32_t*)keep(out, sizeof(uint32_t) * src.index_count);
        mof_wsubmesh* nsubs = (mof_wsubmesh*)keep(out, sizeof(mof_wsubmesh) * nsub);
        if (!idx || !nsubs) { snprintf(err, errlen, "out of memory"); return false; }
        uint32_t at = 0;
        for (uint32_t si = 0; si < nsub; si++) {
            const mof_wsubmesh& sm = subs[si];
            const uint32_t* sidx = src.indices + sm.first_index;
            size_t count = sm.index_count / 3 * 3;
            size_t target = (size_t)((double)count * ratio / 3.0 + 0.5) * 3;
            if (target < 3) target = 3;
            size_t got = count;
            if (target < count) {
                float e = 0;
                got = meshopt_simplify(idx + at, sidx, count, src.positions, src.vertex_count, 12, target, 0.05f, 0, &e);
                // Separate small pieces cannot be collapsed into each other: merge nearby geometry instead.
                if (got > target + target / 3)
                    got = meshopt_simplifySloppy(idx + at, sidx, count, src.positions, src.vertex_count, 12, target, 1.0f, &e);
                if (got == 0 && count >= 3) {   // everything merged away: keep the largest pieces' first triangle
                    memcpy(idx + at, sidx, 3 * sizeof(uint32_t));
                    got = 3;
                }
            } else {
                memcpy(idx + at, sidx, count * sizeof(uint32_t));
            }
            nsubs[si] = sm;
            nsubs[si].first_index = at;
            nsubs[si].index_count = (uint32_t)got;
            at += (uint32_t)got;
        }
        // Compact the vertices still used.
        uint32_t* new_of = (uint32_t*)malloc(sizeof(uint32_t) * src.vertex_count);
        uint32_t* old_of = (uint32_t*)malloc(sizeof(uint32_t) * src.vertex_count);
        if (!new_of || !old_of) { free(new_of); free(old_of); snprintf(err, errlen, "out of memory"); return false; }
        memset(new_of, 0xFF, sizeof(uint32_t) * src.vertex_count);
        uint32_t nv = 0;
        for (uint32_t i = 0; i < at; i++) {
            uint32_t v = idx[i];
            if (new_of[v] == 0xFFFFFFFFu) { new_of[v] = nv; old_of[nv++] = v; }
            idx[i] = new_of[v];
        }
        dst.vertex_count = nv;
        dst.positions = copy_remapped(out, src.positions, 3, old_of, nv);
        dst.normals = copy_remapped(out, src.normals, 3, old_of, nv);
        dst.tangents = copy_remapped(out, src.tangents, 4, old_of, nv);
        dst.uv0 = copy_remapped(out, src.uv0, 2, old_of, nv);
        dst.uv1 = copy_remapped(out, src.uv1, 2, old_of, nv);
        dst.colors = copy_remapped(out, src.colors, 4, old_of, nv);
        dst.joints = copy_remapped(out, src.joints, 4, old_of, nv);
        dst.weights_f32 = copy_remapped(out, src.weights_f32, 4, old_of, nv);
        dst.weights_u16 = copy_remapped(out, src.weights_u16, 4, old_of, nv);
        free(new_of);
        free(old_of);
        if (nv && !dst.positions) { snprintf(err, errlen, "out of memory"); return false; }
        dst.indices = idx;
        dst.index_count = at;
        dst.submeshes = nsubs;
        dst.submesh_count = nsub;
    }
    out->desc.meshes = meshes;
    out->triangles_after = mof_desc_triangles(&out->desc);
    return true;
}
