#include "gfx/culling.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fe::gfx {

namespace {

constexpr int kBoundsCache = 4096;
constexpr int kDepthWidth = 256;
constexpr int kMaxOccluderTriangles = 40000;
constexpr uint32_t kMaxOccluderVertices = 4096;    // meshes with more (and no occluder mesh) do not occlude
constexpr float kDepthEps = 1e-5f;

double clock_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
}

struct ClipV { float x, y, z, w; };

ClipV mul(const Mat4& m, const Vec3& p) {
    return { m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3),
             m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
             m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3),
             m.at(3, 0) * p.x + m.at(3, 1) * p.y + m.at(3, 2) * p.z + m.at(3, 3) };
}

uint32_t key_slot(uint64_t k, uint32_t count) {
    k ^= (uint64_t)count * 0x9E3779B97F4A7C15ull;
    k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33;
    return (uint32_t)k & (kBoundsCache - 1);
}

}  // namespace

Culler::~Culler() {
    free(bounds_);
    free(depth_);
    free(pyramid_);
    free(wlo_);
    free(whi_);
    free(state_);
}

bool Culler::local_bounds(const RenderMesh& m, Vec3* lo, Vec3* hi) {
    if (m.has_bounds) { *lo = m.bounds_min; *hi = m.bounds_max; return true; }
    uint32_t vc = m.vertex_count / 3 * 3;
    if (!m.positions || !vc) return false;
    if (m.key) {
        if (!bounds_) bounds_ = (BoundsEntry*)calloc(kBoundsCache, sizeof(BoundsEntry));
        if (bounds_) {
            BoundsEntry& e = bounds_[key_slot(m.key, vc)];
            if (e.key == m.key && e.count == vc) { *lo = e.lo; *hi = e.hi; return true; }
        }
    }
    Vec3 a = m.positions[0], b = a;
    for (uint32_t i = 1; i < vc; i++) { a = vmin(a, m.positions[i]); b = vmax(b, m.positions[i]); }
    *lo = a;
    *hi = b;
    if (m.key && bounds_) {
        BoundsEntry& e = bounds_[key_slot(m.key, vc)];
        e.key = m.key; e.count = vc; e.lo = a; e.hi = b;
    }
    return true;
}

bool Culler::ensure(int items, int w, int h) {
    if (items > cap_) {
        int nc = items < 256 ? 256 : items * 2;
        Vec3* a = (Vec3*)realloc(wlo_, sizeof(Vec3) * (size_t)nc);
        if (a) wlo_ = a;
        Vec3* b = (Vec3*)realloc(whi_, sizeof(Vec3) * (size_t)nc);
        if (b) whi_ = b;
        uint8_t* s = (uint8_t*)realloc(state_, (size_t)nc);
        if (s) state_ = s;
        if (!a || !b || !s) return false;
        cap_ = nc;
    }
    if (w != w_ || h != h_) {
        free(depth_);
        free(pyramid_);
        depth_ = (float*)malloc(sizeof(float) * (size_t)w * (size_t)h);
        // Room for every level after the first (each a quarter of the one before, rounded up).
        size_t total = 0;
        int lw = w, lh = h;
        levels_ = 1;
        level_w_[0] = w; level_h_[0] = h;
        while ((lw > 1 || lh > 1) && levels_ < 12) {
            lw = (lw + 1) / 2; lh = (lh + 1) / 2;
            level_w_[levels_] = lw; level_h_[levels_] = lh;
            total += (size_t)lw * (size_t)lh;
            levels_++;
        }
        pyramid_ = (float*)malloc(sizeof(float) * (total ? total : 1));
        if (!depth_ || !pyramid_) { w_ = h_ = 0; return false; }
        w_ = w;
        h_ = h;
        level_[0] = depth_;
        float* p = pyramid_;
        for (int l = 1; l < levels_; l++) { level_[l] = p; p += (size_t)level_w_[l] * (size_t)level_h_[l]; }
    }
    return true;
}

// Draws triangles (object space, `mvp` to clip space) into the depth buffer,
// keeping the nearest NDC depth. Triangles are clipped at the near plane and
// back faces skipped (front = counter-clockwise, or clockwise when mirrored).
void Culler::raster(const Vec3* pos, uint32_t count, const Mat4& mvp, bool mirrored, int* tris) {
    const float W = (float)w_, H = (float)h_;
    for (uint32_t t = 0; t + 2 < count; t += 3) {
        ClipV in[3] = { mul(mvp, pos[t]), mul(mvp, pos[t + 1]), mul(mvp, pos[t + 2]) };
        // Trivially outside one side plane: skip.
        bool out = true;
        for (int k = 0; k < 3 && out; k++) out = in[k].x > in[k].w;
        if (out) continue;
        out = true;
        for (int k = 0; k < 3 && out; k++) out = in[k].x < -in[k].w;
        if (out) continue;
        out = true;
        for (int k = 0; k < 3 && out; k++) out = in[k].y > in[k].w;
        if (out) continue;
        out = true;
        for (int k = 0; k < 3 && out; k++) out = in[k].y < -in[k].w;
        if (out) continue;
        // Clip against the near plane (z + w >= 0).
        ClipV poly[4];
        int np = 0;
        for (int k = 0; k < 3; k++) {
            const ClipV& a = in[k];
            const ClipV& b = in[(k + 1) % 3];
            float da = a.z + a.w, db = b.z + b.w;
            if (da >= 0) poly[np++] = a;
            if ((da >= 0) != (db >= 0)) {
                float s = da / (da - db);
                poly[np++] = { a.x + (b.x - a.x) * s, a.y + (b.y - a.y) * s, a.z + (b.z - a.z) * s, a.w + (b.w - a.w) * s };
            }
        }
        if (np < 3) continue;
        float sx[4], sy[4], sz[4];
        bool bad = false;
        for (int k = 0; k < np; k++) {
            if (!(poly[k].w > 1e-6f)) { bad = true; break; }
            float iw = 1.0f / poly[k].w;
            sx[k] = (poly[k].x * iw * 0.5f + 0.5f) * W;
            sy[k] = (0.5f - poly[k].y * iw * 0.5f) * H;
            sz[k] = poly[k].z * iw;
        }
        if (bad) continue;
        (*tris)++;
        for (int f = 1; f + 1 < np; f++) {
            int i0 = 0, i1 = f, i2 = f + 1;
            // Screen y points down: a counter-clockwise front face has a negative area here.
            float area = (sx[i1] - sx[i0]) * (sy[i2] - sy[i0]) - (sx[i2] - sx[i0]) * (sy[i1] - sy[i0]);
            if (mirrored ? area <= 0 : area >= 0) continue;
            if (area > 0) { int tmp = i1; i1 = i2; i2 = tmp; area = -area; }
            float minx = fminf_(sx[i0], fminf_(sx[i1], sx[i2])), maxx = fmaxf_(sx[i0], fmaxf_(sx[i1], sx[i2]));
            float miny = fminf_(sy[i0], fminf_(sy[i1], sy[i2])), maxy = fmaxf_(sy[i0], fmaxf_(sy[i1], sy[i2]));
            int x0 = (int)fmaxf_(minx, 0.0f), x1 = (int)fminf_(maxx, W - 1.0f);
            int y0 = (int)fmaxf_(miny, 0.0f), y1 = (int)fminf_(maxy, H - 1.0f);
            if (x0 > x1 || y0 > y1) continue;
            float inv = 1.0f / -area;
            for (int y = y0; y <= y1; y++) {
                float py = (float)y + 0.5f;
                float* row = depth_ + (size_t)y * (size_t)w_;
                for (int x = x0; x <= x1; x++) {
                    float px = (float)x + 0.5f;
                    // Edge functions (counter-clockwise in screen space after the swap above).
                    float e0 = (sx[i2] - sx[i1]) * (py - sy[i1]) - (sy[i2] - sy[i1]) * (px - sx[i1]);
                    float e1 = (sx[i0] - sx[i2]) * (py - sy[i2]) - (sy[i0] - sy[i2]) * (px - sx[i2]);
                    float e2 = (sx[i1] - sx[i0]) * (py - sy[i0]) - (sy[i1] - sy[i0]) * (px - sx[i0]);
                    if (e0 > 0 || e1 > 0 || e2 > 0) continue;
                    float z = (e0 * sz[i0] + e1 * sz[i1] + e2 * sz[i2]) * -inv;
                    if (z < row[x]) row[x] = z;
                }
            }
        }
    }
}

void Culler::build_pyramid() {
    for (int l = 1; l < levels_; l++) {
        int pw = level_w_[l - 1], ph = level_h_[l - 1], lw = level_w_[l], lh = level_h_[l];
        const float* src = level_[l - 1];
        float* dst = level_[l];
        for (int y = 0; y < lh; y++)
            for (int x = 0; x < lw; x++) {
                int sx0 = x * 2, sy0 = y * 2;
                int sx1 = sx0 + 1 < pw ? sx0 + 1 : sx0, sy1 = sy0 + 1 < ph ? sy0 + 1 : sy0;
                float m = fmaxf_(fmaxf_(src[sy0 * pw + sx0], src[sy0 * pw + sx1]), fmaxf_(src[sy1 * pw + sx0], src[sy1 * pw + sx1]));
                dst[y * lw + x] = m;
            }
    }
}

bool Culler::box_hidden(const Mat4& vp, const Vec3& lo, const Vec3& hi) const {
    float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f, zmin = 1e30f;
    for (int i = 0; i < 8; i++) {
        Vec3 p((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
        ClipV c = mul(vp, p);
        if (!(c.w > 1e-5f) || c.z < -c.w) return false;     // reaches the near plane: visible
        float iw = 1.0f / c.w;
        float x = (c.x * iw * 0.5f + 0.5f) * (float)w_, y = (0.5f - c.y * iw * 0.5f) * (float)h_;
        minx = fminf_(minx, x); maxx = fmaxf_(maxx, x);
        miny = fminf_(miny, y); maxy = fmaxf_(maxy, y);
        zmin = fminf_(zmin, c.z * iw);
    }
    int x0 = (int)fmaxf_(minx - 1.0f, 0.0f), x1 = (int)fminf_(maxx + 1.0f, (float)(w_ - 1));
    int y0 = (int)fmaxf_(miny - 1.0f, 0.0f), y1 = (int)fminf_(maxy + 1.0f, (float)(h_ - 1));
    if (x0 > x1 || y0 > y1) return false;
    int l = 0;
    while (l + 1 < levels_ && ((x1 >> l) - (x0 >> l) > 3 || (y1 >> l) - (y0 >> l) > 3)) l++;
    const float* lv = level_[l];
    int lw = level_w_[l];
    for (int y = y0 >> l; y <= (y1 >> l); y++)
        for (int x = x0 >> l; x <= (x1 >> l); x++)
            if (lv[y * lw + x] >= zmin - kDepthEps) return false;
    return true;
}

int Culler::run(const RenderScene& s, bool occlusion, int* out, CullStats* st) {
    double t0 = clock_ms();
    CullStats local;
    CullStats& S = st ? *st : local;
    S = CullStats();
    int n = s.item_count;
    float aspect = s.camera.proj.at(0, 0) != 0 ? s.camera.proj.at(1, 1) / s.camera.proj.at(0, 0) : 1.0f;
    int dh = (int)((float)kDepthWidth / fclamp(aspect, 0.25f, 4.0f) + 0.5f);
    if (dh < 16) dh = 16;
    if (dh > 1024) dh = 1024;
    if (!ensure(n, kDepthWidth, dh)) {
        for (int i = 0; i < n; i++) out[i] = i;
        return n;
    }
    const Mat4 vp = s.camera.proj * s.camera.view;
    // Frustum planes (Gribb & Hartmann): left, right, bottom, top, far. Points behind the camera fail the
    // side planes together, so the near plane is not needed.
    float pl[5][4];
    for (int r = 0; r < 4; r++) {
        float r0 = vp.at(0, r), r1 = vp.at(1, r), r2 = vp.at(2, r), r3 = vp.at(3, r);
        pl[0][r] = r3 + r0; pl[1][r] = r3 - r0; pl[2][r] = r3 + r1; pl[3][r] = r3 - r1; pl[4][r] = r3 - r2;
    }
    Vec3 eye = s.camera.position;
    int visible = 0;
    for (int i = 0; i < n; i++) {
        const DrawItem& it = s.items[i];
        Vec3 lo, hi;
        state_[i] = 1;
        S.tested++;
        if (!local_bounds(it.mesh, &lo, &hi)) { state_[i] = 0; continue; }   // nothing to draw
        // World box of the transformed local box (centre + extents through |M|).
        Vec3 c = (lo + hi) * 0.5f, e = (hi - lo) * 0.5f;
        const Mat4& m = it.model;
        Vec3 wc = transform_point(m, c);
        Vec3 we(fabsf_(m.at(0, 0)) * e.x + fabsf_(m.at(0, 1)) * e.y + fabsf_(m.at(0, 2)) * e.z,
                fabsf_(m.at(1, 0)) * e.x + fabsf_(m.at(1, 1)) * e.y + fabsf_(m.at(1, 2)) * e.z,
                fabsf_(m.at(2, 0)) * e.x + fabsf_(m.at(2, 1)) * e.y + fabsf_(m.at(2, 2)) * e.z);
        wlo_[i] = wc - we;
        whi_[i] = wc + we;
        bool in = true;
        for (int p = 0; p < 5 && in; p++) {
            // The box corner furthest along the plane normal.
            float d = pl[p][0] * (pl[p][0] >= 0 ? whi_[i].x : wlo_[i].x) + pl[p][1] * (pl[p][1] >= 0 ? whi_[i].y : wlo_[i].y) +
                      pl[p][2] * (pl[p][2] >= 0 ? whi_[i].z : wlo_[i].z) + pl[p][3];
            in = d >= 0;
        }
        if (!in) { state_[i] = 0; S.frustum_culled++; continue; }
        visible++;
    }
    if (occlusion && visible > 1) {
        for (int i = 0; i < w_ * h_; i++) depth_[i] = 1.0f;
        // Occluders: the biggest items for their distance first, up to a triangle budget.
        int cand_n = 0;
        int* cand = out;                    // scratch: `out` is written only at the end
        float* score = (float*)malloc(sizeof(float) * (size_t)(n ? n : 1));
        if (score) {
            for (int i = 0; i < n; i++) {
                if (!state_[i]) continue;
                const DrawItem& it = s.items[i];
                if (!it.occluder || it.foliage || it.alpha_cut > 0.0f || (it.shader_path && it.shader_path[0])) continue;
                bool has_occ = it.mesh.occluder_positions && it.mesh.occluder_count;
                if (!has_occ && (!it.mesh.positions || it.mesh.vertex_count > kMaxOccluderVertices)) continue;
                Vec3 c = (wlo_[i] + whi_[i]) * 0.5f;
                float r = length(whi_[i] - wlo_[i]) * 0.5f;
                float d = length(c - eye);
                float k = d > r ? r / d : 10.0f;
                if (k < (has_occ ? 0.02f : 0.08f)) continue;
                score[i] = k;
                cand[cand_n++] = i;
            }
            // Sort the candidates by score (insertion sort: there are few).
            for (int a = 1; a < cand_n; a++) {
                int v = cand[a];
                int b = a - 1;
                while (b >= 0 && score[cand[b]] < score[v]) { cand[b + 1] = cand[b]; b--; }
                cand[b + 1] = v;
            }
            int tris = 0;
            for (int q = 0; q < cand_n && tris < kMaxOccluderTriangles; q++) {
                const DrawItem& it = s.items[cand[q]];
                const float* mm = it.model.m;
                Vec3 c0(mm[0], mm[1], mm[2]), c1(mm[4], mm[5], mm[6]), c2(mm[8], mm[9], mm[10]);
                bool mirrored = dot(c0, cross(c1, c2)) < 0.0f;
                Mat4 mvp = vp * it.model;
                bool has_occ = it.mesh.occluder_positions && it.mesh.occluder_count;
                if (has_occ) raster(it.mesh.occluder_positions, it.mesh.occluder_count, mvp, mirrored, &tris);
                else raster(it.mesh.positions, it.mesh.vertex_count / 3 * 3, mvp, mirrored, &tris);
                state_[cand[q]] = 2;
                S.occluders++;
            }
            S.occluder_triangles = tris;
            free(score);
        }
        if (S.occluders > 0) {
            build_pyramid();
            for (int i = 0; i < n; i++) {
                if (!state_[i]) continue;
                if (box_hidden(vp, wlo_[i], whi_[i])) { state_[i] = 0; S.occlusion_culled++; }
            }
        }
    }
    int k = 0;
    for (int i = 0; i < n; i++)
        if (state_[i]) out[k++] = i;
    S.ms = clock_ms() - t0;
    return k;
}

}  // namespace fe::gfx
