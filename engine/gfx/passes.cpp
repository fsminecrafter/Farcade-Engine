// The GPU tier (engine/gfx/passes.h). A frame, with everything on:
//
//   1. shadow     the casters, from the sun, into a packed-depth color target (own depth surface)
//   2. depth      all items, linear depth packed in 24 bits, at half resolution
//   3. ssao       hemisphere AO from that depth (normals rebuilt from depth), half resolution
//      blur x2    depth aware 5 tap blurs; the result is read by the lit shader
//   4. lit        items (PBR / Blinn-Phong, shadow lookup, AO, fog) + sky, into an HDR target
//      (or straight into the final / anti-aliasing target when no bloom)
//   5. bloom      bright pass to 1/4, three more halvings, a tent upsampling chain
//   6. composite  scene + bloom, tonemap, grade, vignette, sRGB (the base tier's post chain)
//   7. fxaa       FXAA 3.11 from the composite into the context's color surface
//
// Every pass renders into a color surface that the next one samples. A pass
// switches targets by pointing the context's color/depth at the pass's
// surfaces (see set_target) and restores them at the end of the frame.
#include "gfx/passes.h"
#include "gfx/shaders.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fe::gfx {

namespace {

constexpr int kBloomLevels = 4;
constexpr int kMaxLitVariants = 8;
constexpr int kBoundsCache = 128;

double now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
}

FGLMat4 to_fgl(const Mat4& m) {
    FGLMat4 r;
    memcpy(r.m, m.m, sizeof(r.m));
    return r;
}

float luma(const Vec3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

struct Target {
    FGLTexture color = 0;
    FGLSurface depth = 0;
    int w = 0, h = 0;
};

struct LitProg {
    FGLShader* sh = nullptr;
    shaders::HqLitKey key;
    uint64_t last = 0;
    int mat = -1, pbr = -1;
};

struct Bounds {
    Vec3 lo, hi;
    bool any = false;
    void add(const Vec3& p) {
        if (!any) { lo = hi = p; any = true; return; }
        lo = vmin(lo, p);
        hi = vmax(hi, p);
    }
    void add(const Bounds& b) {
        if (!b.any) return;
        add(b.lo);
        add(b.hi);
    }
    Vec3 corner(int i) const { return Vec3((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z); }
};

struct MeshBounds {
    uint64_t key = 0;
    uint32_t count = 0;
    Bounds b;
};

// A point or spot light's place in the shadow atlas (see atlas_pass).
struct LocalShadow {
    int tile = 0;                           // first tile (a point light uses six from here)
    Vec3 s, u, f;                           // spot: the light camera's right, up, forward
    float tn = 1;                           // tan of half the field of view
    float bias = 0;                         // relative depth bias
};

// The shadow map of a frame: matrices and the constants the lit shader needs.
struct ShadowFit {
    bool valid = false;
    Mat4 view, proj, viewproj;
    Vec3 right, up, to_light;
    float meters_per_uv = 1, inv_range = 1, bias = 0, normal_offset = 0, radius_uv = 0;
};

uint64_t fnv(uint64_t h, const void* data, size_t bytes) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < bytes; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

}  // namespace

struct GpuTier::Impl {
    FGLContext* gl = nullptr;
    GpuTierStats stats;
    uint64_t frame = 0;
    char* err = nullptr;
    int err_cap = 0;
    int surfaces = 0, programs = 0;

    FGLBuffer tri = 0;
    // programs
    FGLShader* shadow_p = nullptr;
    FGLShader* local_p = nullptr;           // point / spot casters into the atlas
    FGLShader* depth_p = nullptr;
    FGLShader* normal_p = nullptr;
    FGLShader* skymask_p = nullptr;
    FGLShader* fxaa_mask_p = nullptr;
    FGLShader* ssao_p = nullptr;
    FGLShader* blur_p = nullptr;
    FGLShader* bright_p = nullptr;
    FGLShader* down_p = nullptr;
    FGLShader* up_p = nullptr;
    FGLShader* comp_p[2] = {};
    FGLShader* fxaa_p = nullptr;
    FGLShader* unlit_p[4] = {};             // [fog * 2 + hdr]
    FGLShader* sky_p[2] = {};               // [hdr]
    LitProg lit_p[kMaxLitVariants];
    // targets
    Target shadow, gdepth, gnormal, ao_a, ao_b, scene, ldr;
    Target atlas;
    uint64_t atlas_hash = 0;
    LocalShadow locals[kMaxLocalShadows];
    int local_count = 0;
    Target bl_down[kBloomLevels], bl_up[kBloomLevels - 1];
    uint64_t shadow_hash = 0;
    MeshBounds bounds_cache[kBoundsCache];
    int bounds_next = 0;
    // the main targets of the context while a pass has its own bound
    FGLSurface main_color = 0, main_depth = 0;

    bool fail(const char* what) {
        if (err && err_cap) snprintf(err, (size_t)err_cap, "%s", what);
        return false;
    }

    // ---- objects

    bool compile(FGLShader** out, const char* vs, const char* fs, const char* what) {
        if (*out) return true;
        FGLShader* sh = fglCreateShader(gl, vs, fs);
        if (!sh) {
            if (err && err_cap) snprintf(err, (size_t)err_cap, "%s shader: %s", what, fglShaderLog(gl));
            if (getenv("FE_DUMP_SHADER")) fprintf(stderr, "%s\n", fs);
            return false;
        }
        *out = sh;
        programs++;
        return true;
    }

    void drop(FGLShader** s) {
        if (*s) { fglDestroyShader(gl, *s); programs--; }
        *s = nullptr;
    }

    // Compiles a program from generator functions (vertex, then fragment).
    template <class V, class F>
    bool make(FGLShader** out, V vgen, F fgen, const char* what) {
        if (*out) return true;
        char* vs = (char*)malloc(shaders::kHqSourceBytes);
        char* fs = (char*)malloc(shaders::kHqSourceBytes);
        if (!vs || !fs) { free(vs); free(fs); return fail("out of memory"); }
        vgen(vs, shaders::kHqSourceBytes);
        fgen(fs, shaders::kHqSourceBytes);
        bool ok = compile(out, vs, fs, what);
        free(vs);
        free(fs);
        return ok;
    }

    bool make_tri() {
        if (tri) return true;
        static const float v[18] = { -1, -1, 0, 0, 0, 0,   3, -1, 0, 0, 0, 0,   -1, 3, 0, 0, 0, 0 };
        tri = fglCreateBuffer(gl, sizeof v);
        if (!tri || fglBufferData(gl, tri, 0, v, sizeof v) != 0) return fail("FarcadeGL: cannot create the pass buffer");
        return true;
    }

    // (Re)creates a target when its size or the need for a depth surface changed.
    // Returns false on failure; *fresh tells that the contents are new (zero).
    bool ensure(Target& t, int w, int h, bool depth, bool* fresh = nullptr) {
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        if (t.color && t.w == w && t.h == h && ((t.depth != 0) == depth)) return true;
        free_target(t);
        t.color = fglCreateTexture(gl, (uint32_t)w, (uint32_t)h, nullptr);
        if (!t.color) return fail("FarcadeGL: cannot create a render target");
        surfaces++;
        if (depth) {
            t.depth = fgl__surface((uint32_t)w, (uint32_t)h, FGPU_FORMAT_DEPTH32F, 0);
            if (!t.depth) { free_target(t); return fail("FarcadeGL: cannot create a depth target"); }
            surfaces++;
        }
        t.w = w;
        t.h = h;
        if (fresh) *fresh = true;
        return true;
    }

    void free_target(Target& t) {
        if (t.color) { fglDestroyTexture(gl, t.color); surfaces--; }
        if (t.depth) { fgl__destroy(SYS_GPU_DESTROY_SURFACE, t.depth); surfaces--; }
        t = Target();
    }

    // ---- passes

    // Starts a pass into (color, depth): finishes the recorded commands, then
    // points the context at the new surfaces. The context rebinds its
    // color/depth after every submit (also the budget submits of draw_mesh), so
    // the fields stay changed until restore_main().
    void set_target(FGLSurface color, FGLSurface depth) {
        fglSubmit(gl);
        gl->color = color;
        gl->depth = depth;
        fgl__bind(gl);
        for (uint32_t u = 0; u < FGPU_MAX_TEXTURE_UNITS; u++) gl->textures[u] = 0;   // nothing bound from the last pass
        gl->textures_dirty = 1;
        stats.passes++;
    }

    void restore_main() {
        fglSubmit(gl);
        gl->color = main_color;
        gl->depth = main_depth;
        fgl__bind(gl);
    }

    // Ends a pass and adds its time to *ms.
    bool end_pass(double t0, double* ms) {
        if (fglSubmit(gl) != 0) {
            char b[96];
            snprintf(b, sizeof b, "FarcadeGL: submit failed (error %d)", gl->error);
            return fail(b);
        }
        *ms += now_ms() - t0;
        return true;
    }

    // A fullscreen pass into `dst` with `sh`; the caller has set its uniforms and
    // textures (bind_tex) before.
    void fs_begin(const Target& dst) {
        set_target(dst.color, 0);
        fglDisable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
    }
    void fs_draw(FGLShader* sh) {
        fglUseShader(gl, sh);
        fglSetModel(gl, to_fgl(Mat4::identity()));
        fglDraw(gl, tri, 0, 3);
    }
    void bind_tex(FGLShader* sh, const char* name, const Target& t, uint32_t flags) {
        fglUniformTexture(gl, sh, fglUniformLocation(sh, name), t.color, flags);
    }
    static void u4(FGLShader* sh, const char* name, float x, float y, float z, float w) {
        fglUniform4f(sh, fglUniformLocation(sh, name), x, y, z, w);
    }

    void set_post(FGLShader* sh, const PostProcess& p, int w, int h) {
        u4(sh, "u_post", p.exposure, p.contrast, p.saturation, fclamp(p.vignette, 0.0f, 1.0f));
        u4(sh, "u_tint", p.tint.x, p.tint.y, p.tint.z, (float)(int)p.tonemap);
        u4(sh, "u_res", (float)w, (float)h, 0, 0);
    }

    // ---- camera

    struct Cam {
        bool ortho = false;
        float tx = 1, ty = 1, n = 0.1f, f = 500;
        Vec3 pos, right, up, fwd;
    };

    static Cam read_camera(const RenderCamera& c) {
        Cam k;
        const Mat4& v = c.view;
        const Mat4& p = c.proj;
        k.right = Vec3(v.m[0], v.m[4], v.m[8]);
        k.up = Vec3(v.m[1], v.m[5], v.m[9]);
        k.fwd = Vec3(-v.m[2], -v.m[6], -v.m[10]);
        k.pos = c.position;
        k.ortho = p.m[15] == 1.0f && p.m[11] == 0.0f;
        k.tx = p.m[0] != 0 ? 1.0f / fabsf_(p.m[0]) : 1.0f;      // half extents when orthographic
        k.ty = p.m[5] != 0 ? 1.0f / fabsf_(p.m[5]) : 1.0f;
        if (!k.ortho) {
            float a = p.m[10] - 1.0f, b = p.m[10] + 1.0f;
            if (a != 0 && b != 0) { k.n = p.m[14] / a; k.f = p.m[14] / b; }
        } else if (p.m[10] != 0) {
            float range = -2.0f / p.m[10];
            k.n = (p.m[14] + 1.0f) / p.m[10];
            k.f = k.n + range;
        }
        if (!(k.n > 0.0f) && !k.ortho) k.n = c.near_plane;
        if (!(k.f > k.n)) { k.n = c.near_plane; k.f = c.far_plane; }
        return k;
    }

    // ---- bounds

    Bounds mesh_bounds(const RenderMesh& m) {
        Bounds b;
        uint32_t vc = m.vertex_count / 3 * 3;
        if (!m.positions || !vc) return b;
        if (m.key) {
            for (const MeshBounds& e : bounds_cache)
                if (e.key == m.key && e.count == vc) return e.b;
        }
        for (uint32_t i = 0; i < vc; i++) b.add(m.positions[i]);
        if (m.key) {
            MeshBounds& e = bounds_cache[bounds_next++ % kBoundsCache];
            e.key = m.key;
            e.count = vc;
            e.b = b;
        }
        return b;
    }

    Bounds item_bounds(const DrawItem& it) {
        Bounds local = mesh_bounds(it.mesh), w;
        if (!local.any) return w;
        for (int i = 0; i < 8; i++) w.add(transform_point(it.model, local.corner(i)));
        return w;
    }

    // ---- shadows

    // Fits an orthographic light camera to the part of the view frustum that
    // matters: the slice [near, min(distance, farthest receiver)] is wrapped in
    // a sphere (rotation does not change it) and, when smaller, the scene's
    // bounding sphere replaces it. The radius is quantized to steps of 2^(1/4)
    // and the center snapped to whole shadow texels in light space, so the map
    // does not shimmer while the camera moves.
    ShadowFit fit_shadow(const RenderScene& s, const Bounds& receivers, const Bounds& casters, Vec3 travel, int size,
                         float distance, bool soft) {
        ShadowFit out;
        if (!receivers.any || !casters.any) return out;
        Vec3 dir = normalize(travel);
        if (!(length(dir) > 0.5f)) return out;
        Cam c = read_camera(s.camera);
        float far_fit = fminf_(c.f, distance);
        float maxd = c.n;
        for (int i = 0; i < 8; i++) maxd = fmaxf_(maxd, dot(receivers.corner(i) - c.pos, c.fwd));
        far_fit = fminf_(far_fit, fmaxf_(maxd, c.n + 0.1f));
        if (!(far_fit > c.n)) far_fit = c.n + 0.1f;

        float zc, radius;
        if (c.ortho) {
            zc = (c.n + far_fit) * 0.5f;
            float hd = (far_fit - c.n) * 0.5f;
            radius = fsqrt(hd * hd + c.tx * c.tx + c.ty * c.ty);
        } else {
            float t2 = c.tx * c.tx + c.ty * c.ty;
            zc = (far_fit + c.n) * (1.0f + t2) * 0.5f;
            if (zc > far_fit) zc = far_fit;
            float rf = fsqrt((far_fit - zc) * (far_fit - zc) + far_fit * far_fit * t2);
            float rn = fsqrt((zc - c.n) * (zc - c.n) + c.n * c.n * t2);
            radius = fmaxf_(rf, rn);
        }
        Vec3 center = c.pos + c.fwd * zc;
        Vec3 sc = (receivers.lo + receivers.hi) * 0.5f;
        float sr = length(receivers.hi - receivers.lo) * 0.5f;
        if (sr < radius) { radius = sr; center = sc; }
        if (radius < 0.5f) radius = 0.5f;
        float q = 0.5f;
        for (int i = 0; i < 80 && q < radius; i++) q *= 1.1892071f;
        radius = q;

        Vec3 up_ref = fabsf_(dir.y) < 0.99f ? Vec3(0, 1, 0) : Vec3(0, 0, 1);
        Mat4 v0 = Mat4::look_at(Vec3(0, 0, 0), dir, up_ref);
        Vec3 right(v0.m[0], v0.m[4], v0.m[8]), up(v0.m[1], v0.m[5], v0.m[9]);
        float texel = 2.0f * radius / (float)size;
        float cx = dot(center, right), cy = dot(center, up), cz = dot(center, dir);
        cx = floorf_(cx / texel + 0.5f) * texel;
        cy = floorf_(cy / texel + 0.5f) * texel;
        Vec3 snapped = right * cx + up * cy + dir * cz;

        // casters between the light and the sphere set how far back the light starts
        float tmin = 0;
        for (int i = 0; i < 8; i++) tmin = fminf_(tmin, dot(casters.corner(i), dir) - cz);
        float extra = fmaxf_(0.0f, -tmin - radius);
        if (extra > 8.0f * radius) extra = 8.0f * radius;
        float step = 0.25f * radius;
        extra = (float)(int)(extra / step + 0.999f) * step;
        float lead = radius + extra;
        float range = lead + radius * 1.05f;
        Vec3 eye = snapped - dir * lead;

        out.view = Mat4::look_at(eye, eye + dir, up_ref);
        out.proj = Mat4::orthographic(radius, 1.0f, 0.0f, range);
        out.viewproj = out.proj * out.view;
        out.right = right;
        out.up = up;
        out.to_light = dir * -1.0f;
        out.meters_per_uv = 2.0f * radius;
        out.inv_range = 1.0f / range;
        out.bias = 1.5f * texel / range;
        out.normal_offset = 1.5f * texel;
        float r_tex = soft ? fclamp(0.06f / texel, 1.5f, 4.0f) : 0.0f;
        out.radius_uv = r_tex / (float)size;
        out.valid = true;
        return out;
    }

    static float floorf_(float x) { return (float)(int)(x < 0 ? x - 0.999999f : x); }

    // ---- lit shaders

    LitProg* lit_variant(const shaders::HqLitKey& key) {
        LitProg* slot = nullptr;
        for (LitProg& v : lit_p) {
            if (v.sh && v.key == key) { v.last = frame; return &v; }
        }
        for (LitProg& v : lit_p) if (!v.sh) { slot = &v; break; }
        if (!slot) {
            slot = &lit_p[0];
            for (LitProg& v : lit_p) if (v.last < slot->last) slot = &v;
            drop(&slot->sh);
        }
        char* vs = (char*)malloc(shaders::kHqSourceBytes);
        char* fs = (char*)malloc(shaders::kHqSourceBytes);
        if (!vs || !fs) { free(vs); free(fs); fail("out of memory"); return nullptr; }
        shaders::hq_lit_vertex(vs, shaders::kHqSourceBytes, key.shadow != 0);
        shaders::hq_lit_fragment(fs, shaders::kHqSourceBytes, key);
        FGLShader* sh = nullptr;
        bool ok = compile(&sh, vs, fs, "lit");
        free(vs);
        free(fs);
        if (!ok) return nullptr;
        slot->sh = sh;
        slot->key = key;
        slot->last = frame;
        slot->mat = fglUniformLocation(sh, "u_mat");
        slot->pbr = fglUniformLocation(sh, "u_pbr");
        return slot;
    }

    // ---- the frame

    bool draw_casters(const RenderScene& s, const PassHost& host) {
        bool failed = false;
        for (int i = 0; i < s.item_count && !failed; i++) {
            const DrawItem& it = s.items[i];
            if (!it.cast_shadows) continue;
            host.draw_mesh(host.user, it, false, &failed);
        }
        return !failed;
    }

    bool shadow_pass(const RenderScene& s, const PassHost& host, const ShadowFit& fit, int size, uint64_t hash) {
        bool fresh = false;
        if (!ensure(shadow, size, size, true, &fresh)) return false;
        if (fresh) shadow_hash = 0;
        if (hash && hash == shadow_hash) { stats.shadow_reused = true; return true; }
        double t0 = now_ms();
        if (!make(&shadow_p, shaders::hq_shadow_vertex, shaders::hq_shadow_fragment, "shadow")) return false;
        set_target(shadow.color, shadow.depth);
        fglClearColor(gl, 1, 1, 1);
        fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
        fglSetProjection(gl, to_fgl(fit.proj));
        fglSetView(gl, to_fgl(fit.view));
        fglEnable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
        fglUseShader(gl, shadow_p);
        bool ok = draw_casters(s, host);
        if (!end_pass(t0, &stats.shadow_ms) || !ok) return false;
        shadow_hash = hash;
        return true;
    }

    // ---- point and spot shadows

    static constexpr float kPointTan = 1.08f;    // a cube face sees a little more than 90 degrees

    int atlas_size(const GraphicsSettings& gs, bool hw) const {
        int size = gs.local_shadow_resolution;
        if (size < 512) size = 512;
        if (size > 4096) size = 4096;
        if (!hw && size > 1024) size = 1024;     // clears and fills cost too much on the CPU backend
        return size / kAtlasTiles * kAtlasTiles;
    }

    // Perspective with tan(half fov) = tn, square.
    static Mat4 frustum(float tn, float zn, float zf) {
        Mat4 r;
        for (float& v : r.m) v = 0;
        float f = 1.0f / tn;
        r.at(0, 0) = f;
        r.at(1, 1) = f;
        r.at(2, 2) = (zf + zn) / (zn - zf);
        r.at(2, 3) = 2.0f * zf * zn / (zn - zf);
        r.at(3, 2) = -1.0f;
        return r;
    }

    // Renders the casters of every shadowed point / spot light into its
    // tiles: radial distance / range, packed. Skipped when nothing changed.
    bool atlas_pass(const RenderScene& s, const PackedLights& ls, const PassHost& host, const GraphicsSettings& gs, bool hw) {
        local_count = ls.nps + ls.nss;
        int size = atlas_size(gs, hw), T = size / kAtlasTiles;
        bool fresh = false;
        if (!ensure(atlas, size, size, true, &fresh)) return false;
        if (fresh) atlas_hash = 0;
        // where each light goes, and the hash of everything the atlas depends on
        uint64_t hash = 1469598103934665603ull;
        bool cacheable = true;
        int tile = 0;
        for (int j = 0; j < local_count; j++) {
            const RenderLight& l = *ls.local[j];
            LocalShadow& o = locals[j];
            o = LocalShadow();
            o.tile = tile;
            if (l.type == LightType::Point) {
                o.tn = kPointTan;
                tile += 6;
            } else {
                Vec3 f = length(l.direction) > 0.0f ? normalize(l.direction) : Vec3(0, -1, 0);
                Vec3 up = fabsf_(f.y) < 0.99f ? Vec3(0, 1, 0) : Vec3(0, 0, 1);
                o.f = f;
                o.s = normalize(cross(f, up));
                o.u = cross(o.s, f);
                float half = fclamp(l.spot_angle * 0.5f + 2.0f, 2.0f, 75.0f);
                o.tn = ftan(radians(half));
                tile += 1;
            }
            o.bias = 3.0f * o.tn / (float)T;
            hash = fnv(hash, &l.type, sizeof l.type);
            hash = fnv(hash, &l.position, sizeof l.position);
            hash = fnv(hash, &l.range, sizeof l.range);
            hash = fnv(hash, &o.f, sizeof o.f);
            hash = fnv(hash, &o.tn, sizeof o.tn);
        }
        for (int i = 0; i < s.item_count; i++) {
            const DrawItem& it = s.items[i];
            if (!it.cast_shadows) continue;
            hash = fnv(hash, it.model.m, sizeof it.model.m);
            hash = fnv(hash, &it.mesh.key, sizeof it.mesh.key);
            hash = fnv(hash, &it.mesh.vertex_count, sizeof it.mesh.vertex_count);
            if (!it.mesh.key) cacheable = false;
        }
        hash = fnv(hash, &size, sizeof size);
        stats.local_shadows = local_count;
        if (cacheable && hash == atlas_hash) { stats.atlas_reused = true; return true; }

        double t0 = now_ms();
        if (!make(&local_p, shaders::hq_local_shadow_vertex, shaders::hq_local_shadow_fragment, "local shadow")) return false;
        set_target(atlas.color, atlas.depth);
        fglClearColor(gl, 1, 1, 1);
        fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
        fglEnable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
        fglUseShader(gl, local_p);
        bool failed = false;
        for (int j = 0; j < local_count && !failed; j++) {
            const RenderLight& l = *ls.local[j];
            const LocalShadow& o = locals[j];
            float range = l.range > 0.01f ? l.range : 0.01f;
            u4(local_p, "u_lp", l.position.x, l.position.y, l.position.z, 1.0f / range);
            float zn = fmaxf_(0.02f, range * 0.002f);
            fglSetProjection(gl, to_fgl(frustum(o.tn, zn, range)));
            int faces = l.type == LightType::Point ? 6 : 1;
            for (int face = 0; face < faces && !failed; face++) {
                Mat4 view;
                if (l.type == LightType::Point) {
                    float fw[3], up[3];
                    shaders::cube_face(face, fw, up);
                    view = Mat4::look_at(l.position, l.position + Vec3(fw[0], fw[1], fw[2]), Vec3(up[0], up[1], up[2]));
                } else {
                    Vec3 up = fabsf_(o.f.y) < 0.99f ? Vec3(0, 1, 0) : Vec3(0, 0, 1);
                    view = Mat4::look_at(l.position, l.position + o.f, up);
                }
                int t = o.tile + face;
                host.set_viewport(host.user, (t % kAtlasTiles) * T, (t / kAtlasTiles) * T, T, T);
                fglSetView(gl, to_fgl(view));
                for (int i = 0; i < s.item_count && !failed; i++) {
                    const DrawItem& it = s.items[i];
                    if (!it.cast_shadows) continue;
                    // only casters that reach into the light's range
                    Bounds b = item_bounds(it);
                    if (!b.any) continue;
                    Vec3 c = vmax(b.lo, vmin(l.position, b.hi));
                    Vec3 dd = c - l.position;
                    if (dot(dd, dd) > range * range) continue;
                    host.draw_mesh(host.user, it, false, &failed);
                }
            }
        }
        host.set_viewport(host.user, 0, 0, 0, 0);
        if (!end_pass(t0, &stats.local_shadow_ms) || failed) return false;
        atlas_hash = cacheable ? hash : 0;
        return true;
    }

    // "Masks": a pass can skip pixels cheaply with the depth test, which runs before the
    // fragment shader. A first pass draws its triangle at a near depth (u_z) and discards
    // the pixels to keep, so only the others get a depth; the next passes draw farther
    // and are rejected where a depth was written. Depth is ndc z * 0.5 + 0.5.
    static constexpr float kMaskZ = -0.4f;       // depth 0.3

    bool ao_passes(const RenderScene& s, const PassHost& host, const GraphicsSettings& gs) {
        int w = gl->desc.width, h = gl->desc.height;
        int hw = (w + 1) / 2, hh = (h + 1) / 2;
        if (!ensure(gdepth, hw, hh, true) || !ensure(gnormal, hw, hh, false) || !ensure(ao_a, hw, hh, false) ||
            !ensure(ao_b, hw, hh, false)) return false;
        if (!make_tri()) return false;
        Cam c = read_camera(s.camera);
        float far_m = fminf_(c.f, 1000.0f);
        bool failed = false;

        // linear depth, then view space normals, of all items at half resolution
        double t0 = now_ms();
        if (!make(&depth_p, shaders::hq_depth_vertex, shaders::hq_depth_fragment, "depth")) return false;
        if (!make(&normal_p, shaders::hq_normal_vertex, shaders::hq_normal_fragment, "normal")) return false;
        for (int pass = 0; pass < 2 && !failed; pass++) {
            FGLShader* sh = pass == 0 ? depth_p : normal_p;
            set_target(pass == 0 ? gdepth.color : gnormal.color, gdepth.depth);
            fglClearColor(gl, 1, 1, 1);
            fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
            fglSetProjection(gl, to_fgl(s.camera.proj));
            fglSetView(gl, to_fgl(s.camera.view));
            fglEnable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
            if (pass == 0) u4(sh, "u_pp", 1.0f / far_m, 0, 0, 0);
            fglUseShader(gl, sh);
            for (int i = 0; i < s.item_count && !failed; i++) host.draw_mesh(host.user, s.items[i], false, &failed);
        }
        if (!end_pass(t0, &stats.depth_ms) || failed) return false;

        // ssao: the sky is masked out; its pixels stay white (no occlusion)
        t0 = now_ms();
        if (!make(&skymask_p, shaders::pp_vertex, shaders::pp_skymask_fragment, "sky mask")) return false;
        if (!make(&ssao_p, shaders::pp_vertex, shaders::pp_ssao_fragment, "ssao")) return false;
        float px = 1.0f / (float)hw, py = 1.0f / (float)hh;
        set_target(ao_a.color, gdepth.depth);
        fglClearColor(gl, 1, 1, 1);
        fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
        fglEnable(gl, FGL_DEPTH_TEST);
        fglDisable(gl, FGL_CULL_FACE);
        u4(skymask_p, "u_p", px, py, 0, 0);
        u4(skymask_p, "u_z", kMaskZ, 0, 0, 0);
        bind_tex(skymask_p, "u_depth", gdepth, FGL_TEXTURE_CLAMP);
        fs_draw(skymask_p);
        u4(ssao_p, "u_p", px, py, px, py);
        u4(ssao_p, "u_ao", fclamp(gs.ao_radius, 0.05f, 5.0f), fclamp(gs.ao_strength, 0.0f, 2.0f),
           c.ortho ? 1.0f : 0.0f, far_m);
        u4(ssao_p, "u_proj", c.tx, c.ty, 0, 0);
        u4(ssao_p, "u_z", 0, 0, 0, 0);
        bind_tex(ssao_p, "u_depth", gdepth, FGL_TEXTURE_CLAMP);
        bind_tex(ssao_p, "u_normal", gnormal, FGL_TEXTURE_CLAMP);
        fs_draw(ssao_p);
        if (!end_pass(t0, &stats.ssao_ms)) return false;

        // blur: ao_a -> ao_b -> ao_a, over the same mask (each pass a little nearer)
        t0 = now_ms();
        if (!make(&blur_p, shaders::pp_vertex, shaders::pp_blur_fragment, "blur")) return false;
        for (int pass = 0; pass < 2; pass++) {
            const Target& src = pass == 0 ? ao_a : ao_b;
            const Target& dst = pass == 0 ? ao_b : ao_a;
            set_target(dst.color, gdepth.depth);
            if (pass == 0) {
                fglClearColor(gl, 1, 1, 1);
                fglClear(gl, FGL_COLOR_BUFFER_BIT);
            }
            fglEnable(gl, FGL_DEPTH_TEST);
            fglDisable(gl, FGL_CULL_FACE);
            u4(blur_p, "u_p", px, py, px, py);
            u4(blur_p, "u_dir", pass == 0 ? px : 0.0f, pass == 0 ? 0.0f : py, 0, 0);
            u4(blur_p, "u_q", far_m, 0, 0, 0);
            u4(blur_p, "u_z", pass == 0 ? -0.2f : -0.3f, 0, 0, 0);
            bind_tex(blur_p, "u_src", src, FGL_TEXTURE_CLAMP);
            fs_draw(blur_p);
        }
        return end_pass(t0, &stats.blur_ms);
    }

    static void set_sky_uniforms(FGLShader* sky, const RenderScene& scene, const PackedLights& ls) {
        const Sky& k = scene.sky;
        const Mat4& v = scene.camera.view;
        Vec3 right(v.m[0], v.m[4], v.m[8]), up(v.m[1], v.m[5], v.m[9]), back(v.m[2], v.m[6], v.m[10]);
        bool ortho = scene.camera.proj.m[15] == 1.0f && scene.camera.proj.m[11] == 0.0f;
        float px = scene.camera.proj.m[0], py = scene.camera.proj.m[5];
        Vec3 rx = ortho || px == 0 ? Vec3(0, 0, 0) : right * (1.0f / px);
        Vec3 ry = ortho || py == 0 ? Vec3(0, 0, 0) : up * (1.0f / py);
        u4(sky, "u_fwd", -back.x, -back.y, -back.z, 0);
        u4(sky, "u_rx", rx.x, rx.y, rx.z, 0);
        u4(sky, "u_ry", ry.x, ry.y, ry.z, 0);
        float i = k.intensity;
        bool flat = k.mode == SkyMode::Color;
        u4(sky, "u_zen", k.zenith.x * i, k.zenith.y * i, k.zenith.z * i, flat ? 1.0f : 0.0f);
        u4(sky, "u_hor", k.horizon.x * i, k.horizon.y * i, k.horizon.z * i, 0);
        u4(sky, "u_grd", k.ground.x * i, k.ground.y * i, k.ground.z * i, 0);
        if (ls.has_sun && k.sun_size > 0.0f && !flat) {
            float r = radians(fclamp(k.sun_size, 0.05f, 60.0f) * 0.5f);
            u4(sky, "u_sun", ls.sun_dir.x, ls.sun_dir.y, ls.sun_dir.z, fcos(r));
            Vec3 c = ls.sun_color * 4.0f;
            u4(sky, "u_suncol", c.x, c.y, c.z, 0);
        } else {
            u4(sky, "u_sun", 0, 1, 0, 2.0f);
            u4(sky, "u_suncol", 0, 0, 0, 0);
        }
    }

    bool lit_pass(const RenderScene& s, const PackedLights& ls, const GpuPlan& plan, const PassHost& host,
                  const GraphicsSettings& gs, const ShadowFit& fit, int shadow_light, const Target& dst, bool hdr) {
        int w = gl->desc.width, h = gl->desc.height;
        bool any_lit = false;
        for (int i = 0; i < s.item_count; i++) if (!s.items[i].unlit) { any_lit = true; break; }
        bool use_shadow = fit.valid;
        bool use_ao = plan.ao;
        double t0 = now_ms();

        // the fog: towards the horizon color
        const Sky& k = s.sky;
        float density = plan.fog ? 0.06f * fclamp(k.fog, 0.0f, 1.0f) : 0.0f;
        Vec3 fogc = (k.mode == SkyMode::Color ? k.zenith : k.horizon) * k.intensity;

        LitProg* lit = nullptr;
        if (any_lit) {
            shaders::HqLitKey key;
            key.nd = ls.nd; key.np = ls.np; key.ns = ls.ns;
            key.pbr = plan.pbr;
            key.shadow = use_shadow ? (gs.soft_shadows ? 2 : 1) : 0;
            key.shadow_light = shadow_light;
            key.ao = use_ao;
            key.fog = plan.fog;
            key.hdr = hdr;
            key.nps = ls.nps;
            key.nss = ls.nss;
            key.local_soft = (ls.nps + ls.nss) > 0 && gs.soft_shadows;
            lit = lit_variant(key);
            if (!lit) return false;
        }
        int ui = (plan.fog ? 2 : 0) + (hdr ? 1 : 0);
        bool fog = plan.fog, h2 = hdr;
        if (!make(&unlit_p[ui], shaders::hq_unlit_vertex,
                  [fog, h2](char* o, size_t c) { shaders::hq_unlit_fragment(o, c, fog, h2); }, "unlit")) return false;
        if (!make(&sky_p[hdr ? 1 : 0], shaders::sky_vertex,
                  [h2](char* o, size_t c) { shaders::hq_sky_fragment(o, c, h2); }, "sky")) return false;
        FGLShader* unlit = unlit_p[ui];
        FGLShader* sky = sky_p[hdr ? 1 : 0];

        // uniforms of the frame
        set_post(unlit, s.post, w, h);
        u4(unlit, "u_cam", s.camera.position.x, s.camera.position.y, s.camera.position.z, 0);
        u4(unlit, "u_fog", fogc.x, fogc.y, fogc.z, density);
        set_post(sky, s.post, w, h);
        set_sky_uniforms(sky, s, ls);
        if (lit) {
            FGLShader* sh = lit->sh;
            set_post(sh, s.post, w, h);
            u4(sh, "u_cam", s.camera.position.x, s.camera.position.y, s.camera.position.z, 0);
            const SkyLight& sl = s.sky_light;
            u4(sh, "u_sky", sl.sky_color.x * sl.intensity, sl.sky_color.y * sl.intensity, sl.sky_color.z * sl.intensity, 0);
            u4(sh, "u_gnd", sl.ground_color.x * sl.intensity, sl.ground_color.y * sl.intensity, sl.ground_color.z * sl.intensity, 0);
            u4(sh, "u_fog", fogc.x, fogc.y, fogc.z, density);
            uint32_t total = (uint32_t)(ls.nd + ls.np + ls.ns);
            if (total) {
                fglUniformArray(sh, fglUniformLocation(sh, "u_l0"), 0, ls.l0, total);
                fglUniformArray(sh, fglUniformLocation(sh, "u_l1"), 0, ls.l1, total);
                fglUniformArray(sh, fglUniformLocation(sh, "u_l2"), 0, ls.l2, total);
            }
            if (use_shadow) {
                fglUniformMatrix4(sh, fglUniformLocation(sh, "u_shm"), to_fgl(fit.viewproj));
                u4(sh, "u_sdir", fit.to_light.x, fit.to_light.y, fit.to_light.z, 0);
                u4(sh, "u_shp", 1.0f / (float)shadow.w, fit.meters_per_uv, fit.inv_range, fit.bias);
                u4(sh, "u_shq", fit.radius_uv, fit.normal_offset, 0, 0);
                u4(sh, "u_lr", fit.right.x, fit.right.y, fit.right.z, 0);
                u4(sh, "u_lu", fit.up.x, fit.up.y, fit.up.z, 0);
            }
            if (ls.nps + ls.nss > 0) {
                float T = (float)(atlas.w / kAtlasTiles);
                u4(sh, "u_la", (float)kAtlasTiles, 1.0f / (float)kAtlasTiles, 1.5f / T, 0);
                float v[kMaxLocalShadows * 4];
                for (int j = 0; j < ls.nps + ls.nss; j++) {
                    const LocalShadow& o = locals[j];
                    float e[4] = { (float)o.tile, o.tn, o.bias, 3.0f * o.tn / T };
                    memcpy(v + j * 4, e, sizeof e);
                }
                fglUniformArray(sh, fglUniformLocation(sh, "u_lsh"), 0, v, (uint32_t)(ls.nps + ls.nss));
            }
        }

        set_target(dst.color, main_depth);
        if (lit) {
            if (use_shadow) bind_tex(lit->sh, "u_shadow", shadow, FGL_TEXTURE_CLAMP);
            if (use_ao) bind_tex(lit->sh, "u_ao", ao_a, FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP);
            if (ls.nps + ls.nss > 0) bind_tex(lit->sh, "u_atlas", atlas, FGL_TEXTURE_CLAMP);
        }
        fglClearColor(gl, 0, 0, 0);
        fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
        fglSetProjection(gl, to_fgl(s.camera.proj));
        fglSetView(gl, to_fgl(s.camera.view));
        fglEnable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
        bool failed = false;
        for (int i = 0; i < s.item_count && !failed; i++) {
            const DrawItem& it = s.items[i];
            if (it.unlit) {
                fglUseShader(gl, unlit);
                u4(unlit, "u_mat", it.color.x, it.color.y, it.color.z, it.roughness);
            } else {
                fglUseShader(gl, lit->sh);
                fglUniform4f(lit->sh, lit->mat, it.color.x, it.color.y, it.color.z, it.roughness);
                fglUniform4f(lit->sh, lit->pbr, fclamp(it.metallic, 0.0f, 1.0f), 0, 0, 0);
            }
            host.draw_mesh(host.user, it, true, &failed);
        }
        if (!failed) {
            fglDisable(gl, FGL_CULL_FACE);
            fglUseShader(gl, sky);
            fglSetModel(gl, to_fgl(Mat4::identity()));
            fglDraw(gl, tri, 0, 3);
        }
        bool ok = end_pass(t0, &stats.lit_ms);
        return ok && !failed;
    }

    bool bloom_passes(const RenderScene& s, const PostProcess& post, const Target& hdr) {
        int w = gl->desc.width, h = gl->desc.height;
        for (int i = 0; i < kBloomLevels; i++) {
            if (!ensure(bl_down[i], w >> (2 + i), h >> (2 + i), false)) return false;
            if (i < kBloomLevels - 1 && !ensure(bl_up[i], w >> (2 + i), h >> (2 + i), false)) return false;
        }
        double t0 = now_ms();
        if (!make(&bright_p, shaders::pp_vertex, shaders::pp_bright_fragment, "bloom bright")) return false;
        if (!make(&down_p, shaders::pp_vertex, shaders::pp_down_fragment, "bloom down")) return false;
        if (!make(&up_p, shaders::pp_vertex, shaders::pp_up_fragment, "bloom up")) return false;
        (void)s;
        // bright pass: full size -> 1/4 (each output texel is a 4x4 box of the scene)
        {
            const Target& d = bl_down[0];
            fs_begin(d);
            u4(bright_p, "u_p", 1.0f / (float)d.w, 1.0f / (float)d.h, 1.0f / (float)hdr.w, 1.0f / (float)hdr.h);
            u4(bright_p, "u_q", fmaxf_(post.bloom_threshold, 0.0f), 0, 0, 0);
            bind_tex(bright_p, "u_src", hdr, FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP);
            fs_draw(bright_p);
        }
        for (int i = 1; i < kBloomLevels; i++) {
            const Target& d = bl_down[i];
            const Target& sr = bl_down[i - 1];
            fs_begin(d);
            u4(down_p, "u_p", 1.0f / (float)d.w, 1.0f / (float)d.h, 1.0f / (float)sr.w, 1.0f / (float)sr.h);
            bind_tex(down_p, "u_src", sr, FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP);
            fs_draw(down_p);
        }
        for (int i = kBloomLevels - 2; i >= 0; i--) {
            const Target& d = bl_up[i];
            const Target& fine = bl_down[i];
            const Target& coarse = i == kBloomLevels - 2 ? bl_down[kBloomLevels - 1] : bl_up[i + 1];
            fs_begin(d);
            u4(up_p, "u_p", 1.0f / (float)d.w, 1.0f / (float)d.h, 1.0f / (float)coarse.w, 1.0f / (float)coarse.h);
            bind_tex(up_p, "u_fine", fine, FGL_TEXTURE_CLAMP);
            bind_tex(up_p, "u_coarse", coarse, FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP);
            fs_draw(up_p);
        }
        return end_pass(t0, &stats.bloom_ms);
    }

    bool composite(const RenderScene& s, const Target& scene_t, bool with_bloom, FGLSurface dst_color) {
        int w = gl->desc.width, h = gl->desc.height;
        double t0 = now_ms();
        FGLShader** slot = &comp_p[with_bloom ? 1 : 0];
        bool b = with_bloom;
        if (!make(slot, shaders::pp_vertex, [b](char* o, size_t c) { shaders::pp_composite_fragment(o, c, b); }, "composite"))
            return false;
        FGLShader* sh = *slot;
        set_target(dst_color, 0);
        fglDisable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
        set_post(sh, s.post, w, h);
        u4(sh, "u_p", 1.0f / (float)w, 1.0f / (float)h, 0, 0);
        bind_tex(sh, "u_scene", scene_t, FGL_TEXTURE_CLAMP);
        if (with_bloom) {
            u4(sh, "u_q", fclamp(s.post.bloom, 0.0f, 1.0f) * 0.3f, 0, 0, 0);
            bind_tex(sh, "u_bloom", bl_up[0], FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP);
        }
        fs_draw(sh);
        return end_pass(t0, &stats.composite_ms);
    }

    // FXAA in two draws over one depth mask (see kMaskZ): the first copies the pixels
    // without an edge, the second runs the search on the rest.
    bool fxaa(const Target& src) {
        int w = gl->desc.width, h = gl->desc.height;
        double t0 = now_ms();
        if (!make(&fxaa_mask_p, shaders::pp_vertex, shaders::pp_fxaa_mask_fragment, "fxaa mask")) return false;
        if (!make(&fxaa_p, shaders::pp_vertex, shaders::pp_fxaa_fragment, "fxaa")) return false;
        set_target(main_color, main_depth);
        fglClear(gl, FGL_DEPTH_BUFFER_BIT);
        fglEnable(gl, FGL_DEPTH_TEST);
        fglDisable(gl, FGL_CULL_FACE);
        float px = 1.0f / (float)w, py = 1.0f / (float)h;
        u4(fxaa_mask_p, "u_p", px, py, 0, 0);
        u4(fxaa_mask_p, "u_z", kMaskZ, 0, 0, 0);
        u4(fxaa_p, "u_p", px, py, 0, 0);
        u4(fxaa_p, "u_z", 0, 0, 0, 0);
        bind_tex(fxaa_mask_p, "u_n", src, FGL_TEXTURE_CLAMP);
        bind_tex(fxaa_p, "u_n", src, FGL_TEXTURE_CLAMP);
        bind_tex(fxaa_p, "u_l", src, FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP);
        fs_draw(fxaa_mask_p);
        fs_draw(fxaa_p);
        return end_pass(t0, &stats.fxaa_ms);
    }

    bool render(FGLContext* ctx, const RenderScene& s, const PackedLights& ls, const GraphicsSettings& gs, const GpuPlan& plan,
                const PassHost& host, char* error, int error_cap) {
        gl = ctx;
        err = error;
        err_cap = error_cap;
        frame++;
        stats = GpuTierStats();
        main_color = gl->color;
        main_depth = gl->depth;
        int w = gl->desc.width, h = gl->desc.height;
        bool ok = run(s, ls, gs, plan, host, w, h);
        // leave the context on its own surfaces whatever happened
        restore_main();
        if (!ok) return false;
        if (gl->error) { char b[64]; snprintf(b, sizeof b, "FarcadeGL: submit failed (error %d)", gl->error); return fail(b); }
        return true;
    }

    bool run(const RenderScene& s, const PackedLights& ls, const GraphicsSettings& gs, const GpuPlan& plan,
             const PassHost& host, int w, int h) {
        if (!make_tri()) return false;

        // shadow
        ShadowFit fit;
        int shadow_light = 0;
        if (plan.shadows && ls.nd > 0) {
            // the strongest directional light with shadows, found again in the packed lights
            const RenderLight* best = nullptr;
            float best_power = 0;
            for (int i = 0; i < s.light_count; i++) {
                const RenderLight& l = s.lights[i];
                float power = l.intensity * luma(l.color);
                if (l.type == LightType::Directional && l.shadows && power > best_power) { best = &l; best_power = power; }
            }
            if (best) {
                Vec3 dir = normalize(best->direction);
                Vec3 col = best->color * best->intensity;
                for (int i = 0; i < ls.nd; i++) {
                    const float* d = ls.l1 + i * 4;
                    const float* c = ls.l2 + i * 4;
                    if (fabsf_(d[0] - dir.x) + fabsf_(d[1] - dir.y) + fabsf_(d[2] - dir.z) < 1e-3f &&
                        fabsf_(c[0] - col.x) + fabsf_(c[1] - col.y) + fabsf_(c[2] - col.z) < 1e-3f) {
                        shadow_light = i;
                        Bounds recv, cast;
                        uint64_t hash = 1469598103934665603ull;
                        bool cacheable = true;
                        for (int j = 0; j < s.item_count; j++) {
                            const DrawItem& it = s.items[j];
                            Bounds b = item_bounds(it);
                            recv.add(b);
                            if (it.cast_shadows) {
                                cast.add(b);
                                hash = fnv(hash, it.model.m, sizeof it.model.m);
                                hash = fnv(hash, &it.mesh.key, sizeof it.mesh.key);
                                hash = fnv(hash, &it.mesh.vertex_count, sizeof it.mesh.vertex_count);
                                if (!it.mesh.key) cacheable = false;
                            }
                        }
                        int size = gs.shadow_resolution;
                        if (size < 256) size = 256;
                        if (size > 4096) size = 4096;
                        if (!plan.hw && size > 1024) size = 1024;     // clears and fills cost too much on the CPU backend
                        fit = fit_shadow(s, recv, cast, best->direction, size, fclamp(gs.shadow_distance, 1.0f, 2000.0f),
                                         gs.soft_shadows);
                        if (fit.valid) {
                            hash = fnv(hash, fit.viewproj.m, sizeof fit.viewproj.m);
                            hash = fnv(hash, &size, sizeof size);
                            if (!shadow_pass(s, host, fit, size, cacheable ? hash : 0)) return false;
                        }
                        break;
                    }
                }
            }
        }

        stats.local_shadows = 0;
        stats.atlas_reused = false;
        stats.local_shadow_ms = 0;
        if (ls.nps + ls.nss > 0 && !atlas_pass(s, ls, host, gs, plan.hw)) return false;
        if (plan.ao && !ao_passes(s, host, gs)) return false;

        // the scene
        bool bloom = plan.bloom, aa = plan.aa;
        if (bloom || aa) {
            if (!ensure(scene, w, h, false)) return false;
            if (bloom && aa && !ensure(ldr, w, h, false)) return false;
        }
        Target main_t;
        main_t.color = main_color;
        main_t.w = w;
        main_t.h = h;
        if (!lit_pass(s, ls, plan, host, gs, fit, shadow_light, (bloom || aa) ? scene : main_t, bloom)) return false;

        if (bloom) {
            if (!bloom_passes(s, s.post, scene)) return false;
            if (!composite(s, scene, true, aa ? ldr.color : main_color)) return false;
        }
        if (aa) {
            if (!fxaa(bloom ? ldr : scene)) return false;
        }
        return true;
    }

    void release_all() {
        if (!gl) return;
        drop(&shadow_p); drop(&local_p); drop(&depth_p); drop(&normal_p); drop(&skymask_p); drop(&fxaa_mask_p); drop(&ssao_p); drop(&blur_p); drop(&bright_p); drop(&down_p);
        drop(&up_p); drop(&comp_p[0]); drop(&comp_p[1]); drop(&fxaa_p);
        for (FGLShader*& s : unlit_p) drop(&s);
        for (FGLShader*& s : sky_p) drop(&s);
        for (LitProg& v : lit_p) { drop(&v.sh); v = LitProg(); }
        free_target(shadow); free_target(atlas); free_target(gdepth); free_target(gnormal); free_target(ao_a); free_target(ao_b);
        free_target(scene); free_target(ldr);
        for (Target& t : bl_down) free_target(t);
        for (Target& t : bl_up) free_target(t);
        if (tri) fglDestroyBuffer(gl, tri);
        tri = 0;
        shadow_hash = 0;
        atlas_hash = 0;
        gl = nullptr;
    }
};

GpuTier::GpuTier() : impl_(new Impl()) {}
GpuTier::~GpuTier() { delete impl_; }

GpuPlan GpuTier::plan(const RenderScene& scene, const GraphicsFeatures& f) {
    GpuPlan p;
    bool sun = false;
    for (int i = 0; i < scene.light_count; i++) {
        const RenderLight& l = scene.lights[i];
        if (l.type == LightType::Directional && l.shadows && l.intensity * luma(l.color) > 0.0f) sun = true;
    }
    p.shadows = f.shadows && sun && scene.item_count > 0;
    bool local = false;
    for (int i = 0; i < scene.light_count; i++) {
        const RenderLight& l = scene.lights[i];
        if (l.type != LightType::Directional && l.shadows && l.intensity * luma(l.color) > 0.0f) local = true;
    }
    p.local_shadows = f.local_shadows && local && scene.item_count > 0;
    p.ao = f.ambient_occlusion && scene.item_count > 0;
    p.bloom = f.bloom && scene.post.bloom > 0.0f;
    p.aa = f.antialiasing;
    p.pbr = f.pbr;
    p.fog = scene.sky.fog > 0.0005f;
    p.hw = f.gpu;
    return p;
}

bool GpuTier::render(FGLContext* gl, const RenderScene& scene, const PackedLights& lights, const GraphicsSettings& settings,
                     const GpuPlan& plan, const PassHost& host, char* error, int error_cap) {
    return impl_->render(gl, scene, lights, settings, plan, host, error, error_cap);
}

void GpuTier::release(FGLContext*) { impl_->release_all(); }

const GpuTierStats& GpuTier::stats() const { return impl_->stats; }
int GpuTier::live_surfaces() const { return impl_->surfaces; }
int GpuTier::live_programs() const { return impl_->programs; }

}  // namespace fe::gfx
