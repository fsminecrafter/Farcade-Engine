#include "gfx/gi.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fe::gfx {
namespace {

constexpr int D = GiSystem::kVoxelDim;
constexpr float VS = GiSystem::kVoxelSize;
constexpr int PX = GiSystem::kProbesX, PY = GiSystem::kProbesY, PZ = GiSystem::kProbesZ;
constexpr float kPi = 3.14159265f;

double now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
}

float srgb_to_lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

struct Voxel {
    float albedo[3];
    float nrm[3];                           // summed face normals, then normalized
    float rad[3];                           // outgoing radiance after injection
    float emit[3];
    bool occupied;
};

// One probe: ambient coefficients (what the shader adds up): a0 rgb, then the x, y, z terms.
struct Probe {
    float a[4][3];
    int32_t cell[3];
    bool valid_cell = false;                // the slot holds `cell`
    bool initialized = false;
    bool fallback = false;                  // inside geometry or seeing back faces
};

uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
float frand(uint32_t& s) { s = hash32(s + 0x9e3779b9U); return (float)(s >> 8) * (1.0f / 16777216.0f); }

// Triangle / box overlap (Akenine-Moller): box centre c, half size h.
bool tri_box(const Vec3& c, float h, const Vec3& a0, const Vec3& b0, const Vec3& c0) {
    Vec3 v0 = a0 - c, v1 = b0 - c, v2 = c0 - c;
    Vec3 e0 = v1 - v0, e1 = v2 - v1, e2 = v0 - v2;
    const Vec3 edges[3] = { e0, e1, e2 };
    const Vec3 verts[3] = { v0, v1, v2 };
    // the nine edge cross axes
    for (int i = 0; i < 3; i++) {
        Vec3 e = edges[i];
        const Vec3 axes[3] = { Vec3(0, -e.z, e.y), Vec3(e.z, 0, -e.x), Vec3(-e.y, e.x, 0) };
        for (int k = 0; k < 3; k++) {
            const Vec3& ax = axes[k];
            float p0 = dot(ax, verts[0]), p1 = dot(ax, verts[1]), p2 = dot(ax, verts[2]);
            float mn = fminf_(p0, fminf_(p1, p2)), mx = fmaxf_(p0, fmaxf_(p1, p2));
            float r = h * (fabsf_(ax.x) + fabsf_(ax.y) + fabsf_(ax.z));
            if (mn > r || mx < -r) return false;
        }
    }
    // the box axes
    for (int k = 0; k < 3; k++) {
        float mn = 1e30f, mx = -1e30f;
        for (int i = 0; i < 3; i++) {
            float p = k == 0 ? verts[i].x : (k == 1 ? verts[i].y : verts[i].z);
            mn = fminf_(mn, p);
            mx = fmaxf_(mx, p);
        }
        if (mn > h || mx < -h) return false;
    }
    // the triangle's plane
    Vec3 n = cross(e0, e1);
    float d = dot(n, v0);
    float r = h * (fabsf_(n.x) + fabsf_(n.y) + fabsf_(n.z));
    return fabsf_(d) <= r;
}

}  // namespace

struct GiSystem::Impl {
    Voxel* vox = nullptr;
    int32_t* surface = nullptr;             // indices of occupied voxels
    int surface_count = 0;
    int32_t origin[3] = {};                 // voxel cell of the grid's min corner
    bool have_grid = false;
    uint64_t scene_hash = 0;
    // Settled: nothing the GI sees changed while every probe was updated many times over (the probes have
    // converged): updates stop until something changes. quiet_updates counts probe updates since then.
    uint64_t quiet_hash = 0;
    int64_t quiet_updates = 0;
    double since_voxelize = 1e9;            // seconds
    int inject_cursor = 0;

    Probe probes[kProbeCount];
    int32_t win[3] = {};                    // probe cell of the window's min corner
    bool have_win = false;
    int probe_cursor = 0;
    uint64_t frame = 0, version = 1;
    float spacing = 1.0f;
    float intensity = 1.0f;
    GiStats stats;
    Vec3 sky, ground;

    Impl() {
        vox = (Voxel*)calloc((size_t)D * D * D, sizeof(Voxel));
        surface = (int32_t*)malloc(sizeof(int32_t) * (size_t)D * D * D);
    }
    ~Impl() { free(vox); free(surface); }

    int vidx(int x, int y, int z) const { return (z * D + y) * D + x; }

    // ---- the hemisphere: what a ray that leaves the grid sees
    Vec3 sky_at(const Vec3& d) const {
        float t = d.y * 0.5f + 0.5f;
        return ground * (1 - t) + sky * t;
    }

    // ---- scene hash
    static uint64_t fnv(uint64_t h, const void* p, size_t n) {
        const unsigned char* c = (const unsigned char*)p;
        for (size_t i = 0; i < n; i++) { h ^= c[i]; h *= 1099511628211ull; }
        return h;
    }
    uint64_t hash_scene(const GiInput& in) const {
        uint64_t h = 1469598103934665603ull;
        for (int i = 0; i < in.item_count; i++) {
            const DrawItem& it = in.items[i];
            if (it.foliage) continue;
            h = fnv(h, it.model.m, sizeof it.model.m);
            h = fnv(h, &it.mesh.key, sizeof it.mesh.key);
            h = fnv(h, &it.mesh.vertex_count, sizeof it.mesh.vertex_count);
            h = fnv(h, &it.color, sizeof it.color);
            h = fnv(h, &it.unlit, sizeof it.unlit);
            if (!it.mesh.key) h = fnv(h, &frame, sizeof frame);       // uncached meshes: new every frame
        }
        return h;
    }

    // ---- voxelization (conservative: every voxel a triangle touches)
    void voxelize(const GiInput& in) {
        memset(vox, 0, sizeof(Voxel) * (size_t)D * D * D);
        for (int i = 0; i < in.item_count; i++) {
            const DrawItem& it = in.items[i];
            if (!it.mesh.positions || it.foliage) continue;
            float alb[3] = { srgb_to_lin(it.color.x), srgb_to_lin(it.color.y), srgb_to_lin(it.color.z) };
            uint32_t vc = it.mesh.vertex_count / 3 * 3;
            for (uint32_t t = 0; t < vc; t += 3) {
                Vec3 a = transform_point(it.model, it.mesh.positions[t]);
                Vec3 b = transform_point(it.model, it.mesh.positions[t + 1]);
                Vec3 c = transform_point(it.model, it.mesh.positions[t + 2]);
                Vec3 fn = cross(b - a, c - a);
                float fl = length(fn);
                if (fl < 1e-12f) continue;
                fn = fn * (1.0f / fl);
                Vec3 lo = vmin(a, vmin(b, c)), hi = vmax(a, vmax(b, c));
                int x0 = (int)floorf(lo.x / VS) - origin[0], x1 = (int)floorf(hi.x / VS) - origin[0];
                int y0 = (int)floorf(lo.y / VS) - origin[1], y1 = (int)floorf(hi.y / VS) - origin[1];
                int z0 = (int)floorf(lo.z / VS) - origin[2], z1 = (int)floorf(hi.z / VS) - origin[2];
                if (x1 < 0 || y1 < 0 || z1 < 0 || x0 >= D || y0 >= D || z0 >= D) continue;
                x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0; z0 = z0 < 0 ? 0 : z0;
                x1 = x1 >= D ? D - 1 : x1; y1 = y1 >= D ? D - 1 : y1; z1 = z1 >= D ? D - 1 : z1;
                for (int z = z0; z <= z1; z++)
                    for (int y = y0; y <= y1; y++)
                        for (int x = x0; x <= x1; x++) {
                            Vec3 centre(((float)(x + origin[0]) + 0.5f) * VS, ((float)(y + origin[1]) + 0.5f) * VS,
                                        ((float)(z + origin[2]) + 0.5f) * VS);
                            if (!tri_box(centre, VS * 0.5f, a, b, c)) continue;
                            Voxel& v = vox[vidx(x, y, z)];
                            if (!v.occupied) {
                                v.occupied = true;
                                memcpy(v.albedo, alb, sizeof alb);
                                if (it.unlit) { v.emit[0] = alb[0]; v.emit[1] = alb[1]; v.emit[2] = alb[2]; }
                            }
                            v.nrm[0] += fn.x; v.nrm[1] += fn.y; v.nrm[2] += fn.z;
                        }
            }
        }
        surface_count = 0;
        for (int i = 0; i < D * D * D; i++) {
            Voxel& v = vox[i];
            if (!v.occupied) continue;
            float l = sqrtf(v.nrm[0] * v.nrm[0] + v.nrm[1] * v.nrm[1] + v.nrm[2] * v.nrm[2]);
            if (l > 1e-6f) { v.nrm[0] /= l; v.nrm[1] /= l; v.nrm[2] /= l; } else { v.nrm[0] = 0; v.nrm[1] = 1; v.nrm[2] = 0; }
            surface[surface_count++] = i;
        }
        stats.voxels = surface_count;
        inject_cursor = 0;
    }

    // ---- a ray through the voxels (3D DDA). Returns the hit voxel index or -1; *t the distance.
    int march(Vec3 o, const Vec3& d, float max_dist, float* t, bool skip_start) const {
        Vec3 g((o.x / VS) - (float)origin[0], (o.y / VS) - (float)origin[1], (o.z / VS) - (float)origin[2]);
        int ix = (int)floorf(g.x), iy = (int)floorf(g.y), iz = (int)floorf(g.z);
        int sx = d.x > 0 ? 1 : -1, sy = d.y > 0 ? 1 : -1, sz = d.z > 0 ? 1 : -1;
        const float inf = 1e30f;
        float tdx = fabsf_(d.x) > 1e-9f ? VS / fabsf_(d.x) : inf;
        float tdy = fabsf_(d.y) > 1e-9f ? VS / fabsf_(d.y) : inf;
        float tdz = fabsf_(d.z) > 1e-9f ? VS / fabsf_(d.z) : inf;
        float tx = fabsf_(d.x) > 1e-9f ? (((d.x > 0 ? (float)(ix + 1) : (float)ix) - g.x) * VS) / d.x : inf;
        float ty = fabsf_(d.y) > 1e-9f ? (((d.y > 0 ? (float)(iy + 1) : (float)iy) - g.y) * VS) / d.y : inf;
        float tz = fabsf_(d.z) > 1e-9f ? (((d.z > 0 ? (float)(iz + 1) : (float)iz) - g.z) * VS) / d.z : inf;
        float cur = 0;
        bool first = true;
        for (int step = 0; step < 4 * D; step++) {
            if (ix >= 0 && iy >= 0 && iz >= 0 && ix < D && iy < D && iz < D) {
                if (!(first && skip_start) && vox[vidx(ix, iy, iz)].occupied) { *t = cur; return vidx(ix, iy, iz); }
            } else if (!first) {
                return -1;                  // left the grid
            }
            first = false;
            if (tx < ty && tx < tz) { cur = tx; tx += tdx; ix += sx; }
            else if (ty < tz) { cur = ty; ty += tdy; iy += sy; }
            else { cur = tz; tz += tdz; iz += sz; }
            if (cur > max_dist) return -1;
        }
        return -1;
    }

    // ---- light injection into one surface voxel
    void inject(int vi, const GiInput& in, const GiParams& p) {
        Voxel& v = vox[vi];
        int x = vi % D, y = (vi / D) % D, z = vi / (D * D);
        Vec3 pos(((float)(x + origin[0]) + 0.5f) * VS, ((float)(y + origin[1]) + 0.5f) * VS, ((float)(z + origin[2]) + 0.5f) * VS);
        Vec3 n(v.nrm[0], v.nrm[1], v.nrm[2]);
        Vec3 start = pos + n * (VS * 0.75f);
        float lr = 0, lg = 0, lb = 0;
        for (int i = 0; i < in.light_count; i++) {
            const RenderLight& l = in.lights[i];
            Vec3 col = l.color * l.intensity;
            if (l.type == LightType::Directional) {
                Vec3 L = normalize(l.direction * -1.0f);
                float ndl = dot(n, L);
                if (ndl <= 0) continue;
                float tt;
                if (l.shadows && march(start, L, 64.0f * VS, &tt, false) >= 0) continue;
                lr += col.x * ndl; lg += col.y * ndl; lb += col.z * ndl;
            } else {
                Vec3 d = l.position - pos;
                float dist = length(d);
                if (dist < 1e-4f || dist > l.range) continue;
                Vec3 L = d * (1.0f / dist);
                float ndl = dot(n, L);
                if (ndl <= 0) continue;
                float x2 = dist / (l.range > 0.001f ? l.range : 0.001f);
                float att = (1 - x2 * x2);
                att *= att;
                if (l.type == LightType::Spot) {
                    Vec3 f = normalize(l.direction);
                    float cd = dot(f, L * -1.0f);
                    float outer = cosf(fclamp(l.spot_angle * 0.5f, 0.5f, 89.0f) * kPi / 180.0f);
                    float inner = cosf(fclamp(l.spot_angle * 0.5f, 0.5f, 89.0f) * kPi / 180.0f * (1.0f - fclamp(l.spot_blend, 0, 1)));
                    if (inner < outer + 1e-4f) inner = outer + 1e-4f;
                    float s = fclamp((cd - outer) / (inner - outer), 0, 1);
                    att *= s * s * (3 - 2 * s);
                }
                lr += col.x * ndl * att; lg += col.y * ndl * att; lb += col.z * ndl * att;
            }
        }
        // the bounce: the irradiance the probes had last frame at this surface
        Vec3 amb = ambient(pos + n * VS, n) * p.bounce;
        lr += amb.x; lg += amb.y; lb += amb.z;
        v.rad[0] = v.albedo[0] * lr + v.emit[0];
        v.rad[1] = v.albedo[1] * lg + v.emit[1];
        v.rad[2] = v.albedo[2] * lb + v.emit[2];
    }

    // ---- probes
    float probe_spacing(GiDensity d) const { return d == GiDensity::Coarse ? 2.0f : (d == GiDensity::Normal ? 1.0f : 0.5f); }
    static int mod(int a, int n) { int r = a % n; return r < 0 ? r + n : r; }
    int slot_of(int cx, int cy, int cz) const { return (mod(cz, PZ) * PY + mod(cy, PY)) * PX + mod(cx, PX); }

    void set_hemisphere(Probe& pr) const {
        // SH of the hemisphere gradient: a0 = mean, y term from the up / down difference
        for (int c = 0; c < 3; c++) {
            float s = c == 0 ? sky.x : (c == 1 ? sky.y : sky.z), g = c == 0 ? ground.x : (c == 1 ? ground.y : ground.z);
            pr.a[0][c] = (s + g) * 0.5f;
            pr.a[1][c] = 0; pr.a[2][c] = (s - g) * 0.5f; pr.a[3][c] = 0;
        }
    }

    void update_probe(Probe& pr, const Vec3& pos, const GiParams& p, uint32_t seed) {
        float c00[3] = {}, c1[3][3] = {};   // radiance projected: Y00, then Y1 for x, y, z
        int n = p.rays < 8 ? 8 : p.rays, back = 0;
        // a random rotation per update, from the seed
        uint32_t s = seed;
        float u1 = frand(s), u2 = frand(s), u3 = frand(s);
        float q0 = sqrtf(1 - u1) * sinf(2 * kPi * u2), q1 = sqrtf(1 - u1) * cosf(2 * kPi * u2);
        float q2 = sqrtf(u1) * sinf(2 * kPi * u3), q3 = sqrtf(u1) * cosf(2 * kPi * u3);
        float R[9] = { 1 - 2 * (q2 * q2 + q3 * q3), 2 * (q1 * q2 - q0 * q3), 2 * (q1 * q3 + q0 * q2),
                       2 * (q1 * q2 + q0 * q3), 1 - 2 * (q1 * q1 + q3 * q3), 2 * (q2 * q3 - q0 * q1),
                       2 * (q1 * q3 - q0 * q2), 2 * (q2 * q3 + q0 * q1), 1 - 2 * (q1 * q1 + q2 * q2) };
        for (int i = 0; i < n; i++) {
            // spherical Fibonacci
            float zz = 1.0f - 2.0f * ((float)i + 0.5f) / (float)n;
            float rr = sqrtf(fmaxf_(0.0f, 1.0f - zz * zz));
            float ph = (float)i * 2.39996323f;
            Vec3 d0(rr * cosf(ph), zz, rr * sinf(ph));
            Vec3 d(R[0] * d0.x + R[1] * d0.y + R[2] * d0.z, R[3] * d0.x + R[4] * d0.y + R[5] * d0.z, R[6] * d0.x + R[7] * d0.y + R[8] * d0.z);
            float t;
            int hit = march(pos, d, 40.0f, &t, false);
            Vec3 L;
            if (hit >= 0) {
                const Voxel& v = vox[hit];
                if (v.nrm[0] * d.x + v.nrm[1] * d.y + v.nrm[2] * d.z > 0.0f && t < VS * 1.5f) { back++; }   // sees the inside of a surface
                L = Vec3(v.rad[0], v.rad[1], v.rad[2]);
            } else {
                L = sky_at(d);
            }
            float w = 4.0f * kPi / (float)n;
            float Lc[3] = { L.x, L.y, L.z };
            for (int c = 0; c < 3; c++) {
                c00[c] += Lc[c] * kShY0 * w;
                c1[0][c] += Lc[c] * kShY1 * d.x * w;
                c1[1][c] += Lc[c] * kShY1 * d.y * w;
                c1[2][c] += Lc[c] * kShY1 * d.z * w;
            }
        }
        pr.fallback = back * 4 > n;        // most rays hit back faces: the probe is inside geometry
        if (pr.fallback) {
            if (!pr.initialized) set_hemisphere(pr);
            pr.initialized = true;
            return;
        }
        float h = pr.initialized ? p.hysteresis : 0.0f;
        for (int c = 0; c < 3; c++) {
            float a0 = c00[c] * kShY0;                         // pi * Y00 * c00 / pi
            float ax = (2.0f / 3.0f) * kShY1 * c1[0][c];
            float ay = (2.0f / 3.0f) * kShY1 * c1[1][c];
            float az = (2.0f / 3.0f) * kShY1 * c1[2][c];
            pr.a[0][c] = pr.a[0][c] * h + a0 * (1 - h);
            pr.a[1][c] = pr.a[1][c] * h + ax * (1 - h);
            pr.a[2][c] = pr.a[2][c] * h + ay * (1 - h);
            pr.a[3][c] = pr.a[3][c] * h + az * (1 - h);
        }
        pr.initialized = true;
    }

    bool occupied_at(const Vec3& p) const;

    // The ambient colour of the probes at a point: trilinear over the 8 probes around it.
    Vec3 ambient(const Vec3& pos, const Vec3& n) const {
        if (!have_win) return (ground + sky) * 0.5f;
        float gx = pos.x / spacing - (float)win[0] - 0.5f, gy = pos.y / spacing - (float)win[1] - 0.5f, gz = pos.z / spacing - (float)win[2] - 0.5f;
        gx = fclamp(gx, 0.0f, (float)(PX - 1)); gy = fclamp(gy, 0.0f, (float)(PY - 1)); gz = fclamp(gz, 0.0f, (float)(PZ - 1));
        int x0 = (int)gx, y0 = (int)gy, z0 = (int)gz;
        int x1 = x0 + 1 < PX ? x0 + 1 : x0, y1 = y0 + 1 < PY ? y0 + 1 : y0, z1 = z0 + 1 < PZ ? z0 + 1 : z0;
        float fx = gx - (float)x0, fy = gy - (float)y0, fz = gz - (float)z0;
        float acc[3] = {};
        for (int k = 0; k < 8; k++) {
            int x = (k & 1) ? x1 : x0, y = (k & 2) ? y1 : y0, z = (k & 4) ? z1 : z0;
            float w = ((k & 1) ? fx : 1 - fx) * ((k & 2) ? fy : 1 - fy) * ((k & 4) ? fz : 1 - fz);
            const Probe& pr = probes[slot_of(win[0] + x, win[1] + y, win[2] + z)];
            for (int c = 0; c < 3; c++) acc[c] += w * (pr.a[0][c] + pr.a[1][c] * n.x + pr.a[2][c] * n.y + pr.a[3][c] * n.z);
        }
        return Vec3(fmaxf_(acc[0], 0.0f), fmaxf_(acc[1], 0.0f), fmaxf_(acc[2], 0.0f)) * intensity;
    }

    void slide_window(const Vec3& cam) {
        int32_t w[3] = { (int32_t)floorf(cam.x / spacing) - PX / 2, (int32_t)floorf(cam.y / spacing) - PY / 2,
                         (int32_t)floorf(cam.z / spacing) - PZ / 2 };
        if (have_win && w[0] == win[0] && w[1] == win[1] && w[2] == win[2]) return;
        memcpy(win, w, sizeof w);
        have_win = true;
        // slots whose cell is no longer the one the window wants start again
        for (int z = 0; z < PZ; z++) for (int y = 0; y < PY; y++) for (int x = 0; x < PX; x++) {
            Probe& pr = probes[slot_of(win[0] + x, win[1] + y, win[2] + z)];
            int32_t cell[3] = { win[0] + x, win[1] + y, win[2] + z };
            if (!pr.valid_cell || pr.cell[0] != cell[0] || pr.cell[1] != cell[1] || pr.cell[2] != cell[2]) {
                memcpy(pr.cell, cell, sizeof cell);
                pr.valid_cell = true;
                pr.initialized = false;
                set_hemisphere(pr);
            }
        }
    }
};

GiSystem::GiSystem() : impl_(new Impl()) {}
GiSystem::~GiSystem() { delete impl_; }
void GiSystem::reset() {
    Impl& m = *impl_;
    m.have_grid = false;
    m.have_win = false;
    m.scene_hash = 0;
    for (Probe& p : m.probes) p = Probe();
    m.version++;
}

void GiSystem::update(const GiInput& in, const GiParams& p, bool force_all) {
    Impl& m = *impl_;
    double t0 = now_ms();
    m.stats = GiStats();
    m.frame++;
    m.sky = in.sky_color;
    m.ground = in.ground_color;
    m.intensity = p.intensity;
    float sp = m.probe_spacing(p.density);
    if (sp != m.spacing) { m.spacing = sp; m.have_win = false; for (Probe& pr : m.probes) pr = Probe(); }
    m.slide_window(in.camera);

    // the voxel grid: centred on the camera, moved when the camera leaves its middle third
    int32_t want[3] = { (int32_t)floorf(in.camera.x / VS) - D / 2, (int32_t)floorf(in.camera.y / VS) - D / 2,
                        (int32_t)floorf(in.camera.z / VS) - D / 2 };
    bool moved = !m.have_grid;
    if (m.have_grid) for (int i = 0; i < 3; i++) if (abs(want[i] - m.origin[i]) > D / 6) moved = true;
    m.since_voxelize += in.dt;
    uint64_t h = m.hash_scene(in);
    bool changed = h != m.scene_hash;
    if (moved || force_all || (changed && m.since_voxelize >= 0.1)) {
        if (moved) memcpy(m.origin, want, sizeof want);
        m.voxelize(in);
        m.have_grid = true;
        m.scene_hash = h;
        m.since_voxelize = 0;
        m.stats.revoxelized = true;
    } else {
        m.stats.voxels = m.surface_count;
    }

    // settled? (the scene, the lights, the sky, the windows and the parameters as they were)
    {
        uint64_t q = Impl::fnv(h, m.origin, sizeof m.origin);
        q = Impl::fnv(q, m.win, sizeof m.win);
        for (int i = 0; i < in.light_count; i++) {
            const RenderLight& L = in.lights[i];
            float f[13] = { L.position.x, L.position.y, L.position.z, L.direction.x, L.direction.y, L.direction.z,
                            L.color.x, L.color.y, L.color.z, L.intensity, L.range, L.spot_angle, L.spot_blend };
            q = Impl::fnv(q, f, sizeof f);
            int ty = (int)L.type;
            q = Impl::fnv(q, &ty, sizeof ty);
        }
        float g[11] = { in.sky_color.x, in.sky_color.y, in.sky_color.z, in.ground_color.x, in.ground_color.y,
                        in.ground_color.z, p.hysteresis, p.bounce, p.intensity, (float)p.rays, m.spacing };
        q = Impl::fnv(q, g, sizeof g);
        int up = (int)p.update;
        q = Impl::fnv(q, &up, sizeof up);
        if (q != m.quiet_hash || force_all) { m.quiet_hash = q; m.quiet_updates = 0; }
        // 0.93^48 ~ 3%: what is left of any older value after 48 updates of a probe.
        if (!force_all && m.quiet_updates >= (int64_t)GiSystem::kProbeCount * 48) {
            m.stats.voxels = m.surface_count;
            m.stats.settled = true;
            m.stats.ms = now_ms() - t0;
            return;                 // version() unchanged: nothing is uploaded either
        }
    }

    // light: a quarter of the surface voxels a frame
    int count = m.surface_count;
    int slice = force_all ? count : (count / 4 > 256 ? count / 4 : (count < 256 ? count : 256));
    for (int i = 0; i < slice && count; i++) {
        m.inject(m.surface[m.inject_cursor], in, p);
        m.inject_cursor = (m.inject_cursor + 1) % count;
    }

    // probes: the fraction of the window per frame, never initialized ones first
    int per = p.update == GiUpdate::Slow ? 32 : (p.update == GiUpdate::Normal ? 16 : 8);
    int todo = force_all ? GiSystem::kProbeCount : GiSystem::kProbeCount / per;
    int updated = 0, invalid = 0;
    auto run_probe = [&](int idx) {
        int x = idx % PX, y = (idx / PX) % PY, z = idx / (PX * PY);
        int cx = m.win[0] + x, cy = m.win[1] + y, cz = m.win[2] + z;
        Probe& pr = m.probes[m.slot_of(cx, cy, cz)];
        Vec3 pos(((float)cx + 0.5f) * m.spacing, ((float)cy + 0.5f) * m.spacing, ((float)cz + 0.5f) * m.spacing);
        // move out of geometry: try the 6 neighbours at a voxel's distance
        if (m.occupied_at(pos)) {
            const Vec3 dirs[6] = { Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(0, -1, 0), Vec3(0, 0, 1), Vec3(0, 0, -1) };
            for (const Vec3& d : dirs) {
                Vec3 q = pos + d * (VS * 1.01f);
                if (!m.occupied_at(q)) { pos = q; break; }
            }
        }
        m.update_probe(pr, pos, p, (uint32_t)(m.frame * 2654435761u) ^ (uint32_t)idx * 40503u);
        updated++;
    };
    // first the probes that never got a value
    for (int i = 0; i < GiSystem::kProbeCount && updated < (force_all ? 1 << 30 : todo); i++) {
        int idx = (m.probe_cursor + i) % GiSystem::kProbeCount;
        int x = idx % PX, y = (idx / PX) % PY, z = idx / (PX * PY);
        if (!m.probes[m.slot_of(m.win[0] + x, m.win[1] + y, m.win[2] + z)].initialized) run_probe(idx);
    }
    for (int i = 0; i < todo && !force_all; i++) {
        if (updated >= todo * 2) break;
        run_probe(m.probe_cursor);
        m.probe_cursor = (m.probe_cursor + 1) % GiSystem::kProbeCount;
    }
    if (force_all) {
        // everything once more with the voxels now lit (the bounce), so a still frame has converged
        for (int pass = 0; pass < 3; pass++) {
            for (int i = 0; i < count; i++) m.inject(m.surface[i], in, p);
            for (int i = 0; i < GiSystem::kProbeCount; i++) run_probe(i);
        }
    }
    for (const Probe& pr : m.probes) if (pr.fallback) invalid++;
    m.stats.probes_updated = updated;
    m.quiet_updates += updated;
    m.stats.probes_invalid = invalid;
    m.version++;
    m.stats.ms = now_ms() - t0;
}

bool GiSystem::Impl::occupied_at(const Vec3& p) const {
    int x = (int)floorf(p.x / VS) - origin[0], y = (int)floorf(p.y / VS) - origin[1], z = (int)floorf(p.z / VS) - origin[2];
    if (x < 0 || y < 0 || z < 0 || x >= D || y >= D || z >= D) return false;
    return vox[vidx(x, y, z)].occupied;
}

Vec3 GiSystem::ambient(const Vec3& pos, const Vec3& n) const { return impl_->ambient(pos, n); }
bool GiSystem::occupied(const Vec3& pos) const { return impl_->occupied_at(pos); }
const GiStats& GiSystem::stats() const { return impl_->stats; }
float GiSystem::spacing() const { return impl_->spacing; }
uint64_t GiSystem::version() const { return impl_->version; }

void GiSystem::uniforms(float gi0[4], float gi1[4]) const {
    const Impl& m = *impl_;
    gi0[0] = (float)m.win[0] * m.spacing; gi0[1] = (float)m.win[1] * m.spacing; gi0[2] = (float)m.win[2] * m.spacing;
    gi0[3] = 1.0f / m.spacing;
    gi1[0] = (float)PX; gi1[1] = (float)PY; gi1[2] = (float)PZ; gi1[3] = m.intensity;
}

// 8 bit encodings: the DC term sqrt(v / 4), the directional terms 0.5 + 0.5 sign sqrt(|v| / 2).
void GiSystem::encode(uint32_t* out) const {
    const Impl& m = *impl_;
    auto enc_dc = [](float v) { return (uint32_t)(fclamp(sqrtf(fclamp(v, 0.0f, 4.0f) * 0.25f), 0.0f, 1.0f) * 255.0f + 0.5f); };
    auto enc_dir = [](float v) {
        float s = sqrtf(fclamp(fabsf_(v), 0.0f, 2.0f) * 0.5f);
        return (uint32_t)(fclamp(0.5f + (v < 0 ? -0.5f : 0.5f) * s, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    for (int z = 0; z < PZ; z++) for (int y = 0; y < PY; y++) for (int x = 0; x < PX; x++) {
        const Probe& pr = m.probes[m.slot_of(m.win[0] + x, m.win[1] + y, m.win[2] + z)];
        for (int c = 0; c < 4; c++) {
            uint32_t r, g, b;
            if (c == 0) { r = enc_dc(pr.a[0][0]); g = enc_dc(pr.a[0][1]); b = enc_dc(pr.a[0][2]); }
            else { r = enc_dir(pr.a[c][0]); g = enc_dir(pr.a[c][1]); b = enc_dir(pr.a[c][2]); }
            out[(y * PZ + z) * kTexW + c * PX + x] = (r << 16) | (g << 8) | b;
        }
    }
}

}  // namespace fe::gfx
