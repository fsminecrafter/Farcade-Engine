// .mof writer: builds a file image from a mof_wdesc into a growable buffer.
#include "mof/mof.h"
#include "mof/mof_internal.h"

using mofi::fail;

// ------------------------------------------------------------------ defaults

void mof_wmesh_default(mof_wmesh* m) { MOF_MEMSET(m, 0, sizeof *m); }

void mof_wmaterial_default(mof_wmaterial* m) {
    MOF_MEMSET(m, 0, sizeof *m);
    for (int i = 0; i < 4; i++) m->base_color[i] = 1.0f;
    m->metallic = 1.0f;
    m->roughness = 1.0f;
    m->normal_scale = 1.0f;
    m->occlusion_strength = 1.0f;
    m->alpha_cutoff = 0.5f;
    for (uint32_t s = 0; s < MOF_SLOT_COUNT; s++) m->tex[s].texture = -1;
}

void mof_wtexture_default(mof_wtexture* t) {
    MOF_MEMSET(t, 0, sizeof *t);
    t->mip_count = 1;
    t->min_filter = MOF_FILTER_LINEAR;
    t->mag_filter = MOF_FILTER_LINEAR;
    t->mip_filter = MOF_MIP_NONE;
}

void mof_wbone_default(mof_wbone* b) {
    MOF_MEMSET(b, 0, sizeof *b);
    b->parent = -1;
    b->node = -1;
    b->rotation[3] = 1.0f;
    b->scale[0] = b->scale[1] = b->scale[2] = 1.0f;
    b->inverse_bind[0] = b->inverse_bind[5] = b->inverse_bind[10] = b->inverse_bind[15] = 1.0f;
}

void mof_wnode_default(mof_wnode* n) {
    MOF_MEMSET(n, 0, sizeof *n);
    n->parent = -1;
    n->mesh = -1;
    n->rotation[3] = 1.0f;
    n->scale[0] = n->scale[1] = n->scale[2] = 1.0f;
}

// ------------------------------------------------------------------- buffer

void mof_buffer_init(mof_buffer* b, mof_alloc alloc) {
    b->data = nullptr;
    b->size = b->capacity = 0;
    b->alloc = alloc;
}

void mof_buffer_free(mof_buffer* b) {
    if (b->data && b->alloc.realloc_fn) b->alloc.realloc_fn(b->alloc.user, b->data, b->capacity, 0);
    b->data = nullptr;
    b->size = b->capacity = 0;
}

namespace {

constexpr uint64_t kMaxFile = 0x7FFFFFFFull;  // keeps every u32 offset and size safe

struct Ctx {
    const mof_wdesc* d;
    mof_buffer* b;
    char* err;
    size_t errlen;
    bool ok = true;

    bool bad(const char* msg, uint32_t idx = 0xFFFFFFFFu) {
        if (ok) fail(err, errlen, msg, idx);
        ok = false;
        return false;
    }
    void* alloc(size_t n) {
        void* p = d->alloc.realloc_fn(d->alloc.user, nullptr, 0, n ? n : 1);
        if (!p) { bad("out of memory"); return nullptr; }
        MOF_MEMSET(p, 0, n ? n : 1);
        return p;
    }
    void release(void* p, size_t n) {
        if (p) d->alloc.realloc_fn(d->alloc.user, p, n ? n : 1, 0);
    }
    // Appends n zeroed bytes to the file image; returns pointer to them.
    uint8_t* append(uint64_t n) {
        if (!ok) return nullptr;
        if ((uint64_t)b->size + n > kMaxFile) { bad("model too large"); return nullptr; }
        size_t need = b->size + (size_t)n;
        if (need > b->capacity) {
            size_t cap = b->capacity ? b->capacity : 4096;
            while (cap < need) cap *= 2;
            uint8_t* p = (uint8_t*)d->alloc.realloc_fn(d->alloc.user, b->data, b->capacity, cap);
            if (!p) { bad("out of memory"); return nullptr; }
            b->data = p;
            b->capacity = cap;
        }
        uint8_t* r = b->data + b->size;
        MOF_MEMSET(r, 0, (size_t)n);
        b->size = need;
        return r;
    }
    void align16() {
        size_t pad = (16 - (b->size & 15)) & 15;
        if (pad) append(pad);
    }
};

// Growing scratch string table: offset 0 is the empty string.
struct Strings {
    Ctx* c;
    uint8_t* d = nullptr;
    size_t n = 0, cap = 0;

    bool init(Ctx* ctx) {
        c = ctx;
        cap = 256;
        d = (uint8_t*)c->alloc(cap);
        n = 1;  // d[0] == 0
        return d != nullptr;
    }
    static size_t len(const char* s) { size_t l = 0; while (s[l]) l++; return l; }
    uint32_t intern(const char* s) {
        if (!s || !s[0] || !c->ok) return 0;
        size_t l = len(s);
        if (l > 4096) l = 4096;  // names are truncated, never rejected
        for (size_t off = 1; off < n;) {  // dedupe
            size_t sl = len((const char*)d + off);
            if (sl == l && MOF_MEMCMP(d + off, s, l) == 0) return (uint32_t)off;
            off += sl + 1;
        }
        if (n + l + 1 > cap) {
            size_t ncap = cap;
            while (ncap < n + l + 1) ncap *= 2;
            uint8_t* nd = (uint8_t*)c->alloc(ncap);
            if (!nd) return 0;
            MOF_MEMCPY(nd, d, n);
            c->release(d, cap);
            d = nd;
            cap = ncap;
        }
        uint32_t off = (uint32_t)n;
        MOF_MEMCPY(d + n, s, l);
        d[n + l] = 0;
        n += l + 1;
        return off;
    }
};

// Newton square root (avoids a libm dependency in freestanding builds).
float msqrt(float x) {
    if (!(x > 0.0f) || x - x != 0.0f) return 0.0f;
    uint32_t bits;
    MOF_MEMCPY(&bits, &x, 4);
    bits = (bits >> 1) + 0x1FBD1DF5u;
    float y;
    MOF_MEMCPY(&y, &bits, 4);
    for (int i = 0; i < 4; i++) y = 0.5f * (y + x / y);
    return y;
}

struct ChunkOut {
    uint32_t type;
    uint32_t flags;
    uint64_t offset, size;
    uint32_t count;
};

}  // namespace

bool mof_write(const mof_wdesc* desc, mof_buffer* out, char* err, size_t errlen) {
    if (err && errlen) err[0] = 0;
    if (!desc || !out || !desc->alloc.realloc_fn) return fail(err, errlen, "bad arguments");
    mof_alloc alloc = desc->alloc;
    mof_buffer_init(out, alloc);

    Ctx c{desc, out, err, errlen};
    Strings strs;
    mof_mesh* mrec = nullptr;
    mof_texture* trec = nullptr;
    ChunkOut chunks[16 + 64];
    uint32_t nch = 0;
    const uint32_t mc = desc->mesh_count, tc = desc->texture_count;

    if ((mc && !desc->meshes) || (desc->material_count && !desc->materials) || (tc && !desc->textures) ||
        (desc->bone_count && !desc->bones) || (desc->node_count && !desc->nodes) ||
        (desc->extra_chunk_count && !desc->extra_chunks))
        return fail(err, errlen, "null array in description");
    if (desc->extra_chunk_count > 32) return fail(err, errlen, "too many extra chunks");
    if (desc->bone_count > MOF_MAX_BONES) return fail(err, errlen, "too many bones");

    auto begin = [&](uint32_t type, uint32_t count) -> ChunkOut* {
        c.align16();
        ChunkOut* o = &chunks[nch++];
        o->type = type; o->flags = 0; o->offset = out->size; o->size = 0; o->count = count;
        return o;
    };
    auto end = [&](ChunkOut* o) { o->size = out->size - o->offset; };

    // Which chunks exist is known up front (fixes the table size).
    uint32_t total_sub = 0;
    for (uint32_t i = 0; i < mc; i++) {
        const mof_wmesh& m = desc->meshes[i];
        if (m.submesh_count && !m.submeshes) return fail(err, errlen, "null submesh array", i);
        total_sub += m.submesh_count ? m.submesh_count : (m.index_count ? 1u : 0u);
    }
    uint32_t chunk_total = 1 /*STRS*/ + desc->extra_chunk_count;
    if (mc) chunk_total += 3 + (total_sub ? 1 : 0u);  // MESH VERT INDX [SUBM]
    if (desc->material_count) chunk_total++;
    if (tc) chunk_total += 2;
    if (desc->bone_count) chunk_total++;
    if (desc->node_count) chunk_total++;

    // header + table placeholder
    uint8_t* hdr = c.append(sizeof(mof_header) + (uint64_t)chunk_total * sizeof(mof_chunk_entry));
    if (!hdr) { mof_buffer_free(out); return false; }

    if (!strs.init(&c)) { mof_buffer_free(out); return false; }
    if (mc) { mrec = (mof_mesh*)c.alloc(sizeof(mof_mesh) * mc); }
    if (tc) { trec = (mof_texture*)c.alloc(sizeof(mof_texture) * tc); }
    uint8_t* wsub_dummy = nullptr; (void)wsub_dummy;
    mof_submesh* subs = nullptr;
    if (total_sub) subs = (mof_submesh*)c.alloc(sizeof(mof_submesh) * total_sub);

    // ---- vertices + indices (fills mrec offsets)
    if (c.ok && mc) {
        ChunkOut* vo = begin(MOF_CHUNK_VERTICES, 0);
        uint32_t sub_cursor = 0;
        for (uint32_t i = 0; i < mc && c.ok; i++) {
            const mof_wmesh& m = desc->meshes[i];
            mof_mesh& r = mrec[i];
            r.name = strs.intern(m.name);
            r.vertex_count = m.vertex_count;
            r.index_count = m.index_count;
            if (m.vertex_count && !m.positions) { c.bad("mesh without positions", i); break; }
            if ((uint64_t)m.vertex_count * 16 > kMaxFile) { c.bad("mesh too large", i); break; }
            const bool skin = m.joints || m.weights_f32 || m.weights_u16;
            if (skin && !(m.joints && (m.weights_f32 || m.weights_u16))) { c.bad("skin needs joints and weights", i); break; }
            r.stream_mask = (m.normals ? MOF_STREAM_NORMAL : 0u) | (m.tangents ? MOF_STREAM_TANGENT : 0u) |
                            (m.uv0 ? MOF_STREAM_UV0 : 0u) | (m.uv1 ? MOF_STREAM_UV1 : 0u) |
                            (m.colors ? MOF_STREAM_COLOR : 0u) | (skin ? MOF_STREAM_SKIN : 0u);
            r.weight_format = m.weights_f32 ? MOF_WEIGHTS_F32 : MOF_WEIGHTS_UNORM16;
            const uint64_t nv = m.vertex_count;
            auto put = [&](const void* src, uint64_t bytes) -> uint32_t {
                c.align16();
                uint32_t off = (uint32_t)(out->size - vo->offset);
                uint8_t* p = c.append(bytes);
                if (p && bytes) MOF_MEMCPY(p, src, (size_t)bytes);
                return off;
            };
            r.off_position = put(m.positions, nv * 12);
            if (m.normals) r.off_normal = put(m.normals, nv * 12);
            if (m.tangents) r.off_tangent = put(m.tangents, nv * 16);
            if (m.uv0) r.off_uv0 = put(m.uv0, nv * 8);
            if (m.uv1) r.off_uv1 = put(m.uv1, nv * 8);
            if (m.colors) r.off_color = put(m.colors, nv * 4);
            if (skin) {
                r.off_joints = put(m.joints, nv * 8);
                r.off_weights = m.weights_f32 ? put(m.weights_f32, nv * 16) : put(m.weights_u16, nv * 8);
            }
            // bounds
            if (nv) {
                float mn[3], mx[3];
                for (uint64_t v = 0; v < nv * 3; v++)
                    if (!mofi::finite(m.positions[v])) { c.bad("non-finite vertex position", i); break; }
                if (!c.ok) break;
                for (int a = 0; a < 3; a++) mn[a] = mx[a] = m.positions[a];
                for (uint64_t v = 1; v < nv; v++)
                    for (int a = 0; a < 3; a++) {
                        float p = m.positions[v * 3 + a];
                        if (p < mn[a]) mn[a] = p;
                        if (p > mx[a]) mx[a] = p;
                    }
                float cen[3], r2 = 0;
                for (int a = 0; a < 3; a++) { r.bounds_min[a] = mn[a]; r.bounds_max[a] = mx[a]; cen[a] = r.sphere_center[a] = 0.5f * (mn[a] + mx[a]); }
                for (uint64_t v = 0; v < nv; v++) {
                    float dx = m.positions[v * 3] - cen[0], dy = m.positions[v * 3 + 1] - cen[1], dz = m.positions[v * 3 + 2] - cen[2];
                    float q = dx * dx + dy * dy + dz * dz;
                    if (q > r2) r2 = q;
                }
                r.sphere_radius = msqrt(r2);
            }
            // submeshes
            r.first_submesh = sub_cursor;
            if (m.submesh_count) {
                for (uint32_t k = 0; k < m.submesh_count; k++) {
                    const mof_wsubmesh& s = m.submeshes[k];
                    subs[sub_cursor].first_index = s.first_index;
                    subs[sub_cursor].index_count = s.index_count;
                    subs[sub_cursor].material = s.material < 0 ? MOF_NONE : (uint32_t)s.material;
                    sub_cursor++;
                }
                r.submesh_count = m.submesh_count;
            } else if (m.index_count) {
                subs[sub_cursor].first_index = 0;
                subs[sub_cursor].index_count = m.index_count;
                subs[sub_cursor].material = MOF_NONE;
                sub_cursor++;
                r.submesh_count = 1;
            }
        }
        if (c.ok) end(vo);

        if (c.ok) {
            ChunkOut* io = begin(MOF_CHUNK_INDICES, 0);
            for (uint32_t i = 0; i < mc && c.ok; i++) {
                const mof_wmesh& m = desc->meshes[i];
                mof_mesh& r = mrec[i];
                if (m.index_count && !m.indices) { c.bad("mesh without indices", i); break; }
                for (uint32_t k = 0; k < m.index_count; k++)
                    if (m.indices[k] >= m.vertex_count) { c.bad("index value >= vertex_count", i); break; }
                if (!c.ok) break;
                const bool wide = m.force_index32 || m.vertex_count > 65536;
                r.index_format = wide ? MOF_INDEX_U32 : MOF_INDEX_U16;
                c.align16();
                r.off_indices = (uint32_t)(out->size - io->offset);
                uint8_t* p = c.append((uint64_t)m.index_count * (wide ? 4 : 2));
                if (!p) break;
                if (wide) {
                    if (m.index_count) MOF_MEMCPY(p, m.indices, (size_t)m.index_count * 4);
                } else {
                    for (uint32_t k = 0; k < m.index_count; k++) {
                        uint16_t v = (uint16_t)m.indices[k];
                        MOF_MEMCPY(p + k * 2, &v, 2);
                    }
                }
            }
            if (c.ok) end(io);
        }
        if (c.ok) {
            ChunkOut* mo = begin(MOF_CHUNK_MESHES, mc);
            uint8_t* p = c.append((uint64_t)mc * sizeof(mof_mesh));
            if (p) MOF_MEMCPY(p, mrec, (size_t)mc * sizeof(mof_mesh));
            end(mo);
        }
        if (c.ok && total_sub) {
            ChunkOut* so = begin(MOF_CHUNK_SUBMESHES, total_sub);
            uint8_t* p = c.append((uint64_t)total_sub * sizeof(mof_submesh));
            if (p) MOF_MEMCPY(p, subs, (size_t)total_sub * sizeof(mof_submesh));
            end(so);
        }
    }

    // ---- materials
    if (c.ok && desc->material_count) {
        ChunkOut* o = begin(MOF_CHUNK_MATERIALS, desc->material_count);
        for (uint32_t i = 0; i < desc->material_count && c.ok; i++) {
            const mof_wmaterial& w = desc->materials[i];
            mof_material* r = (mof_material*)c.append(sizeof(mof_material));
            if (!r) break;
            r->name = strs.intern(w.name);
            r->flags = (w.double_sided ? MOF_MATERIAL_DOUBLE_SIDED : 0u) | ((w.alpha_mode << MOF_MATERIAL_ALPHA_SHIFT) & MOF_MATERIAL_ALPHA_MASK);
            r->alpha_cutoff = w.alpha_cutoff; r->metallic = w.metallic; r->roughness = w.roughness;
            r->normal_scale = w.normal_scale; r->occlusion_strength = w.occlusion_strength;
            for (int a = 0; a < 4; a++) r->base_color[a] = w.base_color[a];
            for (int a = 0; a < 3; a++) r->emissive[a] = w.emissive[a];
            for (uint32_t s = 0; s < MOF_SLOT_COUNT; s++) {
                r->tex[s].texture = w.tex[s].texture < 0 ? MOF_NONE : (uint32_t)w.tex[s].texture;
                r->tex[s].uv_set = w.tex[s].uv_set;
            }
        }
        end(o);
    }

    // ---- texture payloads, then records
    if (c.ok && tc) {
        ChunkOut* o = begin(MOF_CHUNK_TEXDATA, 0);
        for (uint32_t i = 0; i < tc && c.ok; i++) {
            const mof_wtexture& w = desc->textures[i];
            if (w.data_size && !w.data) { c.bad("texture without data", i); break; }
            uint64_t off = 0;
            bool shared = false;
            for (uint32_t j = 0; j < i; j++)
                if (desc->textures[j].data == w.data && desc->textures[j].data_size == w.data_size && w.data_size) {
                    off = trec[j].data_offset;
                    shared = true;
                    break;
                }
            if (!shared) {
                c.align16();
                off = out->size - o->offset;
                uint8_t* p = c.append(w.data_size);
                if (p && w.data_size) MOF_MEMCPY(p, w.data, (size_t)w.data_size);
            }
            mof_texture& r = trec[i];
            r.name = strs.intern(w.name);
            r.codec = w.codec; r.width = w.width; r.height = w.height; r.mip_count = w.mip_count;
            r.wrap_s = w.wrap_s; r.wrap_t = w.wrap_t; r.min_filter = w.min_filter; r.mag_filter = w.mag_filter;
            r.mip_filter = w.mip_filter;
            r.data_offset = off; r.data_size = w.data_size;
        }
        end(o);
        if (c.ok) {
            ChunkOut* t = begin(MOF_CHUNK_TEXTURES, tc);
            uint8_t* p = c.append((uint64_t)tc * sizeof(mof_texture));
            if (p) MOF_MEMCPY(p, trec, (size_t)tc * sizeof(mof_texture));
            end(t);
        }
    }

    // ---- skeleton
    if (c.ok && desc->bone_count) {
        ChunkOut* o = begin(MOF_CHUNK_SKELETON, desc->bone_count);
        for (uint32_t i = 0; i < desc->bone_count && c.ok; i++) {
            const mof_wbone& w = desc->bones[i];
            mof_bone* r = (mof_bone*)c.append(sizeof(mof_bone));
            if (!r) break;
            r->name = strs.intern(w.name);
            r->parent = w.parent;
            r->node = w.node < 0 ? MOF_NONE : (uint32_t)w.node;
            for (int a = 0; a < 3; a++) { r->translation[a] = w.translation[a]; r->scale[a] = w.scale[a]; }
            for (int a = 0; a < 4; a++) r->rotation[a] = w.rotation[a];
            for (int a = 0; a < 16; a++) r->inverse_bind[a] = w.inverse_bind[a];
        }
        end(o);
    }

    // ---- nodes
    if (c.ok && desc->node_count) {
        ChunkOut* o = begin(MOF_CHUNK_NODES, desc->node_count);
        for (uint32_t i = 0; i < desc->node_count && c.ok; i++) {
            const mof_wnode& w = desc->nodes[i];
            mof_node* r = (mof_node*)c.append(sizeof(mof_node));
            if (!r) break;
            r->name = strs.intern(w.name);
            r->parent = w.parent; r->mesh = w.mesh;
            r->flags = w.skinned ? MOF_NODE_SKINNED : 0u;
            for (int a = 0; a < 3; a++) { r->translation[a] = w.translation[a]; r->scale[a] = w.scale[a]; }
            for (int a = 0; a < 4; a++) r->rotation[a] = w.rotation[a];
        }
        end(o);
    }

    // ---- extra (opaque) chunks
    for (uint32_t i = 0; i < desc->extra_chunk_count && c.ok; i++) {
        const mof_wchunk& w = desc->extra_chunks[i];
        if (w.size && !w.data) { c.bad("extra chunk without data", i); break; }
        ChunkOut* o = begin(w.type, 0);
        o->flags = w.flags;
        uint8_t* p = c.append(w.size);
        if (p && w.size) MOF_MEMCPY(p, w.data, (size_t)w.size);
        end(o);
    }

    // ---- strings last (names are interned while records are written)
    if (c.ok) {
        ChunkOut* o = begin(MOF_CHUNK_STRINGS, (uint32_t)strs.n);
        uint8_t* p = c.append(strs.n);
        if (p) MOF_MEMCPY(p, strs.d, strs.n);
        end(o);
    }

    c.release(strs.d, strs.cap);
    c.release(mrec, sizeof(mof_mesh) * mc);
    c.release(trec, sizeof(mof_texture) * tc);
    c.release(subs, sizeof(mof_submesh) * total_sub);

    if (!c.ok) { mof_buffer_free(out); return false; }
    if (nch != chunk_total) { mof_buffer_free(out); return fail(err, errlen, "internal error: chunk count"); }
    c.align16();

    // ---- header and table
    mof_header* h = (mof_header*)out->data;
    MOF_MEMCPY(h->magic, "MOF\0", 4);
    h->version_major = MOF_VERSION_MAJOR;
    h->version_minor = (uint16_t)(desc->minor_version ? desc->minor_version : MOF_VERSION_MINOR);
    h->flags = 0;
    h->chunk_count = nch;
    h->header_size = sizeof(mof_header);
    h->chunk_entry_size = sizeof(mof_chunk_entry);
    h->table_offset = sizeof(mof_header);
    h->file_size = out->size;
    mof_chunk_entry* te = (mof_chunk_entry*)(out->data + sizeof(mof_header));
    for (uint32_t i = 0; i < nch; i++) {
        te[i].type = chunks[i].type;
        te[i].flags = chunks[i].flags;
        te[i].offset = chunks[i].offset;
        te[i].size = chunks[i].size;
        te[i].count = chunks[i].count;
    }

    if (!desc->skip_verify) {
        mof_file f;
        char msg[160];
        if (!mof_open(out->data, out->size, &f, msg, sizeof msg)) {
            mof_buffer_free(out);
            fail(err, errlen, "written file failed validation: ");
            if (err && errlen) {  // append the reader message
                size_t n = 0;
                while (err[n]) n++;
                for (size_t k = 0; msg[k] && n + 1 < errlen; k++) err[n++] = msg[k];
                err[n] = 0;
            }
            return false;
        }
    }
    return true;
}
