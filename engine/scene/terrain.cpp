#include "scene/terrain.h"

#include <stdlib.h>
#include <string.h>

namespace fe {

namespace {

constexpr size_t kHistoryBytes = 96u << 20;         // undo memory at most
constexpr int kMaxCandidates = 64;                  // foliage candidates per cell at most
constexpr int kOccluderBlocks = 8;                  // occluder quads per chunk side
constexpr uint32_t kFileMagic = 0x52544546u;        // "FETR"
constexpr uint32_t kFileVersion = 1;

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
int floori(float v) { int i = (int)v; return (float)i > v ? i - 1 : i; }
int ceili(float v) { int i = (int)v; return (float)i < v ? i + 1 : i; }
int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
uint32_t hash4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return hash32(a ^ hash32(b ^ hash32(c ^ hash32(d + 0x9e3779b9u))));
}
float unit(uint32_t h) { return (float)(h >> 8) * (1.0f / 16777216.0f); }   // [0, 1)

float brush_weight(float d, float r, float hard) {
    if (d >= r) return 0.0f;
    float u = d / r;
    if (hard >= 0.999f || u <= hard) return 1.0f;
    float t = (1.0f - u) / (1.0f - hard);
    return t * t * (3.0f - 2.0f * t);
}

// Heap sort of 64 bit keys (no standard library).
void sort64(uint64_t* a, int n) {
    auto sift = [&](int i, int m) {
        for (;;) {
            int c = 2 * i + 1;
            if (c >= m) return;
            if (c + 1 < m && a[c + 1] > a[c]) c++;
            if (a[i] >= a[c]) return;
            uint64_t t = a[i]; a[i] = a[c]; a[c] = t;
            i = c;
        }
    };
    for (int i = n / 2 - 1; i >= 0; i--) sift(i, n);
    for (int m = n - 1; m > 0; m--) {
        uint64_t t = a[0]; a[0] = a[m]; a[m] = t;
        sift(0, m);
    }
}

// ---- byte writer / reader for the file
struct Writer {
    uint8_t* b = nullptr;
    size_t n = 0, cap = 0;
    bool fail = false;
    void put(const void* p, size_t k) {
        if (fail) return;
        if (n + k > cap) {
            size_t nc = cap ? cap * 2 : 65536;
            while (nc < n + k) nc *= 2;
            uint8_t* nb = (uint8_t*)realloc(b, nc);
            if (!nb) { fail = true; return; }
            b = nb;
            cap = nc;
        }
        memcpy(b + n, p, k);
        n += k;
    }
    void u32(uint32_t v) { put(&v, 4); }
    void f32(float v) { put(&v, 4); }
    void byte(uint8_t v) { put(&v, 1); }
    void varint(uint32_t v) {
        while (v >= 0x80) { byte((uint8_t)(v | 0x80)); v >>= 7; }
        byte((uint8_t)v);
    }
    // Runs of equal bytes: value, length.
    void rle(const uint8_t* p, size_t count) {
        size_t i = 0;
        while (i < count) {
            size_t j = i + 1;
            while (j < count && p[j] == p[i] && j - i < 0x0FFFFFFF) j++;
            byte(p[i]);
            varint((uint32_t)(j - i));
            i = j;
        }
    }
};

struct Reader {
    const uint8_t* p;
    size_t n, at = 0;
    bool fail = false;
    bool get(void* out, size_t k) {
        if (fail || at + k > n) { fail = true; return false; }
        memcpy(out, p + at, k);
        at += k;
        return true;
    }
    uint32_t u32() { uint32_t v = 0; get(&v, 4); return v; }
    float f32() { float v = 0; get(&v, 4); return v; }
    uint8_t byte() { uint8_t v = 0; get(&v, 1); return v; }
    uint32_t varint() {
        uint32_t v = 0;
        for (int s = 0; s < 35; s += 7) {
            uint8_t c = byte();
            if (fail) return 0;
            v |= (uint32_t)(c & 0x7F) << s;
            if (!(c & 0x80)) return v;
        }
        fail = true;
        return 0;
    }
    bool unrle(uint8_t* out, size_t count) {
        size_t i = 0;
        while (i < count) {
            uint8_t v = byte();
            uint32_t len = varint();
            if (fail || len == 0 || i + len > count) { fail = true; return false; }
            memset(out + i, v, len);
            i += len;
        }
        return true;
    }
};

}  // namespace

// ---------------------------------------------------------------- history

struct Step {
    int from = 0, to = 0;
    bool full = false;
    // Full steps (Generate): the whole file before and after (before may be null: no data).
    uint8_t* before = nullptr;
    size_t before_n = 0;
    uint8_t* after = nullptr;
    size_t after_n = 0;
    // Stroke steps: the touched chunks, a block of each before and after.
    int n = 0;
    int* chunk = nullptr;
    uint8_t* before_blocks = nullptr;
    uint8_t* after_blocks = nullptr;
    size_t mem = 0;                     // bytes held
};

struct TerrainData::History {
    Step* undo = nullptr;
    int undo_n = 0, undo_cap = 0;
    Step* redo = nullptr;
    int redo_n = 0, redo_cap = 0;
    int next_rev = 1;
    size_t bytes = 0;
    size_t block = 0;                   // bytes of one chunk block
    // The open stroke.
    bool stroking = false;
    bool changed = false;
    uint8_t* touched = nullptr;         // per chunk
    int* list = nullptr;
    int list_n = 0, list_cap = 0;
    uint8_t* before = nullptr;          // list_n blocks
    size_t before_cap = 0;
};

namespace {

size_t step_bytes(const Step& s, size_t) { return s.mem; }

void step_free(Step& s) {
    free(s.before);
    free(s.after);
    free(s.chunk);
    free(s.before_blocks);
    free(s.after_blocks);
    s = Step();
}

bool push_step(Step** arr, int* n, int* cap, const Step& s) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 16;
        Step* na = (Step*)realloc(*arr, sizeof(Step) * (size_t)nc);
        if (!na) return false;
        *arr = na;
        *cap = nc;
    }
    (*arr)[(*n)++] = s;
    return true;
}

}  // namespace

// ---------------------------------------------------------------- data

TerrainData::~TerrainData() { clear(); }

void TerrainData::free_data() {
    free(heights);
    free(holes);
    for (int l = 0; l < kFoliageLayers; l++) { free(foliage[l]); foliage[l] = nullptr; }
    free(chunk_info);
    heights = nullptr;
    holes = nullptr;
    chunk_info = nullptr;
    chunks_x = chunks_z = cells = 0;
    chunk_size = 0;
}

void TerrainData::clear() {
    free_data();
    if (hist_) {
        for (int i = 0; i < hist_->undo_n; i++) step_free(hist_->undo[i]);
        for (int i = 0; i < hist_->redo_n; i++) step_free(hist_->redo[i]);
        free(hist_->undo);
        free(hist_->redo);
        free(hist_->touched);
        free(hist_->list);
        free(hist_->before);
        free(hist_);
        hist_ = nullptr;
    }
    revision = 0;
}

bool TerrainData::alloc(int cx, int cz, int cells_, float size) {
    free_data();
    cells_ = clampi(cells_, 4, 128);
    cx = clampi(cx, 1, kTerrainMaxCells / cells_);
    cz = clampi(cz, 1, kTerrainMaxCells / cells_);
    if (!(size > 0.01f)) size = 32.0f;
    chunks_x = cx;
    chunks_z = cz;
    cells = cells_;
    chunk_size = size;
    size_t nv = (size_t)(side_x() + 1) * (size_t)(side_z() + 1);
    size_t nc = (size_t)side_x() * (size_t)side_z();
    heights = (float*)calloc(nv, sizeof(float));
    holes = (uint8_t*)calloc(nc, 1);
    chunk_info = (TerrainChunkInfo*)malloc(sizeof(TerrainChunkInfo) * (size_t)chunk_count());
    if (!heights || !holes || !chunk_info) { free_data(); return false; }
    for (int i = 0; i < chunk_count(); i++) chunk_info[i] = TerrainChunkInfo();
    return true;
}

bool TerrainData::ensure_layer(int layer) {
    if (layer < 0 || layer >= kFoliageLayers || !valid()) return false;
    if (!foliage[layer]) foliage[layer] = (uint8_t*)calloc((size_t)side_x() * (size_t)side_z(), 1);
    return foliage[layer] != nullptr;
}

bool TerrainData::generate(int cx, int cz, int cells_, float size, float height) {
    if (!hist_) {
        hist_ = (History*)calloc(1, sizeof(History));
        if (!hist_) return false;
        hist_->next_rev = revision + 1;
    }
    Step st;
    st.full = true;
    st.from = revision;
    if (valid()) st.before = save(&st.before_n);
    // Versions keep counting up across a regenerate, so cached meshes of the old data are not reused.
    uint32_t vbase = 1;
    if (chunk_info)
        for (int i = 0; i < chunk_count(); i++)
            if (chunk_info[i].version >= vbase) vbase = chunk_info[i].version + 1;
    if (!alloc(cx, cz, cells_, size)) { free(st.before); return false; }
    size_t nv = (size_t)(side_x() + 1) * (size_t)(side_z() + 1);
    for (size_t i = 0; i < nv; i++) heights[i] = height;
    for (int i = 0; i < chunk_count(); i++) { chunk_info[i].version = vbase; chunk_info[i].foliage_version = vbase; }
    if (hist_->next_rev <= revision) hist_->next_rev = revision + 1;
    revision = hist_->next_rev++;
    st.to = revision;
    st.after = save(&st.after_n);
    st.mem = st.before_n + st.after_n;
    for (int i = 0; i < hist_->redo_n; i++) { hist_->bytes -= hist_->redo[i].mem; step_free(hist_->redo[i]); }
    hist_->redo_n = 0;
    free(hist_->touched);
    hist_->touched = nullptr;
    hist_->block = 0;
    if (st.after && push_step(&hist_->undo, &hist_->undo_n, &hist_->undo_cap, st)) hist_->bytes += step_bytes(st, 0);
    else step_free(st);
    return true;
}

float TerrainData::h(int ix, int iz) const {
    ix = clampi(ix, 0, side_x());
    iz = clampi(iz, 0, side_z());
    return heights[(size_t)iz * (size_t)(side_x() + 1) + (size_t)ix];
}

bool TerrainData::hole(int cx, int cz) const {
    if (cx < 0 || cz < 0 || cx >= side_x() || cz >= side_z()) return true;
    return holes[(size_t)cz * (size_t)side_x() + (size_t)cx] != 0;
}

uint8_t TerrainData::density(int layer, int cx, int cz) const {
    if (layer < 0 || layer >= kFoliageLayers || !foliage[layer]) return 0;
    if (cx < 0 || cz < 0 || cx >= side_x() || cz >= side_z()) return 0;
    return foliage[layer][(size_t)cz * (size_t)side_x() + (size_t)cx];
}

// The quad of cell (ix, iz) is split along a(ix, iz) - c(ix+1, iz+1): triangle
// (a, d, c) where fz > fx (d = (ix, iz+1)), else (a, c, b) (b = (ix+1, iz)).
float TerrainData::height_at(float x, float z, bool* inside) const {
    if (!valid()) { if (inside) *inside = false; return 0.0f; }
    float c = cell();
    float gx = x / c, gz = z / c;
    bool in = gx >= 0 && gz >= 0 && gx <= (float)side_x() && gz <= (float)side_z();
    gx = fclamp(gx, 0.0f, (float)side_x());
    gz = fclamp(gz, 0.0f, (float)side_z());
    int ix = clampi(floori(gx), 0, side_x() - 1), iz = clampi(floori(gz), 0, side_z() - 1);
    float fx = gx - (float)ix, fz = gz - (float)iz;
    if (inside) *inside = in && !hole(ix, iz);
    float ha = h(ix, iz), hb = h(ix + 1, iz), hc = h(ix + 1, iz + 1), hd = h(ix, iz + 1);
    if (fz > fx) return ha + fx * (hc - hd) + fz * (hd - ha);
    return ha + fx * (hb - ha) + fz * (hc - hb);
}

Vec3 TerrainData::normal_at(float x, float z) const {
    if (!valid()) return Vec3(0, 1, 0);
    float c = cell();
    float gx = fclamp(x / c, 0.0f, (float)side_x()), gz = fclamp(z / c, 0.0f, (float)side_z());
    int ix = clampi(floori(gx), 0, side_x() - 1), iz = clampi(floori(gz), 0, side_z() - 1);
    float fx = gx - (float)ix, fz = gz - (float)iz;
    float ha = h(ix, iz), hb = h(ix + 1, iz), hc = h(ix + 1, iz + 1), hd = h(ix, iz + 1);
    float dx, dz;
    if (fz > fx) { dx = (hc - hd) / c; dz = (hd - ha) / c; }
    else { dx = (hb - ha) / c; dz = (hc - hb) / c; }
    return normalize(Vec3(-dx, 1.0f, -dz));
}

bool TerrainData::raycast(const Ray& r, float max_t, float* t_out) const {
    if (!valid()) return false;
    float ylo = 1e30f, yhi = -1e30f;
    for (int i = 0; i < chunk_count(); i++) {
        Vec3 lo, hi;
        const_cast<TerrainData*>(this)->chunk_bounds(i % chunks_x, i / chunks_x, &lo, &hi);
        ylo = fminf_(ylo, lo.y);
        yhi = fmaxf_(yhi, hi.y);
    }
    Vec3 blo(0, ylo - 0.01f, 0), bhi(size_x(), yhi + 0.01f, size_z());
    float t0 = ray_aabb(r, blo, bhi);
    if (t0 < 0 || t0 > max_t) return false;
    // Walk the cells the ray crosses in x/z (Amanatides & Woo).
    float c = cell();
    Vec3 p = r.origin + r.dir * t0;
    int ix = clampi(floori(p.x / c), 0, side_x() - 1), iz = clampi(floori(p.z / c), 0, side_z() - 1);
    int sx = r.dir.x > 0 ? 1 : -1, sz = r.dir.z > 0 ? 1 : -1;
    float ax = fabsf_(r.dir.x), az = fabsf_(r.dir.z);
    float dtx = ax > 1e-9f ? c / ax : 1e30f, dtz = az > 1e-9f ? c / az : 1e30f;
    float nx = (float)(sx > 0 ? ix + 1 : ix) * c, nz = (float)(sz > 0 ? iz + 1 : iz) * c;
    float tx = ax > 1e-9f ? t0 + (nx - p.x) / r.dir.x : 1e30f;
    float tz = az > 1e-9f ? t0 + (nz - p.z) / r.dir.z : 1e30f;
    int steps = side_x() + side_z() + 4;
    while (steps-- > 0 && ix >= 0 && iz >= 0 && ix < side_x() && iz < side_z()) {
        if (!hole(ix, iz)) {
            float x0 = (float)ix * c, z0 = (float)iz * c;
            Vec3 a(x0, h(ix, iz), z0), b(x0 + c, h(ix + 1, iz), z0), cc(x0 + c, h(ix + 1, iz + 1), z0 + c), d(x0, h(ix, iz + 1), z0 + c);
            float best = -1.0f;
            float u = ray_triangle(r, a, d, cc);
            if (u > 0) best = u;
            u = ray_triangle(r, a, cc, b);
            if (u > 0 && (best < 0 || u < best)) best = u;
            if (best > 0 && best <= max_t) { *t_out = best; return true; }
        }
        float tn = tx < tz ? tx : tz;
        if (tn > max_t) break;
        if (tx < tz) { ix += sx; tx += dtx; }
        else { iz += sz; tz += dtz; }
    }
    return false;
}

// ---------------------------------------------------------------- editing

void TerrainData::bump(int ix0, int iz0, int ix1, int iz1, bool mesh, bool foliage_too) {
    // Normals reach one vertex further.
    int a0 = ix0 - 1, b0 = iz0 - 1, a1 = ix1 + 1, b1 = iz1 + 1;
    int cx0 = clampi(floordiv(a0 - 1, cells), 0, chunks_x - 1), cx1 = clampi(floordiv(a1, cells), 0, chunks_x - 1);
    int cz0 = clampi(floordiv(b0 - 1, cells), 0, chunks_z - 1), cz1 = clampi(floordiv(b1, cells), 0, chunks_z - 1);
    for (int z = cz0; z <= cz1; z++)
        for (int x = cx0; x <= cx1; x++) {
            TerrainChunkInfo& ci = chunk_info[z * chunks_x + x];
            if (mesh) ci.version++;
            if (foliage_too) ci.foliage_version++;
        }
}

bool TerrainData::import_heights(const float* samples, int w, int h, float scale, float offset) {
    if (!valid() || !samples || w < 2 || h < 2) return false;
    bool own_stroke = !in_stroke();
    if (own_stroke) begin_stroke();
    snapshot(0, 0, side_x(), side_z());
    const int sx = side_x(), sz = side_z();
    for (int z = 0; z <= sz; z++) {
        float fz = (float)z / (float)sz * (float)(h - 1);
        int z0 = (int)fz;
        if (z0 > h - 2) z0 = h - 2;
        float tz = fz - (float)z0;
        for (int x = 0; x <= sx; x++) {
            float fx = (float)x / (float)sx * (float)(w - 1);
            int x0 = (int)fx;
            if (x0 > w - 2) x0 = w - 2;
            float tx = fx - (float)x0;
            const float* r0 = samples + (size_t)z0 * (size_t)w + (size_t)x0;
            const float* r1 = r0 + w;
            float a = r0[0] + (r0[1] - r0[0]) * tx, b = r1[0] + (r1[1] - r1[0]) * tx;
            heights[(size_t)z * (size_t)(sx + 1) + (size_t)x] = offset + (a + (b - a) * tz) * scale;
        }
    }
    bump(0, 0, sx, sz, true, true);
    if (hist_ && hist_->stroking) hist_->changed = true;
    if (own_stroke) end_stroke();
    return true;
}

void TerrainData::touch_cells(int x0, int z0, int x1, int z1, bool mesh) {
    if (mesh) { bump(x0, z0, x1 + 1, z1 + 1, true, true); return; }
    int cx0 = clampi(x0 / cells, 0, chunks_x - 1), cx1 = clampi(x1 / cells, 0, chunks_x - 1);
    int cz0 = clampi(z0 / cells, 0, chunks_z - 1), cz1 = clampi(z1 / cells, 0, chunks_z - 1);
    for (int z = cz0; z <= cz1; z++)
        for (int x = cx0; x <= cx1; x++) chunk_info[z * chunks_x + x].foliage_version++;
}

namespace {

// One chunk's data as a block: heights (cells+1)^2, holes cells^2, each foliage layer cells^2.
size_t block_size(int cells) {
    return (size_t)(cells + 1) * (size_t)(cells + 1) * 4 + (size_t)cells * (size_t)cells * (1 + kFoliageLayers);
}

}  // namespace

static void read_block(const TerrainData& t, int chunk, uint8_t* out) {
    int c = t.cells, cx = chunk % t.chunks_x, cz = chunk / t.chunks_x;
    int sx = t.side_x();
    float* hp = (float*)out;
    for (int z = 0; z <= c; z++)
        memcpy(hp + z * (c + 1), t.heights + (size_t)(cz * c + z) * (size_t)(sx + 1) + (size_t)(cx * c), sizeof(float) * (size_t)(c + 1));
    uint8_t* p = out + (size_t)(c + 1) * (size_t)(c + 1) * 4;
    for (int z = 0; z < c; z++) memcpy(p + z * c, t.holes + (size_t)(cz * c + z) * (size_t)sx + (size_t)(cx * c), (size_t)c);
    p += (size_t)c * (size_t)c;
    for (int l = 0; l < kFoliageLayers; l++, p += (size_t)c * (size_t)c) {
        if (!t.foliage[l]) { memset(p, 0, (size_t)c * (size_t)c); continue; }
        for (int z = 0; z < c; z++) memcpy(p + z * c, t.foliage[l] + (size_t)(cz * c + z) * (size_t)sx + (size_t)(cx * c), (size_t)c);
    }
}

static void write_block(TerrainData& t, int chunk, const uint8_t* in, bool (*ensure)(TerrainData&, int)) {
    int c = t.cells, cx = chunk % t.chunks_x, cz = chunk / t.chunks_x;
    int sx = t.side_x();
    const float* hp = (const float*)in;
    for (int z = 0; z <= c; z++)
        memcpy(t.heights + (size_t)(cz * c + z) * (size_t)(sx + 1) + (size_t)(cx * c), hp + z * (c + 1), sizeof(float) * (size_t)(c + 1));
    const uint8_t* p = in + (size_t)(c + 1) * (size_t)(c + 1) * 4;
    for (int z = 0; z < c; z++) memcpy(t.holes + (size_t)(cz * c + z) * (size_t)sx + (size_t)(cx * c), p + z * c, (size_t)c);
    p += (size_t)c * (size_t)c;
    for (int l = 0; l < kFoliageLayers; l++, p += (size_t)c * (size_t)c) {
        if (!t.foliage[l]) {
            bool any = false;
            for (int i = 0; i < c * c && !any; i++) any = p[i] != 0;
            if (!any || !ensure(t, l)) continue;
        }
        for (int z = 0; z < c; z++) memcpy(t.foliage[l] + (size_t)(cz * c + z) * (size_t)sx + (size_t)(cx * c), p + z * c, (size_t)c);
    }
}

void TerrainData::snapshot(int ix0, int iz0, int ix1, int iz1) {
    if (!hist_ || !hist_->stroking) return;
    History& H = *hist_;
    int cx0 = clampi(floordiv(ix0 - 1, cells), 0, chunks_x - 1), cx1 = clampi(floordiv(ix1, cells), 0, chunks_x - 1);
    int cz0 = clampi(floordiv(iz0 - 1, cells), 0, chunks_z - 1), cz1 = clampi(floordiv(iz1, cells), 0, chunks_z - 1);
    for (int z = cz0; z <= cz1; z++)
        for (int x = cx0; x <= cx1; x++) {
            int ch = z * chunks_x + x;
            if (H.touched[ch]) continue;
            if (H.list_n == H.list_cap) {
                int nc = H.list_cap ? H.list_cap * 2 : 32;
                int* nl = (int*)realloc(H.list, sizeof(int) * (size_t)nc);
                uint8_t* nb = (uint8_t*)realloc(H.before, H.block * (size_t)nc);
                if (nl) H.list = nl;
                if (nb) H.before = nb;
                if (!nl || !nb) return;
                H.list_cap = nc;
            }
            read_block(*this, ch, H.before + H.block * (size_t)H.list_n);
            H.list[H.list_n++] = ch;
            H.touched[ch] = 1;
        }
}

void TerrainData::begin_stroke() {
    if (!valid()) return;
    if (!hist_) {
        hist_ = (History*)calloc(1, sizeof(History));
        if (!hist_) return;
        hist_->next_rev = revision + 1;
    }
    History& H = *hist_;
    if (H.stroking) end_stroke();
    size_t blk = block_size(cells);
    if (!H.touched || H.block != blk) {
        free(H.touched);
        H.touched = (uint8_t*)calloc((size_t)chunk_count(), 1);
        H.block = blk;
        free(H.before);
        free(H.list);
        H.before = nullptr;
        H.list = nullptr;
        H.list_cap = 0;
        if (!H.touched) return;
    }
    H.stroking = true;
    H.changed = false;
    H.list_n = 0;
}

bool TerrainData::in_stroke() const { return hist_ && hist_->stroking; }

bool TerrainData::end_stroke() {
    if (!hist_ || !hist_->stroking) return false;
    History& H = *hist_;
    H.stroking = false;
    for (int i = 0; i < H.list_n; i++) H.touched[H.list[i]] = 0;
    if (!H.changed || H.list_n == 0) return false;
    Step st;
    st.from = revision;
    if (H.next_rev <= revision) H.next_rev = revision + 1;
    st.to = H.next_rev++;
    st.n = H.list_n;
    st.chunk = (int*)malloc(sizeof(int) * (size_t)st.n);
    st.before_blocks = (uint8_t*)malloc(H.block * (size_t)st.n);
    st.after_blocks = (uint8_t*)malloc(H.block * (size_t)st.n);
    revision = st.to;
    if (!st.chunk || !st.before_blocks || !st.after_blocks) { step_free(st); return true; }
    memcpy(st.chunk, H.list, sizeof(int) * (size_t)st.n);
    memcpy(st.before_blocks, H.before, H.block * (size_t)st.n);
    for (int i = 0; i < st.n; i++) read_block(*this, st.chunk[i], st.after_blocks + H.block * (size_t)i);
    st.mem = (size_t)st.n * (4 + 2 * H.block);
    for (int i = 0; i < H.redo_n; i++) { H.bytes -= step_bytes(H.redo[i], H.block); step_free(H.redo[i]); }
    H.redo_n = 0;
    if (!push_step(&H.undo, &H.undo_n, &H.undo_cap, st)) { step_free(st); return true; }
    H.bytes += step_bytes(st, H.block);
    // Forget the oldest steps over the memory budget (the newest stays).
    while (H.bytes > kHistoryBytes && H.undo_n > 1) {
        H.bytes -= step_bytes(H.undo[0], H.block);
        step_free(H.undo[0]);
        memmove(H.undo, H.undo + 1, sizeof(Step) * (size_t)(H.undo_n - 1));
        H.undo_n--;
    }
    return true;
}

int TerrainData::history_steps() const { return hist_ ? hist_->undo_n : 0; }

static bool ensure_cb(TerrainData& t, int l) {
    if (t.foliage[l]) return true;
    t.foliage[l] = (uint8_t*)calloc((size_t)t.side_x() * (size_t)t.side_z(), 1);
    return t.foliage[l] != nullptr;
}

bool TerrainData::goto_revision(int r) {
    if (r == revision) return true;
    if (!hist_ || hist_->stroking) return false;
    History& H = *hist_;
    auto apply_step = [&](const Step& s, bool forward) -> bool {
        if (s.full) {
            const uint8_t* blob = forward ? s.after : s.before;
            size_t n = forward ? s.after_n : s.before_n;
            uint32_t vbase = 1;
            if (chunk_info)
                for (int i = 0; i < chunk_count(); i++)
                    if (chunk_info[i].version >= vbase) vbase = chunk_info[i].version + 1;
            History* keep = hist_;
            hist_ = nullptr;                // load() clears the history: keep it
            bool ok = blob ? load(blob, n) : (free_data(), true);
            hist_ = keep;
            if (chunk_info)
                for (int i = 0; i < chunk_count(); i++) { chunk_info[i].version = vbase; chunk_info[i].foliage_version = vbase; }
            H.block = 0;
            free(H.touched);
            H.touched = nullptr;
            revision = forward ? s.to : s.from;
            return ok;
        }
        if (!valid()) return false;
        size_t blk = block_size(cells);
        const uint8_t* blocks = forward ? s.after_blocks : s.before_blocks;
        for (int i = 0; i < s.n; i++) {
            int ch = s.chunk[i];
            if (ch < 0 || ch >= chunk_count()) return false;
            write_block(*this, ch, blocks + blk * (size_t)i, ensure_cb);
            int cx = ch % chunks_x, cz = ch / chunks_x;
            bump(cx * cells, cz * cells, (cx + 1) * cells, (cz + 1) * cells, true, true);
        }
        revision = forward ? s.to : s.from;
        return true;
    };
    for (int i = H.undo_n - 1; i >= 0; i--) {
        if (H.undo[i].from != r) continue;
        while (H.undo_n > i) {
            Step s = H.undo[--H.undo_n];
            if (!apply_step(s, false)) { H.undo[H.undo_n++] = s; return false; }
            push_step(&H.redo, &H.redo_n, &H.redo_cap, s);
        }
        return true;
    }
    for (int i = H.redo_n - 1; i >= 0; i--) {
        if (H.redo[i].to != r) continue;
        while (H.redo_n > i) {
            Step s = H.redo[--H.redo_n];
            if (!apply_step(s, true)) { H.redo[H.redo_n++] = s; return false; }
            push_step(&H.undo, &H.undo_n, &H.undo_cap, s);
        }
        return true;
    }
    return false;
}

bool TerrainData::apply(const TerrainBrushOp& op) {
    if (!valid() || !(op.radius > 0)) return false;
    float c = cell();
    float r = op.radius;
    bool changed = false;
    int sx = side_x(), sz = side_z();
    switch (op.brush) {
    case TerrainBrush::Raise:
    case TerrainBrush::Lower:
    case TerrainBrush::Smooth:
    case TerrainBrush::Level: {
        int ix0 = clampi(floori((op.x - r) / c), 0, sx), ix1 = clampi(ceili((op.x + r) / c), 0, sx);
        int iz0 = clampi(floori((op.z - r) / c), 0, sz), iz1 = clampi(ceili((op.z + r) / c), 0, sz);
        if (ix0 > ix1 || iz0 > iz1) return false;
        snapshot(ix0, iz0, ix1, iz1);
        int w = ix1 - ix0 + 1, hgt = iz1 - iz0 + 1;
        float* src = nullptr;
        if (op.brush == TerrainBrush::Smooth) {
            // The region with a one vertex border, read before anything changes.
            src = (float*)malloc(sizeof(float) * (size_t)(w + 2) * (size_t)(hgt + 2));
            if (!src) return false;
            for (int z = 0; z < hgt + 2; z++)
                for (int x = 0; x < w + 2; x++) src[z * (w + 2) + x] = h(ix0 + x - 1, iz0 + z - 1);
        }
        float rate = fclamp(op.rate, 0.0f, 1.0f);
        for (int iz = iz0; iz <= iz1; iz++)
            for (int ix = ix0; ix <= ix1; ix++) {
                float dx = (float)ix * c - op.x, dz = (float)iz * c - op.z;
                float wgt = brush_weight(fsqrt(dx * dx + dz * dz), r, op.hardness);
                if (wgt <= 0) continue;
                float& hv = heights[(size_t)iz * (size_t)(sx + 1) + (size_t)ix];
                float nv = hv;
                if (op.brush == TerrainBrush::Raise) nv = hv + op.amount * wgt;
                else if (op.brush == TerrainBrush::Lower) nv = hv - op.amount * wgt;
                else if (op.brush == TerrainBrush::Level) nv = hv + (op.level - hv) * fclamp(rate * wgt, 0.0f, 1.0f);
                else {
                    int lx = ix - ix0 + 1, lz = iz - iz0 + 1;
                    float sum = 0;
                    for (int oz = -1; oz <= 1; oz++)
                        for (int ox = -1; ox <= 1; ox++) sum += src[(lz + oz) * (w + 2) + lx + ox];
                    nv = hv + (sum / 9.0f - hv) * fclamp(rate * wgt, 0.0f, 1.0f);
                }
                if (nv != hv) { hv = nv; changed = true; }
            }
        free(src);
        if (changed) bump(ix0, iz0, ix1, iz1, true, true);
        break;
    }
    case TerrainBrush::Delete:
    case TerrainBrush::Restore: {
        int x0 = clampi(floori((op.x - r) / c), 0, sx - 1), x1 = clampi(ceili((op.x + r) / c), 0, sx - 1);
        int z0 = clampi(floori((op.z - r) / c), 0, sz - 1), z1 = clampi(ceili((op.z + r) / c), 0, sz - 1);
        snapshot(x0, z0, x1 + 1, z1 + 1);
        uint8_t v = op.brush == TerrainBrush::Delete ? 1 : 0;
        for (int z = z0; z <= z1; z++)
            for (int x = x0; x <= x1; x++) {
                float dx = ((float)x + 0.5f) * c - op.x, dz = ((float)z + 0.5f) * c - op.z;
                if (dx * dx + dz * dz > r * r) continue;
                uint8_t& hv = holes[(size_t)z * (size_t)sx + (size_t)x];
                if (hv != v) { hv = v; changed = true; }
            }
        if (changed) touch_cells(x0, z0, x1, z1, true);
        break;
    }
    case TerrainBrush::Paint:
    case TerrainBrush::Erase: {
        if (op.layer < 0 || op.layer >= kFoliageLayers) return false;
        if (op.brush == TerrainBrush::Erase && !foliage[op.layer]) return false;
        int x0 = clampi(floori((op.x - r) / c), 0, sx - 1), x1 = clampi(ceili((op.x + r) / c), 0, sx - 1);
        int z0 = clampi(floori((op.z - r) / c), 0, sz - 1), z1 = clampi(ceili((op.z + r) / c), 0, sz - 1);
        snapshot(x0, z0, x1 + 1, z1 + 1);
        if (!ensure_layer(op.layer)) return false;
        float target = op.brush == TerrainBrush::Paint ? fclamp(op.target, 0.0f, 1.0f) * 255.0f : 0.0f;
        float rate = fclamp(op.rate, 0.0f, 1.0f);
        for (int z = z0; z <= z1; z++)
            for (int x = x0; x <= x1; x++) {
                float dx = ((float)x + 0.5f) * c - op.x, dz = ((float)z + 0.5f) * c - op.z;
                float wgt = brush_weight(fsqrt(dx * dx + dz * dz), r, op.hardness);
                if (wgt <= 0) continue;
                uint8_t& dv = foliage[op.layer][(size_t)z * (size_t)sx + (size_t)x];
                float k = fclamp(rate * wgt, 0.0f, 1.0f);
                float nv = (float)dv + (target - (float)dv) * k;
                // Always move at least one step towards the target, so slow rates still get there.
                int ni = (int)(nv + 0.5f);
                if (ni == dv && k > 0) ni = target > dv ? dv + 1 : (target < dv ? dv - 1 : dv);
                ni = clampi(ni, 0, 255);
                if (ni != dv) { dv = (uint8_t)ni; changed = true; }
            }
        if (changed) touch_cells(x0, z0, x1, z1, false);
        break;
    }
    default: return false;
    }
    if (changed && hist_ && hist_->stroking) hist_->changed = true;
    return changed;
}

// ---------------------------------------------------------------- chunks

void TerrainData::chunk_bounds(int cx, int cz, Vec3* lo, Vec3* hi) {
    TerrainChunkInfo& ci = chunk_info[cz * chunks_x + cx];
    if (ci.bounds_version != ci.version) {
        float mn = 1e30f, mx = -1e30f;
        for (int z = 0; z <= cells; z++)
            for (int x = 0; x <= cells; x++) {
                float v = h(cx * cells + x, cz * cells + z);
                mn = fminf_(mn, v);
                mx = fmaxf_(mx, v);
            }
        ci.hmin = mn;
        ci.hmax = mx;
        ci.bounds_version = ci.version;
    }
    float skirt = fmaxf_(1.0f, cell() * 4.0f * (float)(1 << max_lod()));
    *lo = Vec3((float)cx * chunk_size, ci.hmin - skirt, (float)cz * chunk_size);
    *hi = Vec3((float)(cx + 1) * chunk_size, ci.hmax, (float)(cz + 1) * chunk_size);
}

int TerrainData::max_lod() const {
    int l = 0;
    while (l + 1 < kTerrainLods && (cells >> (l + 1)) >= 2) l++;
    return l;
}

uint32_t TerrainData::mesh_capacity(int cells_) {
    return (uint32_t)cells_ * (uint32_t)cells_ * 6u + 4u * (uint32_t)cells_ * 6u;
}

uint32_t TerrainData::build_chunk(int cx, int cz, int lod, float uv_scale, Vec3* pos, Vec3* nrm, float* uv) const {
    if (!valid() || cx < 0 || cz < 0 || cx >= chunks_x || cz >= chunks_z) return 0;
    lod = clampi(lod, 0, max_lod());
    const int s = 1 << lod;
    const float c = cell();
    const float inv_uv = uv_scale > 1e-4f ? 1.0f / uv_scale : 1.0f;
    const int gx0 = cx * cells, gz0 = cz * cells;
    const float skirt = fmaxf_(1.0f, c * 4.0f * (float)s);
    uint32_t n = 0;
    auto vnormal = [&](int ix, int iz) {
        return normalize(Vec3((h(ix - 1, iz) - h(ix + 1, iz)), 2.0f * c, (h(ix, iz - 1) - h(ix, iz + 1))));
    };
    auto emit = [&](const Vec3& p, const Vec3& nn, float vofs) {
        pos[n] = p;
        nrm[n] = nn;
        if (uv) { uv[n * 2] = p.x * inv_uv; uv[n * 2 + 1] = p.z * inv_uv + vofs; }
        n++;
    };
    auto block_hole = [&](int ix, int iz) {         // any hole in the s x s cells from (ix, iz)
        for (int z = 0; z < s; z++)
            for (int x = 0; x < s; x++)
                if (hole(ix + x, iz + z)) return true;
        return false;
    };
    auto P = [&](int ix, int iz) { return Vec3((float)ix * c, h(ix, iz), (float)iz * c); };
    for (int z = 0; z < cells; z += s)
        for (int x = 0; x < cells; x += s) {
            int ix = gx0 + x, iz = gz0 + z;
            if (block_hole(ix, iz)) continue;
            Vec3 a = P(ix, iz), b = P(ix + s, iz), cc = P(ix + s, iz + s), d = P(ix, iz + s);
            Vec3 na = vnormal(ix, iz), nb = vnormal(ix + s, iz), nc = vnormal(ix + s, iz + s), nd = vnormal(ix, iz + s);
            emit(a, na, 0); emit(d, nd, 0); emit(cc, nc, 0);
            emit(a, na, 0); emit(cc, nc, 0); emit(b, nb, 0);
        }
    // Skirts: a strip down each edge, facing out of the chunk.
    auto skirt_seg = [&](int ix0, int iz0, int ix1, int iz1, const Vec3& out, int hx, int hz) {
        if (hole(hx, hz)) return;
        Vec3 t0 = P(ix0, iz0), t1 = P(ix1, iz1);
        Vec3 n0 = vnormal(ix0, iz0), n1 = vnormal(ix1, iz1);
        Vec3 b0 = t0 - Vec3(0, skirt, 0), b1 = t1 - Vec3(0, skirt, 0);
        float dv = skirt * inv_uv;
        if (dot(cross(t1 - t0, b1 - t0), out) >= 0) {
            emit(t0, n0, 0); emit(t1, n1, 0); emit(b1, n1, dv);
            emit(t0, n0, 0); emit(b1, n1, dv); emit(b0, n0, dv);
        } else {
            emit(t0, n0, 0); emit(b1, n1, dv); emit(t1, n1, 0);
            emit(t0, n0, 0); emit(b0, n0, dv); emit(b1, n1, dv);
        }
    };
    for (int i = 0; i < cells; i += s) {
        skirt_seg(gx0 + i, gz0, gx0 + i + s, gz0, Vec3(0, 0, -1), gx0 + i, gz0);                                   // -z edge
        skirt_seg(gx0 + i, gz0 + cells, gx0 + i + s, gz0 + cells, Vec3(0, 0, 1), gx0 + i, gz0 + cells - 1);        // +z edge
        skirt_seg(gx0, gz0 + i, gx0, gz0 + i + s, Vec3(-1, 0, 0), gx0, gz0 + i);                                   // -x edge
        skirt_seg(gx0 + cells, gz0 + i, gx0 + cells, gz0 + i + s, Vec3(1, 0, 0), gx0 + cells - 1, gz0 + i);        // +x edge
    }
    return n;
}

uint32_t TerrainData::occluder_capacity() { return (uint32_t)kOccluderBlocks * kOccluderBlocks * 6u; }

uint32_t TerrainData::build_occluder(int cx, int cz, Vec3* pos) const {
    if (!valid() || cx < 0 || cz < 0 || cx >= chunks_x || cz >= chunks_z) return 0;
    int nb = cells < kOccluderBlocks ? cells : kOccluderBlocks;
    int bs = cells / nb;
    const float c = cell();
    const int gx0 = cx * cells, gz0 = cz * cells;
    // Lowest height of each coarse vertex over the blocks around it (the
    // coarse surface then stays under every block it spans).
    float low[(kOccluderBlocks + 1) * (kOccluderBlocks + 1)];
    for (int j = 0; j <= nb; j++)
        for (int i = 0; i <= nb; i++) {
            int x0 = gx0 + (i - 1) * bs, x1 = gx0 + (i + 1) * bs;
            int z0 = gz0 + (j - 1) * bs, z1 = gz0 + (j + 1) * bs;
            float m = 1e30f;
            for (int z = z0; z <= z1; z++)
                for (int x = x0; x <= x1; x++) m = fminf_(m, h(x, z));
            low[j * (nb + 1) + i] = m;
        }
    uint32_t n = 0;
    for (int j = 0; j < nb; j++)
        for (int i = 0; i < nb; i++) {
            bool any = false;
            for (int z = 0; z < bs && !any; z++)
                for (int x = 0; x < bs && !any; x++) any = hole(gx0 + i * bs + x, gz0 + j * bs + z);
            if (any) continue;
            float x0 = (float)(gx0 + i * bs) * c, x1 = (float)(gx0 + (i + 1) * bs) * c;
            float z0 = (float)(gz0 + j * bs) * c, z1 = (float)(gz0 + (j + 1) * bs) * c;
            Vec3 a(x0, low[j * (nb + 1) + i], z0), b(x1, low[j * (nb + 1) + i + 1], z0);
            Vec3 cc(x1, low[(j + 1) * (nb + 1) + i + 1], z1), d(x0, low[(j + 1) * (nb + 1) + i], z1);
            pos[n++] = a; pos[n++] = d; pos[n++] = cc;
            pos[n++] = a; pos[n++] = cc; pos[n++] = b;
        }
    return n;
}

// ---------------------------------------------------------------- foliage

bool TerrainData::has_foliage(int cx, int cz, int layer) const {
    if (layer < 0 || layer >= kFoliageLayers || !foliage[layer]) return false;
    for (int z = 0; z < cells; z++) {
        const uint8_t* row = foliage[layer] + (size_t)(cz * cells + z) * (size_t)side_x() + (size_t)(cx * cells);
        for (int x = 0; x < cells; x++)
            if (row[x]) return true;
    }
    return false;
}

int TerrainData::scatter_bound(int cx, int cz, int layer, const FoliageLayer& fl) const {
    if (!has_foliage(cx, cz, layer) || !(fl.density > 0)) return 0;
    float per = fl.density * cell() * cell();
    int cand = ceili(per);
    if (cand > kMaxCandidates) cand = kMaxCandidates;
    if (cand < 1) cand = 1;
    int painted = 0;
    for (int z = 0; z < cells; z++) {
        const uint8_t* row = foliage[layer] + (size_t)(cz * cells + z) * (size_t)side_x() + (size_t)(cx * cells);
        for (int x = 0; x < cells; x++) painted += row[x] != 0;
    }
    return painted * cand;
}

int TerrainData::scatter(int cx, int cz, int layer, const FoliageLayer& fl, FoliageInstance* out, int cap) const {
    if (!has_foliage(cx, cz, layer) || !(fl.density > 0) || cap <= 0) return 0;
    const float c = cell();
    float per = fl.density * c * c;                 // expected instances in a fully painted cell
    int cand = ceili(per);
    if (cand > kMaxCandidates) cand = kMaxCandidates;
    if (cand < 1) cand = 1;
    float share = per / (float)cand;                // chance of each candidate at full density
    if (share > 1.0f) share = 1.0f;
    float min_up = fcos(radians(fclamp(fl.max_slope, 0.0f, 90.0f)));
    float smin = fl.scale_min, smax = fl.scale_max;
    if (smax < smin) { float t = smin; smin = smax; smax = t; }
    int bound = scatter_bound(cx, cz, layer, fl);
    uint64_t* keys = (uint64_t*)malloc(sizeof(uint64_t) * (size_t)(bound ? bound : 1));
    FoliageInstance* tmp = (FoliageInstance*)malloc(sizeof(FoliageInstance) * (size_t)(bound ? bound : 1));
    if (!keys || !tmp) { free(keys); free(tmp); return 0; }
    int n = 0;
    for (int z = 0; z < cells; z++)
        for (int x = 0; x < cells; x++) {
            int gx = cx * cells + x, gz = cz * cells + z;
            uint8_t d = density(layer, gx, gz);
            if (!d || hole(gx, gz)) continue;
            float chance = share * (float)d * (1.0f / 255.0f);
            for (int k = 0; k < cand && n < bound; k++) {
                uint32_t hsh = hash4((uint32_t)gx, (uint32_t)gz, (uint32_t)k, 0x51u + (uint32_t)layer * 0x1000193u);
                if (unit(hsh) >= chance) continue;
                uint32_t h2 = hash32(hsh ^ 0xa511e9b3u), h3 = hash32(h2 ^ 0x63d83595u), h4 = hash32(h3 ^ 0x2c1b3c6du);
                float px = ((float)gx + unit(h2)) * c, pz = ((float)gz + unit(h3)) * c;
                bool inside = false;
                float py = height_at(px, pz, &inside);
                if (!inside) continue;
                Vec3 nn = normal_at(px, pz);
                if (nn.y < min_up - 1e-4f) continue;
                FoliageInstance& fi = tmp[n];
                fi.pos = Vec3(px, py, pz);
                fi.up = fl.align_to_slope ? nn : Vec3(0, 1, 0);
                fi.yaw = unit(h4) * 2.0f * kPi;
                fi.scale = smin + (smax - smin) * unit(hash32(h4 ^ 0x9e3779b9u));
                fi.pick = hash32(h4 ^ 0x27d4eb2fu);
                keys[n] = ((uint64_t)hash32(h4 ^ 0x85ebca6bu) << 32) | (uint64_t)(uint32_t)n;
                n++;
            }
        }
    sort64(keys, n);
    int m = n < cap ? n : cap;
    for (int i = 0; i < m; i++) out[i] = tmp[(uint32_t)keys[i]];
    free(keys);
    free(tmp);
    return m;
}

// ---------------------------------------------------------------- file

uint8_t* TerrainData::save(size_t* size) const {
    if (!valid()) return nullptr;
    Writer w;
    w.u32(kFileMagic);
    w.u32(kFileVersion);
    w.u32((uint32_t)chunks_x);
    w.u32((uint32_t)chunks_z);
    w.u32((uint32_t)cells);
    w.f32(chunk_size);
    w.u32((uint32_t)revision);
    uint32_t layers = 0;
    for (int l = 0; l < kFoliageLayers; l++)
        if (foliage[l]) layers |= 1u << l;
    w.u32(layers);
    size_t nv = (size_t)(side_x() + 1) * (size_t)(side_z() + 1), nc = (size_t)side_x() * (size_t)side_z();
    w.put(heights, nv * sizeof(float));
    w.rle(holes, nc);
    for (int l = 0; l < kFoliageLayers; l++)
        if (foliage[l]) w.rle(foliage[l], nc);
    if (w.fail) { free(w.b); return nullptr; }
    *size = w.n;
    return w.b;
}

bool TerrainData::load(const void* data, size_t size) {
    Reader r{ (const uint8_t*)data, size };
    if (!data || r.u32() != kFileMagic || r.u32() != kFileVersion) return false;
    int cx = (int)r.u32(), cz = (int)r.u32(), cl = (int)r.u32();
    float cs = r.f32();
    int rev = (int)r.u32();
    uint32_t layers = r.u32();
    if (r.fail || cl < 4 || cl > 128 || cx < 1 || cz < 1 || cx * cl > kTerrainMaxCells || cz * cl > kTerrainMaxCells || !(cs > 0.01f)) return false;
    clear();
    if (!alloc(cx, cz, cl, cs)) return false;
    size_t nv = (size_t)(side_x() + 1) * (size_t)(side_z() + 1), nc = (size_t)side_x() * (size_t)side_z();
    if (!r.get(heights, nv * sizeof(float)) || !r.unrle(holes, nc)) { free_data(); return false; }
    for (int l = 0; l < kFoliageLayers; l++) {
        if (!(layers & (1u << l))) continue;
        if (!ensure_layer(l) || !r.unrle(foliage[l], nc)) { free_data(); return false; }
    }
    revision = rev;
    return true;
}

size_t TerrainData::memory() const {
    size_t n = 0;
    if (valid()) {
        size_t nv = (size_t)(side_x() + 1) * (size_t)(side_z() + 1), nc = (size_t)side_x() * (size_t)side_z();
        n += nv * 4 + nc;
        for (int l = 0; l < kFoliageLayers; l++)
            if (foliage[l]) n += nc;
    }
    if (hist_) n += hist_->bytes;
    return n;
}

}  // namespace fe
