// .mof reader: full validation, then zero-copy views. See docs/mof-format.md.
#include "mof/mof.h"
#include "mof/mof_internal.h"

using mofi::fail;
using mofi::finite;

namespace {

struct Slot {
    bool present = false;
    const uint8_t* p = nullptr;
    uint64_t size = 0;
    uint32_t count = 0;
};

enum { S_STRS, S_MESH, S_SUBM, S_VERT, S_INDX, S_MATL, S_TEXT, S_TEXD, S_SKEL, S_NODE, S_N };

int slot_of(uint32_t type) {
    switch (type) {
        case MOF_CHUNK_STRINGS: return S_STRS;
        case MOF_CHUNK_MESHES: return S_MESH;
        case MOF_CHUNK_SUBMESHES: return S_SUBM;
        case MOF_CHUNK_VERTICES: return S_VERT;
        case MOF_CHUNK_INDICES: return S_INDX;
        case MOF_CHUNK_MATERIALS: return S_MATL;
        case MOF_CHUNK_TEXTURES: return S_TEXT;
        case MOF_CHUNK_TEXDATA: return S_TEXD;
        case MOF_CHUNK_SKELETON: return S_SKEL;
        case MOF_CHUNK_NODES: return S_NODE;
    }
    return -1;
}

bool all_finite(const float* v, int n) {
    for (int i = 0; i < n; i++)
        if (!finite(v[i])) return false;
    return true;
}

// [off, off + count * stride) inside a blob of `blob` bytes, off 16-aligned.
bool stream_ok(uint32_t off, uint64_t count, uint64_t stride, uint64_t blob) {
    if (off % 16) return false;
    uint64_t bytes = count * stride;  // count < 2^32, stride <= 16: cannot overflow
    return (uint64_t)off <= blob && bytes <= blob - off;
}

// Validates the header and the whole chunk table: everything a reader needs to
// trust chunk offsets. `avail` bytes are readable at d. With `whole` the buffer
// must hold the complete file; without it only the header and the table need to
// be present (partial access) and chunk ranges are checked against file_size.
bool check_head(const uint8_t* d, size_t avail, bool whole, uint64_t* table_end_out, char* err, size_t errlen) {
    if (!d) return fail(err, errlen, "null buffer");
    if ((uintptr_t)d % 16 != 0) return fail(err, errlen, "buffer is not 16-byte aligned");
    if (avail < sizeof(mof_header)) return fail(err, errlen, "file smaller than header");

    const mof_header* h = (const mof_header*)d;
    if (MOF_MEMCMP(h->magic, "MOF\0", 4) != 0) return fail(err, errlen, "bad magic");
    if (h->version_major != MOF_VERSION_MAJOR) return fail(err, errlen, "unsupported major version");
    if (h->header_size < sizeof(mof_header) || h->header_size % 16 != 0) return fail(err, errlen, "bad header_size");
    if (h->chunk_entry_size != sizeof(mof_chunk_entry)) return fail(err, errlen, "bad chunk_entry_size");
    if (h->file_size < h->header_size) return fail(err, errlen, "file_size smaller than header");
    if (whole && h->file_size > (uint64_t)avail) return fail(err, errlen, "file truncated (file_size exceeds buffer)");
    const uint64_t fsize = h->file_size;
    if (h->chunk_count > MOF_MAX_CHUNKS) return fail(err, errlen, "too many chunks");
    if (h->table_offset % 16 != 0 || h->table_offset < h->header_size) return fail(err, errlen, "bad table_offset");
    const uint64_t table_bytes = (uint64_t)h->chunk_count * sizeof(mof_chunk_entry);
    if (h->table_offset > fsize || table_bytes > fsize - h->table_offset) return fail(err, errlen, "chunk table out of range");
    const uint64_t table_end = h->table_offset + table_bytes;
    if (table_end > (uint64_t)avail) return fail(err, errlen, "buffer does not hold the whole chunk table");
    const mof_chunk_entry* ce = (const mof_chunk_entry*)(d + h->table_offset);

    for (uint32_t i = 0; i < h->chunk_count; i++) {
        const mof_chunk_entry* e = &ce[i];
        if (e->offset % 16 != 0) return fail(err, errlen, "chunk offset not 16-aligned", i);
        if (e->offset < table_end) return fail(err, errlen, "chunk overlaps header or table", i);
        if (e->offset > fsize || e->size > fsize - e->offset) return fail(err, errlen, "chunk out of range", i);
        const bool known = slot_of(e->type) >= 0;
        if (!known && (e->flags & MOF_CHUNK_FLAG_REQUIRED)) return fail(err, errlen, "unknown required chunk", i);
        for (uint32_t j = 0; j < i; j++) {
            const mof_chunk_entry* o = &ce[j];
            if (e->size && o->size && e->offset < o->offset + o->size && o->offset < e->offset + e->size)
                return fail(err, errlen, "chunks overlap", i);
            if (known && o->type == e->type) return fail(err, errlen, "duplicate chunk", i);
        }
    }
    if (table_end_out) *table_end_out = table_end;
    return true;
}

}  // namespace

bool mof_open(const void* data, size_t size, mof_file* out, char* err, size_t errlen) {
    if (err && errlen) err[0] = 0;
    if (!out) return fail(err, errlen, "null output");
    MOF_MEMSET(out, 0, sizeof *out);
    const uint8_t* d = (const uint8_t*)data;
    uint64_t table_end = 0;
    if (!check_head(d, size, true, &table_end, err, errlen)) return false;
    const mof_header* h = (const mof_header*)d;
    const uint64_t fsize = h->file_size;
    const mof_chunk_entry* ce = (const mof_chunk_entry*)(d + h->table_offset);

    Slot slots[S_N];
    for (uint32_t i = 0; i < h->chunk_count; i++) {
        const mof_chunk_entry* e = &ce[i];
        int s = slot_of(e->type);
        if (s < 0) continue;  // forward compatibility: skip (check_head rejected unknown required chunks)
        slots[s].present = true;
        slots[s].p = d + e->offset;
        slots[s].size = e->size;
        slots[s].count = e->count;
    }

    mof_file f;
    MOF_MEMSET(&f, 0, sizeof f);
    f.data = d;
    f.size = (size_t)fsize;
    f.version_major = h->version_major;
    f.version_minor = h->version_minor;
    f.flags = h->flags;
    f.chunk_count = h->chunk_count;
    f.chunks = ce;

    // record chunks: size must equal count * record size exactly
    struct Rec { int slot; uint64_t rsize; uint32_t* count; const void** ptr; const char* what; };
    const void* meshes_p = nullptr; const void* subm_p = nullptr; const void* matl_p = nullptr;
    const void* text_p = nullptr; const void* skel_p = nullptr; const void* node_p = nullptr;
    const Rec recs[] = {
        {S_MESH, sizeof(mof_mesh), &f.mesh_count, &meshes_p, "MESH chunk size mismatch"},
        {S_SUBM, sizeof(mof_submesh), &f.submesh_count, &subm_p, "SUBM chunk size mismatch"},
        {S_MATL, sizeof(mof_material), &f.material_count, &matl_p, "MATL chunk size mismatch"},
        {S_TEXT, sizeof(mof_texture), &f.texture_count, &text_p, "TEXT chunk size mismatch"},
        {S_SKEL, sizeof(mof_bone), &f.bone_count, &skel_p, "SKEL chunk size mismatch"},
        {S_NODE, sizeof(mof_node), &f.node_count, &node_p, "NODE chunk size mismatch"},
    };
    for (const Rec& r : recs) {
        const Slot& s = slots[r.slot];
        if (!s.present) continue;
        if ((uint64_t)s.count * r.rsize != s.size) return fail(err, errlen, r.what);
        *r.count = s.count;
        *r.ptr = s.p;
    }
    f.meshes = (const mof_mesh*)meshes_p;
    f.submeshes = (const mof_submesh*)subm_p;
    f.materials = (const mof_material*)matl_p;
    f.textures = (const mof_texture*)text_p;
    f.bones = (const mof_bone*)skel_p;
    f.nodes = (const mof_node*)node_p;
    if (f.bone_count > MOF_MAX_BONES) return fail(err, errlen, "too many bones");

    if (slots[S_STRS].present) {
        const Slot& s = slots[S_STRS];
        if (s.size < 1 || s.size > 0xFFFFFFFFull) return fail(err, errlen, "bad string table size");
        if (s.count != s.size) return fail(err, errlen, "string table count mismatch");
        const char* c = (const char*)s.p;
        if (c[0] != 0 || c[s.size - 1] != 0) return fail(err, errlen, "string table must start and end with NUL");
        f.strings = c;
        f.strings_size = (uint32_t)s.size;
    }
    if (slots[S_VERT].present) { f.vertex_data = slots[S_VERT].p; f.vertex_data_size = slots[S_VERT].size; }
    if (slots[S_INDX].present) { f.index_data = slots[S_INDX].p; f.index_data_size = slots[S_INDX].size; }
    if (slots[S_TEXD].present) { f.texture_data = slots[S_TEXD].p; f.texture_data_size = slots[S_TEXD].size; }

    auto name_ok = [&](uint32_t off) { return off == 0 || off < f.strings_size; };

    // ---- nodes
    for (uint32_t i = 0; i < f.node_count; i++) {
        const mof_node& n = f.nodes[i];
        if (!name_ok(n.name)) return fail(err, errlen, "node name offset out of range", i);
        if (n.parent < -1 || (n.parent >= 0 && (uint32_t)n.parent >= i)) return fail(err, errlen, "node parent must precede node", i);
        if (n.mesh < -1 || (n.mesh >= 0 && (uint32_t)n.mesh >= f.mesh_count)) return fail(err, errlen, "node mesh index out of range", i);
        if ((n.flags & MOF_NODE_SKINNED) && f.bone_count == 0) return fail(err, errlen, "skinned node without skeleton", i);
        if (!all_finite(n.translation, 3) || !all_finite(n.rotation, 4) || !all_finite(n.scale, 3))
            return fail(err, errlen, "node transform not finite", i);
    }
    // ---- bones
    for (uint32_t i = 0; i < f.bone_count; i++) {
        const mof_bone& b = f.bones[i];
        if (!name_ok(b.name)) return fail(err, errlen, "bone name offset out of range", i);
        if (b.parent < -1 || (b.parent >= 0 && (uint32_t)b.parent >= i)) return fail(err, errlen, "bone parent must precede bone", i);
        if (b.node != MOF_NONE && b.node >= f.node_count) return fail(err, errlen, "bone node index out of range", i);
        if (!all_finite(b.translation, 3) || !all_finite(b.rotation, 4) || !all_finite(b.scale, 3) ||
            !all_finite(b.inverse_bind, 16))
            return fail(err, errlen, "bone transform not finite", i);
    }
    // ---- textures
    for (uint32_t i = 0; i < f.texture_count; i++) {
        const mof_texture& t = f.textures[i];
        if (!name_ok(t.name)) return fail(err, errlen, "texture name offset out of range", i);
        if (t.codec > MOF_TEX_JPEG) return fail(err, errlen, "unknown texture codec", i);
        if (t.wrap_s > MOF_WRAP_MIRROR || t.wrap_t > MOF_WRAP_MIRROR) return fail(err, errlen, "bad wrap mode", i);
        if (t.min_filter > MOF_FILTER_LINEAR || t.mag_filter > MOF_FILTER_LINEAR || t.mip_filter > MOF_MIP_LINEAR)
            return fail(err, errlen, "bad filter mode", i);
        if (t.data_offset % 16 != 0) return fail(err, errlen, "texture data offset not 16-aligned", i);
        if (t.data_offset > f.texture_data_size || t.data_size > f.texture_data_size - t.data_offset)
            return fail(err, errlen, "texture data out of range", i);
        const uint8_t* p = f.texture_data + t.data_offset;
        if (t.codec == MOF_TEX_RAW_RGBA8) {
            if (t.width < 1 || t.height < 1 || t.width > MOF_MAX_TEXTURE_DIM || t.height > MOF_MAX_TEXTURE_DIM)
                return fail(err, errlen, "bad raw texture size", i);
            uint32_t maxd = t.width > t.height ? t.width : t.height, maxmips = 1;
            while (maxd > 1) { maxd >>= 1; maxmips++; }
            if (t.mip_count < 1 || t.mip_count > maxmips || t.mip_count > MOF_MAX_MIPS)
                return fail(err, errlen, "bad mip count", i);
            uint64_t total = 0;
            for (uint32_t m = 0; m < t.mip_count; m++) {
                uint64_t mw = t.width >> m, mh = t.height >> m;
                if (mw < 1) mw = 1;
                if (mh < 1) mh = 1;
                total += mw * mh * 4;
            }
            if (total != t.data_size) return fail(err, errlen, "raw texture size mismatch", i);
        } else {
            if (t.mip_count != 1) return fail(err, errlen, "encoded texture mip_count must be 1", i);
            if (t.width > MOF_MAX_TEXTURE_DIM || t.height > MOF_MAX_TEXTURE_DIM) return fail(err, errlen, "bad texture size", i);
            if (t.codec == MOF_TEX_PNG) {
                static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
                if (t.data_size < 8 || MOF_MEMCMP(p, sig, 8) != 0) return fail(err, errlen, "texture is not a PNG", i);
            } else {
                if (t.data_size < 4 || p[0] != 0xFF || p[1] != 0xD8 || p[2] != 0xFF) return fail(err, errlen, "texture is not a JPEG", i);
            }
        }
    }
    // ---- materials
    for (uint32_t i = 0; i < f.material_count; i++) {
        const mof_material& m = f.materials[i];
        if (!name_ok(m.name)) return fail(err, errlen, "material name offset out of range", i);
        if (((m.flags & MOF_MATERIAL_ALPHA_MASK) >> MOF_MATERIAL_ALPHA_SHIFT) > MOF_ALPHA_BLEND)
            return fail(err, errlen, "bad alpha mode", i);
        for (uint32_t s = 0; s < MOF_SLOT_COUNT; s++) {
            if (m.tex[s].texture != MOF_NONE && m.tex[s].texture >= f.texture_count)
                return fail(err, errlen, "material texture index out of range", i);
            if (m.tex[s].uv_set > 1) return fail(err, errlen, "material uv set must be 0 or 1", i);
        }
        if (!finite(m.alpha_cutoff) || !finite(m.metallic) || !finite(m.roughness) || !finite(m.normal_scale) ||
            !finite(m.occlusion_strength) || !all_finite(m.base_color, 4) || !all_finite(m.emissive, 3))
            return fail(err, errlen, "material factor not finite", i);
    }
    // ---- meshes
    for (uint32_t i = 0; i < f.mesh_count; i++) {
        const mof_mesh& m = f.meshes[i];
        if (!name_ok(m.name)) return fail(err, errlen, "mesh name offset out of range", i);
        if (m.index_format > MOF_INDEX_U32) return fail(err, errlen, "bad index format", i);
        if (m.weight_format > MOF_WEIGHTS_UNORM16) return fail(err, errlen, "bad weight format", i);
        if (m.index_count % 3 != 0) return fail(err, errlen, "index count not a multiple of 3", i);
        const uint64_t vc = m.vertex_count;
        const uint64_t vb = f.vertex_data_size;
        if (!stream_ok(m.off_position, vc, 12, vb)) return fail(err, errlen, "position stream out of range", i);
        if ((m.stream_mask & MOF_STREAM_NORMAL) && !stream_ok(m.off_normal, vc, 12, vb)) return fail(err, errlen, "normal stream out of range", i);
        if ((m.stream_mask & MOF_STREAM_TANGENT) && !stream_ok(m.off_tangent, vc, 16, vb)) return fail(err, errlen, "tangent stream out of range", i);
        if ((m.stream_mask & MOF_STREAM_UV0) && !stream_ok(m.off_uv0, vc, 8, vb)) return fail(err, errlen, "uv0 stream out of range", i);
        if ((m.stream_mask & MOF_STREAM_UV1) && !stream_ok(m.off_uv1, vc, 8, vb)) return fail(err, errlen, "uv1 stream out of range", i);
        if ((m.stream_mask & MOF_STREAM_COLOR) && !stream_ok(m.off_color, vc, 4, vb)) return fail(err, errlen, "color stream out of range", i);
        if (m.stream_mask & MOF_STREAM_SKIN) {
            if (f.bone_count == 0) return fail(err, errlen, "skinned mesh without skeleton", i);
            if (!stream_ok(m.off_joints, vc, 8, vb)) return fail(err, errlen, "joint stream out of range", i);
            if (!stream_ok(m.off_weights, vc, m.weight_format == MOF_WEIGHTS_F32 ? 16 : 8, vb))
                return fail(err, errlen, "weight stream out of range", i);
            const uint16_t* j = (const uint16_t*)(f.vertex_data + m.off_joints);
            for (uint64_t k = 0; k < vc * 4; k++)
                if (j[k] >= f.bone_count) return fail(err, errlen, "joint index out of range", i);
        }
        const uint64_t isz = m.index_format == MOF_INDEX_U16 ? 2 : 4;
        if (!stream_ok(m.off_indices, m.index_count, isz, f.index_data_size)) return fail(err, errlen, "index data out of range", i);
        if (m.index_format == MOF_INDEX_U16) {
            const uint16_t* ix = (const uint16_t*)(f.index_data + m.off_indices);
            for (uint32_t k = 0; k < m.index_count; k++)
                if (ix[k] >= vc) return fail(err, errlen, "index value out of range", i);
        } else {
            const uint32_t* ix = (const uint32_t*)(f.index_data + m.off_indices);
            for (uint32_t k = 0; k < m.index_count; k++)
                if (ix[k] >= vc) return fail(err, errlen, "index value out of range", i);
        }
        if (m.first_submesh > f.submesh_count || m.submesh_count > f.submesh_count - m.first_submesh)
            return fail(err, errlen, "submesh range out of range", i);
        for (uint32_t k = 0; k < m.submesh_count; k++) {
            const mof_submesh& s = f.submeshes[m.first_submesh + k];
            if (s.first_index > m.index_count || s.index_count > m.index_count - s.first_index)
                return fail(err, errlen, "submesh index range out of range", i);
            if (s.index_count % 3 != 0) return fail(err, errlen, "submesh index count not a multiple of 3", i);
            if (s.material != MOF_NONE && s.material >= f.material_count) return fail(err, errlen, "submesh material out of range", i);
        }
        if (!all_finite(m.bounds_min, 3) || !all_finite(m.bounds_max, 3) || !all_finite(m.sphere_center, 3) ||
            !finite(m.sphere_radius) || m.sphere_radius < 0)
            return fail(err, errlen, "bounds not finite", i);
        for (int a = 0; a < 3; a++)
            if (m.bounds_min[a] > m.bounds_max[a]) return fail(err, errlen, "bounds min > max", i);
    }

    *out = f;
    return true;
}

const char* mof_string(const mof_file* f, uint32_t offset) {
    if (!f || !f->strings || offset >= f->strings_size) return "";
    return f->strings + offset;
}

static const float* fstream(const mof_file* f, const mof_mesh* m, uint32_t bit, uint32_t off) {
    if (bit && !(m->stream_mask & bit)) return nullptr;
    return (const float*)(f->vertex_data + off);
}
const float* mof_mesh_positions(const mof_file* f, const mof_mesh* m) { return fstream(f, m, 0, m->off_position); }
const float* mof_mesh_normals(const mof_file* f, const mof_mesh* m) { return fstream(f, m, MOF_STREAM_NORMAL, m->off_normal); }
const float* mof_mesh_tangents(const mof_file* f, const mof_mesh* m) { return fstream(f, m, MOF_STREAM_TANGENT, m->off_tangent); }
const float* mof_mesh_uv0(const mof_file* f, const mof_mesh* m) { return fstream(f, m, MOF_STREAM_UV0, m->off_uv0); }
const float* mof_mesh_uv1(const mof_file* f, const mof_mesh* m) { return fstream(f, m, MOF_STREAM_UV1, m->off_uv1); }
const uint8_t* mof_mesh_colors(const mof_file* f, const mof_mesh* m) {
    return (m->stream_mask & MOF_STREAM_COLOR) ? f->vertex_data + m->off_color : nullptr;
}
const uint16_t* mof_mesh_joints(const mof_file* f, const mof_mesh* m) {
    return (m->stream_mask & MOF_STREAM_SKIN) ? (const uint16_t*)(f->vertex_data + m->off_joints) : nullptr;
}
const void* mof_mesh_weights(const mof_file* f, const mof_mesh* m) {
    return (m->stream_mask & MOF_STREAM_SKIN) ? (const void*)(f->vertex_data + m->off_weights) : nullptr;
}
const void* mof_mesh_indices(const mof_file* f, const mof_mesh* m) { return f->index_data + m->off_indices; }
uint32_t mof_mesh_index(const mof_file* f, const mof_mesh* m, uint32_t i) {
    if (i >= m->index_count) return 0;
    if (m->index_format == MOF_INDEX_U16) return ((const uint16_t*)(f->index_data + m->off_indices))[i];
    return ((const uint32_t*)(f->index_data + m->off_indices))[i];
}
const uint8_t* mof_texture_bytes(const mof_file* f, uint32_t texture, uint64_t* size) {
    if (size) *size = 0;
    if (!f || texture >= f->texture_count) return nullptr;
    const mof_texture& t = f->textures[texture];
    if (size) *size = t.data_size;
    return f->texture_data + t.data_offset;
}

// ---------------------------------------------------------- partial access

uint64_t mof_head_size(const void* head, size_t size) {
    const uint8_t* d = (const uint8_t*)head;
    if (!d || (uintptr_t)d % 16 != 0 || size < sizeof(mof_header)) return 0;
    const mof_header* h = (const mof_header*)d;
    if (MOF_MEMCMP(h->magic, "MOF\0", 4) != 0 || h->version_major != MOF_VERSION_MAJOR) return 0;
    if (h->header_size < sizeof(mof_header) || h->header_size % 16 != 0) return 0;
    if (h->chunk_entry_size != sizeof(mof_chunk_entry) || h->chunk_count > MOF_MAX_CHUNKS) return 0;
    if (h->table_offset % 16 != 0 || h->table_offset < h->header_size) return 0;
    const uint64_t table_bytes = (uint64_t)h->chunk_count * sizeof(mof_chunk_entry);
    if (h->table_offset > h->file_size || table_bytes > h->file_size - h->table_offset) return 0;
    return h->table_offset + table_bytes;
}

bool mof_find_chunk(const void* head, size_t head_size, uint32_t type, mof_chunk_entry* out, char* err, size_t errlen) {
    if (err && errlen) err[0] = 0;
    if (!out) return fail(err, errlen, "null output");
    MOF_MEMSET(out, 0, sizeof *out);
    const uint8_t* d = (const uint8_t*)head;
    if (!check_head(d, head_size, false, nullptr, err, errlen)) return false;
    const mof_header* h = (const mof_header*)d;
    const mof_chunk_entry* ce = (const mof_chunk_entry*)(d + h->table_offset);
    for (uint32_t i = 0; i < h->chunk_count; i++)
        if (ce[i].type == type) { *out = ce[i]; return true; }
    return fail(err, errlen, "chunk not present");
}
