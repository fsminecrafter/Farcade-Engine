#include "terrain_bridge.h"
#include "model_cache.h"
#include "core/vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fe {

namespace {

double clock_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
}

uint64_t mix64(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h ^= h >> 31; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 29;
    return h;
}

uint64_t fnv(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

// ---------------------------------------------------------------- store

struct Entry {
    char path[kPathMax] = {};
    TerrainData* data = nullptr;
    bool dirty = false;
    bool failed = false;        // the file could not be read: not tried again until the store is cleared
};

Entry* g_entries = nullptr;
int g_entry_count = 0, g_entry_cap = 0;
uint64_t g_change = 1;
uint64_t g_serial = 0;

Entry* find_entry(const char* path) {
    for (int i = 0; i < g_entry_count; i++)
        if (!strcmp(g_entries[i].path, path)) return &g_entries[i];
    return nullptr;
}

Entry* add_entry(const char* path) {
    if (g_entry_count == g_entry_cap) {
        int nc = g_entry_cap ? g_entry_cap * 2 : 8;
        Entry* ne = (Entry*)realloc((void*)g_entries, sizeof(Entry) * (size_t)nc);
        if (!ne) return nullptr;
        g_entries = ne;
        g_entry_cap = nc;
    }
    Entry* e = &g_entries[g_entry_count++];
    *e = Entry();
    snprintf(e->path, sizeof e->path, "%s", path);
    return e;
}

}  // namespace

TerrainData* terrain_data(Object& o) {
    if (o.kind != ObjectKind::Terrain) return nullptr;
    Terrain& ts = o.modules.terrain;
    if (!ts.data[0]) return nullptr;
    Entry* e = find_entry(ts.data);
    if (!e) {
        e = add_entry(ts.data);
        if (!e) return nullptr;
        size_t size = 0;
        void* bytes = vfs::read(ts.data, &size);
        TerrainData* d = new TerrainData();
        d->serial = ++g_serial;
        if (!bytes || !d->load(bytes, size)) { e->failed = true; delete d; d = nullptr; }
        free(bytes);
        e->data = d;
        g_change++;
    }
    if (!e->data) return nullptr;
    if (e->data->revision != ts.revision && !e->data->in_stroke()) {
        if (e->data->goto_revision(ts.revision)) { e->dirty = true; g_change++; }
    }
    return e->data->valid() ? e->data : nullptr;
}

TerrainData* terrain_new_data(const char* path) {
    if (!path || !path[0]) return nullptr;
    Entry* e = find_entry(path);
    if (!e) e = add_entry(path);
    if (!e) return nullptr;
    if (!e->data) { e->data = new TerrainData(); e->data->serial = ++g_serial; }
    e->failed = false;
    e->dirty = true;
    g_change++;
    return e->data;
}

bool terrain_copy_data(const char* from, const char* to) {
    Entry* src = find_entry(from);
    TerrainData* sd = src ? src->data : nullptr;
    size_t size = 0;
    uint8_t* blob = nullptr;
    if (sd && sd->valid()) blob = sd->save(&size);
    else blob = (uint8_t*)vfs::read(from, &size);
    if (!blob) return false;
    TerrainData* d = terrain_new_data(to);
    bool ok = d && d->load(blob, size);
    free(blob);
    return ok;
}

void terrain_mark_dirty(TerrainData* t) {
    for (int i = 0; i < g_entry_count; i++)
        if (g_entries[i].data == t) g_entries[i].dirty = true;
    g_change++;
}

bool terrain_store_unsaved() {
    for (int i = 0; i < g_entry_count; i++)
        if (g_entries[i].dirty && g_entries[i].data && g_entries[i].data->valid()) return true;
    return false;
}

bool terrain_store_save(Scene& sc, bool (*write)(const char* path, const void* data, size_t size, void* user), void* user) {
    bool ok = true;
    for (int i = 0; i < g_entry_count; i++) {
        Entry& e = g_entries[i];
        if (!e.dirty || !e.data || !e.data->valid()) continue;
        bool used = false;
        for (int k = 0; k < sc.count() && !used; k++) {
            const Object& o = sc.at(k);
            used = o.kind == ObjectKind::Terrain && !strcmp(o.modules.terrain.data, e.path);
        }
        if (!used) continue;
        size_t size = 0;
        uint8_t* blob = e.data->save(&size);
        if (blob && write(e.path, blob, size, user)) e.dirty = false;
        else ok = false;
        free(blob);
    }
    return ok;
}

void terrain_store_clear() {
    for (int i = 0; i < g_entry_count; i++) delete g_entries[i].data;
    free((void*)g_entries);
    g_entries = nullptr;
    g_entry_count = g_entry_cap = 0;
    g_change++;
}

uint64_t terrain_store_change() { return g_change; }
void terrain_store_touch() { g_change++; }

void terrain_default_path(const Object& o, char* out, size_t cap) {
    char name[64];
    int n = 0;
    for (const char* s = o.name; *s && n < 40; s++) {
        char c = *s;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        name[n++] = ok ? c : '_';
    }
    name[n] = 0;
    snprintf(out, cap, "terrain/%s_%u.fterrain", n ? name : "Terrain", (unsigned)o.id);
}

Vec3 terrain_origin(Scene& sc, const Object& o) {
    Mat4 w = sc.world_matrix(o);
    return Vec3(w.m[12], w.m[13], w.m[14]);
}

// ---------------------------------------------------------------- view caches

namespace {

constexpr int kMaxParts = 4;                // parts drawn of each foliage model
constexpr int kMaxLayerModels = 64;         // models in one foliage layer's list
constexpr int kMaxInstances = 65536;        // per chunk and layer
constexpr uint32_t kMaxBatchVertices = 1u << 21;

struct ChunkMesh {
    int lod = -1;
    uint32_t version = 0;
    uint32_t n = 0;
    Vec3* pos = nullptr;
    Vec3* nrm = nullptr;
    float* uv = nullptr;
    uint32_t occ_version = 0;
    uint32_t occ_n = 0;
    Vec3* occ = nullptr;
    size_t bytes = 0;
    void release() { free(pos); free(nrm); free(uv); free(occ); *this = ChunkMesh(); }
};

struct FoliagePart {
    Vec3* pos = nullptr;
    Vec3* nrm = nullptr;
    float* uv = nullptr;
    uint32_t per = 0;                       // vertices per instance
    char texture[320] = {};
    Vec3 color = Vec3(1, 1, 1);
};

// One model's instances in a block, and the geometry of a prefix of them.
struct FoliageGroup {
    int model = 0;                          // index into the layer's loaded models
    FoliageInstance* inst = nullptr;
    int total = 0;
    int built = 0;                          // instances in the geometry (a prefix)
    FoliagePart parts[kMaxParts];
    int part_count = 0;
    Vec3 lo, hi;                            // terrain local bounds of the built instances
    size_t bytes = 0;
    void free_geometry() {
        for (int p = 0; p < part_count; p++) { free(parts[p].pos); free(parts[p].nrm); free(parts[p].uv); parts[p] = FoliagePart(); }
        part_count = 0;
        built = 0;
        bytes = inst ? sizeof(FoliageInstance) * (size_t)total : 0;
    }
    void release() { free_geometry(); free(inst); *this = FoliageGroup(); }
};

// A block of a chunk's layer: its instances split by model (each instance picks one of the layer's models).
struct FoliageBatch {
    uint32_t version = 0;                   // the chunk's foliage version it was scattered for
    uint64_t settings = 0;                  // hash of the layer's settings and models
    bool scattered = false;
    FoliageGroup* groups = nullptr;
    int group_count = 0;
    uint64_t last_used = 0;
    size_t bytes = 0;
    void sum() {
        bytes = 0;
        for (int g = 0; g < group_count; g++) bytes += groups[g].bytes;
    }
    void release() {
        for (int g = 0; g < group_count; g++) groups[g].release();
        free(groups);
        *this = FoliageBatch();
    }
};

uint64_t layer_hash(const FoliageLayer& fl, const Model* const* ms, int count) {
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, fl.model, strlen(fl.model));
    h = fnv(h, &fl.density, sizeof fl.density);
    h = fnv(h, &fl.scale_min, sizeof fl.scale_min);
    h = fnv(h, &fl.scale_max, sizeof fl.scale_max);
    h = fnv(h, &fl.max_slope, sizeof fl.max_slope);
    h = fnv(h, &fl.align_to_slope, sizeof fl.align_to_slope);
    for (int i = 0; i < count; i++) {
        h = mix64(h, (uint64_t)(uintptr_t)ms[i]);
        h = mix64(h, (uint64_t)ms[i]->part_count);
    }
    return h;
}

// Share of a layer's instances drawn at distance d (meters): all of them up to 40% of the layer's
// distance, then fewer and fewer, none at the distance.
float foliage_share(float d, float dist) {
    if (!(dist > 0) || d >= dist) return 0.0f;
    float t = d / dist;
    if (t <= 0.4f) return 1.0f;
    float u = (1.0f - t) / 0.6f;
    return u * u;
}

float box_distance(const Vec3& p, const Vec3& lo, const Vec3& hi) {
    float dx = p.x < lo.x ? lo.x - p.x : (p.x > hi.x ? p.x - hi.x : 0.0f);
    float dy = p.y < lo.y ? lo.y - p.y : (p.y > hi.y ? p.y - hi.y : 0.0f);
    float dz = p.z < lo.z ? lo.z - p.z : (p.z > hi.z ? p.z - hi.z : 0.0f);
    return fsqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

struct TerrainView::Cache {
    ObjectId id = kNoObject;
    TerrainData* data = nullptr;
    uint64_t serial = 0;
    int chunks = 0;                         // chunk count the arrays are for
    int cx = 0, cz = 0, cells = 0;
    ChunkMesh* mesh = nullptr;
    FoliageBatch* batch = nullptr;          // chunks * kFoliageLayers * sub * sub
    int sub = 1;                            // foliage blocks per chunk side (about 16 m each)
    int batches() const { return chunks * kFoliageLayers * sub * sub; }
    uint8_t* loaded = nullptr;
    bool streaming = false;
    Vec3 origin;
    uint64_t seen = 0;                      // frame it was last updated
    void release() {
        for (int i = 0; i < chunks; i++) mesh[i].release();
        for (int i = 0; i < batches(); i++) batch[i].release();
        free(mesh); free(batch); free(loaded);
        mesh = nullptr; batch = nullptr; loaded = nullptr;
        chunks = 0;
    }
};

TerrainView::~TerrainView() {
    clear();
    free(caches_);
    free(items_);
    free(item_obj_);
}

void TerrainView::clear() {
    for (int i = 0; i < cache_count_; i++) caches_[i].release();
    cache_count_ = 0;
    item_count_ = 0;
}

gfx::DrawItem* TerrainView::push(ObjectId id) {
    if (item_count_ == item_cap_) {
        int nc = item_cap_ ? item_cap_ * 2 : 256;
        gfx::DrawItem* ni = (gfx::DrawItem*)realloc((void*)items_, sizeof(gfx::DrawItem) * (size_t)nc);
        if (ni) items_ = ni;
        ObjectId* no = (ObjectId*)realloc(item_obj_, sizeof(ObjectId) * (size_t)nc);
        if (no) item_obj_ = no;
        if (!ni || !no) return nullptr;
        item_cap_ = nc;
    }
    item_obj_[item_count_] = id;
    gfx::DrawItem* d = &items_[item_count_++];
    *d = gfx::DrawItem();
    return d;
}

void TerrainView::evict(size_t budget) {
    for (;;) {
        size_t total = 0;
        FoliageBatch* oldest = nullptr;
        for (int c = 0; c < cache_count_; c++) {
            Cache& k = caches_[c];
            for (int i = 0; i < k.chunks; i++) total += k.mesh[i].bytes;
            for (int i = 0; i < k.batches(); i++) {
                FoliageBatch& b = k.batch[i];
                total += b.bytes;
                if (b.bytes && b.last_used != frame_ && (!oldest || b.last_used < oldest->last_used)) oldest = &b;
            }
        }
        stats_.memory = total;
        if (total <= budget || !oldest) return;
        oldest->release();
    }
}

void TerrainView::update(Scene& sc, const Vec3& eye, bool playing) {
    frame_++;
    playing_ = playing;
    item_count_ = 0;
    stats_ = TerrainViewStats();
    double t_start = clock_ms();
    bool budget_left = true;
    for (int i = 0; i < sc.count(); i++) {
        Object& o = sc.at(i);
        if (o.kind != ObjectKind::Terrain || !sc.visible_in_tree(o)) continue;
        TerrainData* td = terrain_data(o);
        if (!td) continue;
        const Terrain& ts = o.modules.terrain;
        // The cache of this object (made, or reset when its data changed shape).
        Cache* k = nullptr;
        for (int c = 0; c < cache_count_; c++)
            if (caches_[c].id == o.id) k = &caches_[c];
        if (!k) {
            if (cache_count_ == cache_cap_) {
                int nc = cache_cap_ ? cache_cap_ * 2 : 4;
                Cache* ncache = (Cache*)realloc((void*)caches_, sizeof(Cache) * (size_t)nc);
                if (!ncache) continue;
                caches_ = ncache;
                cache_cap_ = nc;
            }
            k = &caches_[cache_count_++];
            *k = Cache();
            k->id = o.id;
        }
        if (k->data != td || k->serial != td->serial || k->chunks != td->chunk_count() || k->cx != td->chunks_x || k->cells != td->cells ||
            k->sub != (int)fclamp(td->chunk_size / 16.0f + 0.5f, 1.0f, 4.0f)) {
            k->release();
            k->data = td;
            k->serial = td->serial;
            k->chunks = td->chunk_count();
            k->cx = td->chunks_x;
            k->cz = td->chunks_z;
            k->cells = td->cells;
            k->sub = (int)(td->chunk_size / 16.0f + 0.5f);
            if (k->sub < 1) k->sub = 1;
            if (k->sub > 4) k->sub = 4;
            k->mesh = (ChunkMesh*)calloc((size_t)k->chunks, sizeof(ChunkMesh));
            k->batch = (FoliageBatch*)calloc((size_t)k->batches(), sizeof(FoliageBatch));
            k->loaded = (uint8_t*)calloc((size_t)k->chunks, 1);
            if (!k->mesh || !k->batch || !k->loaded) { k->release(); k->data = nullptr; continue; }
            for (int c = 0; c < k->chunks; c++) k->mesh[c] = ChunkMesh();
            for (int c = 0; c < k->batches(); c++) k->batch[c] = FoliageBatch();
        }
        k->seen = frame_;
        k->origin = terrain_origin(sc, o);
        k->streaming = playing || ts.stream_in_editor;
        stats_.terrains++;
        stats_.chunks += k->chunks;
        Mat4 model = Mat4::translation(k->origin);
        Vec3 local_eye = eye - k->origin;
        float lod_d = ts.lod_distance > 1.0f ? ts.lod_distance : 1.0f;
        // Foliage models of the layers: each layer's list, the ones that load (an instance picks one of them).
        const Model* models[kFoliageLayers][kMaxLayerModels] = {};
        int model_count[kFoliageLayers] = {};
        uint64_t lhash[kFoliageLayers] = {};
        for (int l = 0; l < kFoliageLayers; l++) {
            const FoliageLayer& fl = ts.foliage[l];
            if (!fl.model[0] || !(fl.density > 0)) continue;
            char one[kPathMax];
            for (const char* at = path_list_next(fl.model, one, kPathMax); at && model_count[l] < kMaxLayerModels;
                 at = path_list_next(at, one, kPathMax)) {
                const Model* m = model_load(one);
                if (m && m->part_count > 0) models[l][model_count[l]++] = m;
            }
            lhash[l] = layer_hash(fl, models[l], model_count[l]);
        }
        for (int c = 0; c < k->chunks; c++) {
            int cx = c % td->chunks_x, cz = c / td->chunks_x;
            Vec3 lo, hi;
            td->chunk_bounds(cx, cz, &lo, &hi);
            // Streaming: the horizontal distance from the camera to the chunk.
            float dx = local_eye.x < lo.x ? lo.x - local_eye.x : (local_eye.x > hi.x ? local_eye.x - hi.x : 0.0f);
            float dz = local_eye.z < lo.z ? lo.z - local_eye.z : (local_eye.z > hi.z ? local_eye.z - hi.z : 0.0f);
            bool load = !k->streaming || dx * dx + dz * dz <= ts.load_distance * ts.load_distance;
            k->loaded[c] = load ? 1 : 0;
            if (!load) {
                stats_.chunks_unloaded++;
                k->mesh[c].release();
                for (int i = 0; i < kFoliageLayers * k->sub * k->sub; i++) k->batch[c * kFoliageLayers * k->sub * k->sub + i].release();
                continue;
            }
            float d = box_distance(local_eye, lo, hi);
            int lod = 0;
            while (lod < td->max_lod() && d > lod_d * (float)(1 << lod)) lod++;
            ChunkMesh& cm = k->mesh[c];
            uint32_t ver = td->chunk_info[c].version;
            if (cm.lod != lod || cm.version != ver || !cm.pos) {
                uint32_t cap = TerrainData::mesh_capacity(td->cells);
                if (!cm.pos) {
                    cm.pos = (Vec3*)malloc(sizeof(Vec3) * cap);
                    cm.nrm = (Vec3*)malloc(sizeof(Vec3) * cap);
                    cm.uv = (float*)malloc(sizeof(float) * 2 * cap);
                    cm.bytes = (sizeof(Vec3) * 2 + sizeof(float) * 2) * cap;
                }
                if (!cm.pos || !cm.nrm || !cm.uv) { cm.release(); continue; }
                cm.n = td->build_chunk(cx, cz, lod, ts.uv_scale, cm.pos, cm.nrm, cm.uv);
                cm.lod = lod;
                cm.version = ver;
            }
            if (cm.occ_version != ver || !cm.occ) {
                if (!cm.occ) {
                    cm.occ = (Vec3*)malloc(sizeof(Vec3) * TerrainData::occluder_capacity());
                    cm.bytes += sizeof(Vec3) * TerrainData::occluder_capacity();
                }
                cm.occ_n = cm.occ ? td->build_occluder(cx, cz, cm.occ) : 0;
                cm.occ_version = ver;
            }
            if (cm.n) {
                gfx::DrawItem* it = push(o.id);
                if (it) {
                    it->mesh.positions = cm.pos;
                    it->mesh.normals = cm.nrm;
                    it->mesh.uvs = cm.uv;
                    it->mesh.vertex_count = cm.n;
                    it->mesh.key = mix64(mix64(mix64(mix64(td->serial, (uint64_t)c), (uint64_t)lod), ver), (uint64_t)td->cells) | 1ull;
                    it->mesh.has_bounds = true;
                    it->mesh.bounds_min = lo;
                    it->mesh.bounds_max = hi;
                    it->mesh.occluder_positions = cm.occ;
                    it->mesh.occluder_count = cm.occ_n;
                    it->model = model;
                    it->texture_path = ts.texture[0] ? ts.texture : nullptr;
                    it->color = ts.color;
                    it->roughness = ts.roughness;
                    it->cast_shadows = ts.cast_shadows;
                    stats_.chunks_drawn++;
                    stats_.lod_chunks[lod]++;
                }
            }
            // ---- foliage of this chunk: scattered once per chunk and layer, split into sub x sub blocks that
            // each thin out by their own distance (finer steps than whole chunks), and in each block by model.
            const int S = k->sub, S2 = S * S;
            const float bsize = td->chunk_size / (float)S;
            for (int l = 0; l < kFoliageLayers; l++) {
                FoliageBatch* subs = &k->batch[(c * kFoliageLayers + l) * S2];
                const FoliageLayer& fl = ts.foliage[l];
                const int M = model_count[l];
                if (M <= 0) {
                    for (int q = 0; q < S2; q++) if (subs[q].bytes || subs[q].scattered) subs[q].release();
                    continue;
                }
                if (foliage_share(d, fl.distance) <= 0) continue;         // kept (it may come back), not drawn
                uint32_t fver = td->chunk_info[c].foliage_version;
                if (subs[0].scattered && (subs[0].version != fver || subs[0].settings != lhash[l]))
                    for (int q = 0; q < S2; q++) subs[q].release();
                if (!subs[0].scattered) {
                    if (!budget_left) { stats_.pending = true; continue; }
                    FoliageInstance* all = nullptr;
                    int total = 0;
                    if (td->has_foliage(cx, cz, l)) {
                        int bound = td->scatter_bound(cx, cz, l, fl);
                        if (bound > kMaxInstances) bound = kMaxInstances;
                        all = (FoliageInstance*)malloc(sizeof(FoliageInstance) * (size_t)(bound > 0 ? bound : 1));
                        total = all ? td->scatter(cx, cz, l, fl, all, bound) : 0;
                    }
                    // Into the blocks and, in each, by model, keeping the priority order.
                    auto block_of = [&](const FoliageInstance& fi) {
                        int bx = (int)((fi.pos.x - lo.x) / bsize), bz = (int)((fi.pos.z - lo.z) / bsize);
                        bx = bx < 0 ? 0 : (bx >= S ? S - 1 : bx);
                        bz = bz < 0 ? 0 : (bz >= S ? S - 1 : bz);
                        return bz * S + bx;
                    };
                    int* counts = (int*)calloc((size_t)S2 * (size_t)M, sizeof(int));
                    for (int i = 0; i < total && counts; i++) counts[block_of(all[i]) * M + (int)(all[i].pick % (uint32_t)M)]++;
                    for (int q = 0; q < S2; q++) {
                        FoliageBatch& b = subs[q];
                        b.scattered = true;
                        b.version = fver;
                        b.settings = lhash[l];
                        int used = 0;
                        for (int g = 0; g < M && counts; g++) used += counts[q * M + g] > 0;
                        b.groups = used ? (FoliageGroup*)calloc((size_t)used, sizeof(FoliageGroup)) : nullptr;
                        b.group_count = 0;
                        for (int g = 0; g < M && b.groups; g++) {
                            int n = counts[q * M + g];
                            if (!n) continue;
                            FoliageGroup& gr = b.groups[b.group_count++];
                            gr = FoliageGroup();
                            gr.model = g;
                            gr.inst = (FoliageInstance*)malloc(sizeof(FoliageInstance) * (size_t)n);
                            if (!gr.inst) { b.group_count--; continue; }
                        }
                    }
                    for (int i = 0; i < total && counts; i++) {
                        FoliageBatch& b = subs[block_of(all[i])];
                        int g = (int)(all[i].pick % (uint32_t)M);
                        for (int k2 = 0; k2 < b.group_count; k2++)
                            if (b.groups[k2].model == g) { b.groups[k2].inst[b.groups[k2].total++] = all[i]; break; }
                    }
                    for (int q = 0; q < S2; q++) {
                        for (int g = 0; g < subs[q].group_count; g++) subs[q].groups[g].bytes = sizeof(FoliageInstance) * (size_t)subs[q].groups[g].total;
                        subs[q].sum();
                    }
                    free(counts);
                    free(all);
                    if (clock_ms() - t_start > build_budget_ms) budget_left = false;
                }
                float smax = fl.scale_max > fl.scale_min ? fl.scale_max : fl.scale_min;
                for (int q = 0; q < S2; q++) {
                    FoliageBatch& b = subs[q];
                    if (!b.group_count) continue;
                    Vec3 qlo(lo.x + (float)(q % S) * bsize, lo.y, lo.z + (float)(q / S) * bsize);
                    Vec3 qhi(qlo.x + bsize, hi.y, qlo.z + bsize);
                    float share = foliage_share(box_distance(local_eye, qlo, qhi), fl.distance);
                    if (share <= 0) continue;
                    bool drawn = false;
                    for (int gi = 0; gi < b.group_count; gi++) {
                        FoliageGroup& gr = b.groups[gi];
                        const Model* m = models[l][gr.model];
                        if (gr.total <= 0 || !m) continue;
                        int need = (int)((float)gr.total * share + 0.999f);
                        if (need < 1) need = 1;
                        if (need > gr.total) need = gr.total;
                        // Geometry for a power-of-two share of the instances that covers `need`.
                        if (need > gr.built || (gr.built > 4 * need && gr.built > 256)) {
                            if (!budget_left) { stats_.pending = true; if (gr.built <= 0) continue; }
                            else {
                                int target = gr.total;
                                while (target / 2 >= need && target / 2 >= 64) target /= 2;
                                uint32_t verts = 0;
                                int parts = m->part_count < kMaxParts ? m->part_count : kMaxParts;
                                for (int p = 0; p < parts; p++) verts += m->parts[p].vertex_count;
                                if (verts == 0) continue;
                                if ((uint64_t)target * verts > kMaxBatchVertices) target = (int)(kMaxBatchVertices / verts);
                                if (target < 1) continue;
                                gr.free_geometry();
                                Vec3 mlo = m->mesh.bounds_min, mhi = m->mesh.bounds_max;
                                float r = fmaxf_(fmaxf_(fabsf_(mlo.x), fabsf_(mhi.x)), fmaxf_(fabsf_(mlo.z), fabsf_(mhi.z))) * 1.4143f;
                                if (fl.align_to_slope) r = fmaxf_(r, fmaxf_(fabsf_(mlo.y), fabsf_(mhi.y)));
                                Vec3 blo(1e30f, 1e30f, 1e30f), bhi(-1e30f, -1e30f, -1e30f);
                                for (int i = 0; i < target; i++) { blo = vmin(blo, gr.inst[i].pos); bhi = vmax(bhi, gr.inst[i].pos); }
                                gr.lo = Vec3(blo.x - r * smax, blo.y + fminf_(mlo.y, fl.align_to_slope ? -r : 0.0f) * smax, blo.z - r * smax);
                                gr.hi = Vec3(bhi.x + r * smax, bhi.y + fmaxf_(mhi.y, fl.align_to_slope ? r : 0.0f) * smax, bhi.z + r * smax);
                                bool ok = true;
                                for (int p = 0; p < parts && ok; p++) {
                                    const ModelPart& mp = m->parts[p];
                                    FoliagePart& fp = gr.parts[gr.part_count];
                                    uint32_t pv = mp.vertex_count / 3 * 3;
                                    size_t nv = (size_t)pv * (size_t)target;
                                    if (!pv) continue;
                                    fp.pos = (Vec3*)malloc(sizeof(Vec3) * nv);
                                    fp.nrm = (Vec3*)malloc(sizeof(Vec3) * nv);
                                    fp.uv = mp.uvs ? (float*)malloc(sizeof(float) * 2 * nv) : nullptr;
                                    if (!fp.pos || !fp.nrm || (mp.uvs && !fp.uv)) { free(fp.pos); free(fp.nrm); free(fp.uv); fp = FoliagePart(); ok = false; break; }
                                    fp.per = pv;
                                    snprintf(fp.texture, sizeof fp.texture, "%s", mp.texture);
                                    fp.color = mp.color;
                                    for (int i = 0; i < target; i++) {
                                        const FoliageInstance& fi = gr.inst[i];
                                        // Frame: up along fi.up, turned by yaw around it, scaled.
                                        Vec3 u = fi.up;
                                        Vec3 ref = fabsf_(u.z) < 0.9f ? Vec3(0, 0, 1) : Vec3(1, 0, 0);
                                        Vec3 bx = normalize(cross(u, ref));
                                        Vec3 bz = cross(bx, u);
                                        float cy = fcos(fi.yaw), sy = fsin(fi.yaw);
                                        Vec3 X = bx * cy - bz * sy, Z = bx * sy + bz * cy;
                                        Vec3* op = fp.pos + (size_t)i * pv;
                                        Vec3* on = fp.nrm + (size_t)i * pv;
                                        for (uint32_t v = 0; v < pv; v++) {
                                            const Vec3& sp = mp.positions[v];
                                            op[v] = fi.pos + (X * sp.x + u * sp.y + Z * sp.z) * fi.scale;
                                            Vec3 sn = mp.normals ? mp.normals[v] : Vec3(0, 1, 0);
                                            on[v] = X * sn.x + u * sn.y + Z * sn.z;
                                        }
                                        if (fp.uv) memcpy(fp.uv + (size_t)i * pv * 2, mp.uvs, sizeof(float) * 2 * pv);
                                    }
                                    gr.bytes += nv * (sizeof(Vec3) * 2 + (fp.uv ? sizeof(float) * 2 : 0));
                                    gr.part_count++;
                                }
                                if (!ok) { gr.free_geometry(); continue; }
                                gr.built = target;
                                b.sum();
                                stats_.foliage_built++;
                                if (clock_ms() - t_start > build_budget_ms) budget_left = false;
                            }
                        }
                        if (gr.built <= 0) continue;
                        int draw = need < gr.built ? need : gr.built;
                        drawn = true;
                        uint64_t bid = (uint64_t)((((c * kFoliageLayers + l) * S2) + q) * 64 + gr.model);
                        for (int p = 0; p < gr.part_count; p++) {
                            FoliagePart& fp = gr.parts[p];
                            gfx::DrawItem* it = push(o.id);
                            if (!it) break;
                            it->mesh.positions = fp.pos;
                            it->mesh.normals = fp.nrm;
                            it->mesh.uvs = fp.uv;
                            it->mesh.vertex_count = fp.per * (uint32_t)gr.built;
                            it->mesh.draw_count = fp.per * (uint32_t)draw;
                            it->mesh.key = mix64(mix64(mix64(mix64(td->serial ^ 0xF011A6Eull, bid), (uint64_t)p), b.settings ^ fver), (uint64_t)gr.built) | 1ull;
                            it->mesh.has_bounds = true;
                            it->mesh.bounds_min = gr.lo;
                            it->mesh.bounds_max = gr.hi;
                            it->model = model;
                            it->texture_path = fp.texture[0] ? fp.texture : nullptr;
                            it->color = fp.color;
                            it->roughness = 0.8f;
                            it->cast_shadows = fl.cast_shadows;
                            it->occluder = false;
                            it->foliage = true;
                            stats_.foliage_batches++;
                        }
                        stats_.foliage_instances += draw;
                    }
                    if (drawn) b.last_used = frame_;
                }
            }
        }
    }
    // Terrains that are gone (or hidden): their caches go too.
    for (int c = 0; c < cache_count_;) {
        if (caches_[c].seen != frame_) { caches_[c].release(); caches_[c] = caches_[--cache_count_]; }
        else c++;
    }
    evict(memory_budget);
}

bool TerrainView::loaded(Scene& sc, const Object& o) const {
    if (o.kind == ObjectKind::Terrain || o.kind == ObjectKind::Sky || o.kind == ObjectKind::SkyLight ||
        o.kind == ObjectKind::PostProcess || o.kind == ObjectKind::Clouds) return true;
    switch (o.streaming) {
        case StreamMode::AlwaysLoaded: return true;
        case StreamMode::Unloaded: return !playing_ || o.stream_loaded;
        default: break;
    }
    if (cache_count_ == 0) return true;
    Mat4 w = sc.world_matrix(o);
    Vec3 p(w.m[12], w.m[13], w.m[14]);
    for (int c = 0; c < cache_count_; c++) {
        const Cache& k = caches_[c];
        if (!k.streaming || !k.data || !k.loaded) continue;
        float lx = p.x - k.origin.x, lz = p.z - k.origin.z;
        if (lx < 0 || lz < 0 || lx >= k.data->size_x() || lz >= k.data->size_z()) continue;
        int cx = (int)(lx / k.data->chunk_size), cz = (int)(lz / k.data->chunk_size);
        if (cx >= k.data->chunks_x) cx = k.data->chunks_x - 1;
        if (cz >= k.data->chunks_z) cz = k.data->chunks_z - 1;
        return k.loaded[cz * k.data->chunks_x + cx] != 0;
    }
    return true;
}

}  // namespace fe
