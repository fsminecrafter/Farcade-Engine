#include "mof_info.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void fourcc(uint32_t t, char out[5]) {
    for (int i = 0; i < 4; i++) {
        char c = (char)(t >> (8 * i));
        out[i] = (c >= 32 && c < 127) ? c : '?';
    }
    out[4] = 0;
}

static const char* alpha_name(uint32_t flags) {
    switch ((flags & MOF_MATERIAL_ALPHA_MASK) >> MOF_MATERIAL_ALPHA_SHIFT) {
        case MOF_ALPHA_MASK: return "mask";
        case MOF_ALPHA_BLEND: return "blend";
    }
    return "opaque";
}

// Prints a parent-before-child array as an indented tree (iterative: files can hold 65536 deep chains).
template <class T>
static void print_tree(const T* items, uint32_t n, const mof_file* f, FILE* out, void (*extra)(const mof_file*, const T*, FILE*)) {
    if (n == 0) return;
    uint32_t* first = (uint32_t*)malloc(sizeof(uint32_t) * n * 3);
    if (!first) { fprintf(out, "  (out of memory)\n"); return; }
    uint32_t *next = first + n, *stack = first + 2 * n;
    for (uint32_t i = 0; i < n; i++) first[i] = next[i] = 0xFFFFFFFFu;
    // build child lists keeping index order
    uint32_t* last = (uint32_t*)malloc(sizeof(uint32_t) * n);
    if (!last) { free(first); return; }
    for (uint32_t i = 0; i < n; i++) last[i] = 0xFFFFFFFFu;
    uint32_t root_first = 0xFFFFFFFFu, root_last = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        int32_t p = items[i].parent;
        if (p < 0) {
            if (root_last == 0xFFFFFFFFu) root_first = i; else next[root_last] = i;
            root_last = i;
        } else {
            if (last[p] == 0xFFFFFFFFu) first[p] = i; else next[last[p]] = i;
            last[p] = i;
        }
    }
    // explicit DFS: stack of siblings-to-visit with depth
    uint32_t* depth = (uint32_t*)malloc(sizeof(uint32_t) * n);
    if (!depth) { free(first); free(last); return; }
    uint32_t sp = 0;
    if (root_first != 0xFFFFFFFFu) { stack[sp++] = root_first; depth[root_first] = 0; }
    while (sp) {
        uint32_t i = stack[--sp];
        if (next[i] != 0xFFFFFFFFu) { stack[sp++] = next[i]; depth[next[i]] = depth[i]; }
        fprintf(out, "  %*s[%u] \"%s\"", (int)(depth[i] * 2), "", i, mof_string(f, items[i].name));
        if (extra) extra(f, &items[i], out);
        fprintf(out, "\n");
        if (first[i] != 0xFFFFFFFFu) { stack[sp++] = first[i]; depth[first[i]] = depth[i] + 1; }
    }
    free(depth); free(last); free(first);
}

static void bone_extra(const mof_file*, const mof_bone* b, FILE* out) {
    fprintf(out, "  t=(%g, %g, %g)", b->translation[0], b->translation[1], b->translation[2]);
}
static void node_extra(const mof_file* f, const mof_node* n, FILE* out) {
    if (n->mesh >= 0) fprintf(out, "  mesh %d \"%s\"%s", n->mesh, mof_string(f, f->meshes[n->mesh].name), (n->flags & MOF_NODE_SKINNED) ? " (skinned)" : "");
    fprintf(out, "  t=(%g, %g, %g)", n->translation[0], n->translation[1], n->translation[2]);
}

void mof_print_info(const mof_file* f, FILE* out) {
    fprintf(out, "mof version %u.%u, %zu bytes, %u chunks\n", f->version_major, f->version_minor, f->size, f->chunk_count);
    fprintf(out, "chunks:");
    for (uint32_t i = 0; i < f->chunk_count; i++) {
        char cc[5];
        fourcc(f->chunks[i].type, cc);
        fprintf(out, " %s(%llu)", cc, (unsigned long long)f->chunks[i].size);
    }
    fprintf(out, "\n");

    uint64_t tv = 0, tt = 0;
    for (uint32_t i = 0; i < f->mesh_count; i++) { tv += f->meshes[i].vertex_count; tt += f->meshes[i].index_count / 3; }
    fprintf(out, "meshes: %u (%llu vertices, %llu triangles)\n", f->mesh_count, (unsigned long long)tv, (unsigned long long)tt);
    for (uint32_t i = 0; i < f->mesh_count; i++) {
        const mof_mesh& m = f->meshes[i];
        fprintf(out, "  [%u] \"%s\": %u vertices, %u triangles, %s indices, streams: P", i, mof_string(f, m.name), m.vertex_count,
                m.index_count / 3, m.index_format == MOF_INDEX_U16 ? "u16" : "u32");
        if (m.stream_mask & MOF_STREAM_NORMAL) fprintf(out, " N");
        if (m.stream_mask & MOF_STREAM_TANGENT) fprintf(out, " T");
        if (m.stream_mask & MOF_STREAM_UV0) fprintf(out, " UV0");
        if (m.stream_mask & MOF_STREAM_UV1) fprintf(out, " UV1");
        if (m.stream_mask & MOF_STREAM_COLOR) fprintf(out, " C");
        if (m.stream_mask & MOF_STREAM_SKIN) fprintf(out, " SKIN(%s)", m.weight_format == MOF_WEIGHTS_F32 ? "f32" : "unorm16");
        fprintf(out, "\n      bounds (%g, %g, %g) .. (%g, %g, %g), sphere (%g, %g, %g) r=%g\n", m.bounds_min[0], m.bounds_min[1], m.bounds_min[2],
                m.bounds_max[0], m.bounds_max[1], m.bounds_max[2], m.sphere_center[0], m.sphere_center[1], m.sphere_center[2], m.sphere_radius);
        for (uint32_t k = 0; k < m.submesh_count; k++) {
            const mof_submesh& s = f->submeshes[m.first_submesh + k];
            fprintf(out, "      submesh %u: indices [%u, +%u) material ", k, s.first_index, s.index_count);
            if (s.material == MOF_NONE) fprintf(out, "none\n");
            else fprintf(out, "%u \"%s\"\n", s.material, mof_string(f, f->materials[s.material].name));
        }
    }

    static const char* slot_names[MOF_SLOT_COUNT] = {"baseColor", "normal", "metalRough", "occlusion", "emissive"};
    fprintf(out, "materials: %u\n", f->material_count);
    for (uint32_t i = 0; i < f->material_count; i++) {
        const mof_material& m = f->materials[i];
        fprintf(out, "  [%u] \"%s\": base (%g, %g, %g, %g) metallic %g roughness %g emissive (%g, %g, %g) alpha %s%s", i, mof_string(f, m.name),
                m.base_color[0], m.base_color[1], m.base_color[2], m.base_color[3], m.metallic, m.roughness, m.emissive[0], m.emissive[1],
                m.emissive[2], alpha_name(m.flags),
                (m.flags & MOF_MATERIAL_DOUBLE_SIDED) ? ", double sided" : "");
        if (((m.flags & MOF_MATERIAL_ALPHA_MASK) >> MOF_MATERIAL_ALPHA_SHIFT) == MOF_ALPHA_MASK) fprintf(out, " (cutoff %g)", m.alpha_cutoff);
        for (uint32_t s = 0; s < MOF_SLOT_COUNT; s++)
            if (m.tex[s].texture != MOF_NONE) fprintf(out, "\n      %s: texture %u uv%u", slot_names[s], m.tex[s].texture, m.tex[s].uv_set);
        fprintf(out, "\n");
    }

    static const char* codecs[] = {"raw RGBA8", "PNG", "JPEG"};
    static const char* wraps[] = {"repeat", "clamp", "mirror"};
    uint64_t tex_bytes = 0;
    for (uint32_t i = 0; i < f->texture_count; i++) tex_bytes += f->textures[i].data_size;
    fprintf(out, "textures: %u (%llu bytes)\n", f->texture_count, (unsigned long long)tex_bytes);
    for (uint32_t i = 0; i < f->texture_count; i++) {
        const mof_texture& t = f->textures[i];
        fprintf(out, "  [%u] \"%s\": %s %ux%u, %u mips, %llu bytes, wrap %s/%s, filter %s/%s\n", i, mof_string(f, t.name), codecs[t.codec], t.width,
                t.height, t.mip_count, (unsigned long long)t.data_size, wraps[t.wrap_s], wraps[t.wrap_t], t.min_filter ? "linear" : "nearest",
                t.mag_filter ? "linear" : "nearest");
    }

    fprintf(out, "bones: %u\n", f->bone_count);
    print_tree<mof_bone>(f->bones, f->bone_count, f, out, bone_extra);
    fprintf(out, "nodes: %u\n", f->node_count);
    print_tree<mof_node>(f->nodes, f->node_count, f, out, node_extra);
}
