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

constexpr int kBloomLevels = 5;
constexpr int kMaxCascades = shaders::kMaxCascades;
constexpr int kMaxLitVariants = 8;
constexpr int kBoundsCache = 128;
constexpr uint32_t kFrameVec4Count = (uint32_t)shaders::kFrameHeader + 3u * shaders::kMaxLights + kMaxLocalShadows;

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
    Vec3 center;                            // the snapped centre of the fitted sphere
    float radius = 0;                       // its (quantized) radius
};

// The sun's cascades of a frame: one map (an atlas of grid x grid tiles), a fit for each cascade.
struct CascadeSet {
    int count = 0;                          // 0: no sun shadow this frame
    int grid = 1, tile = 0;                 // tiles per side, tile size in texels
    ShadowFit fit[kMaxCascades];
    Vec3 to_light, right, up;
    bool any() const { return count > 0; }
};

// What a cascade's tile holds: the fit it was drawn with and what it depended on. Far cascades keep their
// fit for a few frames (see GraphicsSettings::cascade_cache) so they are not redrawn while the camera drifts.
struct CascadeState {
    bool valid = false;
    ShadowFit held;
    uint64_t fit_key = 0;                   // everything but the camera position the fit depends on
    uint64_t hash = 0;                      // the fit and the casters the tile was drawn with (0: not cacheable)
    uint64_t fitted_frame = 0;
    Vec3 fitted_at;                         // the camera position of the fit
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
    FGLBuffer frame_data = 0;
    // programs
    FGLShader* shadow_p = nullptr;
    FGLShader* local_p = nullptr;           // point / spot casters into the atlas
    FGLShader* depth_p = nullptr;
    FGLShader* normal_p = nullptr;
    FGLShader* skymask_p = nullptr;
    FGLShader* fxaa_mask_p = nullptr;
    FGLShader* ssao_p = nullptr;
    FGLShader* blur_p = nullptr;
    FGLShader* bright_p[2] = {};            // [karis]
    FGLShader* down_p[2] = {};              // [wide]
    FGLShader* up_p = nullptr;
    FGLShader* comp_p[8] = {};              // [ssr * 4 + chromatic * 2 + bloom]
    shaders::PostKey comp_key[8], unlit_key[4], sky_key[2];
    const PostFrame* pf = nullptr;          // this frame's post chain values
    FGLShader* fxaa_p = nullptr;
    FGLShader* unlit_p[4] = {};             // [fog * 2 + hdr]
    FGLShader* sky_p[2] = {};               // [hdr]
    LitProg lit_p[kMaxLitVariants];
    // targets
    Target shadow, gdepth, gnormal, ao_a, ao_b, scene, ldr;
    Target gmat, ao_c, ao_hist, ssr_t;      // roughness / metal G-buffer, GTAO raw and history, the SSR result
    uint64_t ao_scene_hash = 0;             // what the GTAO history was accumulated for
    FGLShader* gtao_p = nullptr;
    FGLShader* ao_accum_p = nullptr;
    FGLShader* copy_p = nullptr;
    FGLShader* material_p = nullptr;
    FGLShader* ssrmask_p = nullptr;
    FGLShader* ssr_p = nullptr;
    Target sm_edges, sm_wh, sm_wv;          // SMAA: edges, blend weights of the two axes
    FGLTexture sm_area = 0;                 // the area texture (post_stack.h smaa_area), made on first use
    FGLShader* sm_edge_p = nullptr;
    FGLShader* sm_mask_p = nullptr;
    FGLShader* sm_w_p[2] = {};
    FGLShader* sm_blend_p = nullptr;
    Target taa_hist[2], tdepth;             // TAA: the resolved image of the last two frames (output size), full size linear depth
    int taa_cur = 0;                        // which of taa_hist holds the latest resolve
    bool taa_valid = false;                 // taa_hist[taa_cur] is a real previous frame
    Mat4 taa_prev_vp;                       // the unjittered view-projection that frame had
    FGLShader* taa_p = nullptr;
    int iw = 0, ih = 0;                     // the size the scene is drawn at (the context's unless TAA upscales)
    Mat4 cur_proj;                          // the projection of the lit pass: jittered with TAA
    CascadeState cstate[kMaxCascades];
    FGLShader* white_p = nullptr;           // fills a tile with white (the far end of the packed depth)
    Target atlas;
    uint64_t atlas_hash = 0;
    LocalShadow locals[kMaxLocalShadows];
    int local_count = 0;
    Target bl_down[kBloomLevels], bl_up[kBloomLevels - 1];
    int bl_levels = 0;                      // levels used this frame
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
        static const float v[24] = {
            -1, -1, 0, 0, 0, 0, 0, 0,
             3, -1, 0, 0, 0, 0, 1, 0,
            -1,  3, 0, 0, 0, 0, 0, 1
        };
        tri = fglCreateBuffer(gl, sizeof v);
        if (!tri || fglBufferData(gl, tri, 0, v, sizeof v) != 0) return fail("FarcadeGL: cannot create the pass buffer");
        return true;
    }

    bool upload_frame_data(const PackedLights& ls, const CascadeSet& cs) {
        float data[kFrameVec4Count][4] = {};
        int total = ls.nd + ls.np + ls.ns;
        if (cs.any()) {
            data[4][0] = cs.to_light.x; data[4][1] = cs.to_light.y; data[4][2] = cs.to_light.z;
            data[7][0] = cs.right.x; data[7][1] = cs.right.y; data[7][2] = cs.right.z;
            data[8][0] = cs.up.x; data[8][1] = cs.up.y; data[8][2] = cs.up.z;
            float inv_grid = 1.0f / (float)cs.grid;
            for (int c = 0; c < cs.count; c++) {
                const ShadowFit& f = cs.fit[c];
                int base = shaders::kCascadeBase + c * shaders::kCascadeStride;
                memcpy(data[base], f.viewproj.m, sizeof f.viewproj.m);
                data[base + 4][0] = (float)(c % cs.grid) * inv_grid;
                data[base + 4][1] = (float)(c / cs.grid) * inv_grid;
                data[base + 4][2] = inv_grid;
                data[base + 4][3] = f.normal_offset;
                data[base + 5][0] = f.radius_uv;
                data[base + 5][1] = f.meters_per_uv * f.inv_range;
                data[base + 5][2] = f.bias;
            }
        }
        if (ls.nps + ls.nss > 0) {
            int tiles = kAtlasTiles;
            int tile_size = atlas.w / kAtlasTiles;
            data[9][0] = (float)tiles;
            data[9][1] = 1.0f / (float)tiles;
            data[9][2] = 1.5f / (float)tile_size;
        }
        const int H = shaders::kFrameHeader;
        for (int i = 0; i < total; i++) {
            memcpy(data[H + i * 3], ls.l0 + i * 4, 4 * sizeof(float));
            memcpy(data[H + 1 + i * 3], ls.l1 + i * 4, 4 * sizeof(float));
            memcpy(data[H + 2 + i * 3], ls.l2 + i * 4, 4 * sizeof(float));
        }
        int shadow_base = H + total * 3;
        for (int i = 0; i < ls.nps + ls.nss; i++) {
            const LocalShadow& o = locals[i];
            data[shadow_base + i][0] = (float)o.tile;
            data[shadow_base + i][1] = o.tn;
            data[shadow_base + i][2] = o.bias;
            data[shadow_base + i][3] = 3.0f * o.tn / (float)(atlas.w / kAtlasTiles);
        }
        if (!frame_data) {
            frame_data = fglCreateBuffer(gl, sizeof data);
            if (!frame_data) return fail("FarcadeGL: cannot create the frame data buffer");
        }
        if (fglBufferData(gl, frame_data, 0, data, sizeof data) != 0)
            return fail("FarcadeGL: cannot upload frame data");
        fglBindStorageBuffer(gl, 0, frame_data);
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
        u4(sh, "u_post", pf->exposure_ev, p.contrast, p.saturation, fclamp(p.vignette, 0.0f, 1.0f));
        u4(sh, "u_tint", p.tint.x, p.tint.y, p.tint.z, (float)(int)p.tonemap);
        u4(sh, "u_res", (float)w, (float)h, 0, 0);
        if (pf->key.grade) {
            u4(sh, "u_lift", pf->lift[0], pf->lift[1], pf->lift[2], 0);
            u4(sh, "u_gamma", pf->inv_gamma[0], pf->inv_gamma[1], pf->inv_gamma[2], 0);
            u4(sh, "u_gain", pf->gain[0], pf->gain[1], pf->gain[2], 0);
        }
        if (pf->key.grain) u4(sh, "u_grain", pf->grain[0], pf->grain[1], pf->grain[2], 0);
    }

    // A program whose source depends on the post chain: recompiled when the chain's choices change.
    template <class V, class F>
    bool make_keyed(FGLShader** out, shaders::PostKey* stored, V vgen, F fgen, const char* what) {
        if (*out && *stored != pf->key) drop(out);
        if (*out) return true;
        if (!make(out, vgen, fgen, what)) return false;
        *stored = pf->key;
        return true;
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
    // The farthest view depth that gets a shadow: the camera's far plane, the shadow distance, and the
    // farthest receiver, whichever is nearest.
    float shadow_far(const Cam& c, const Bounds& receivers, float distance) {
        float far_fit = fminf_(c.f, distance);
        float maxd = c.n;
        for (int i = 0; i < 8; i++) maxd = fmaxf_(maxd, dot(receivers.corner(i) - c.pos, c.fwd));
        far_fit = fminf_(far_fit, fmaxf_(maxd, c.n + 0.1f));
        if (!(far_fit > c.n)) far_fit = c.n + 0.1f;
        return far_fit;
    }

    // `slice_near` .. `far_fit`: the view depth range this fit covers (a cascade).
    ShadowFit fit_shadow(const RenderScene& s, const Bounds& receivers, const Bounds& casters, Vec3 travel, int size,
                         float slice_near, float far_fit, bool soft) {
        ShadowFit out;
        if (!receivers.any || !casters.any) return out;
        Vec3 dir = normalize(travel);
        if (!(length(dir) > 0.5f)) return out;
        Cam c = read_camera(s.camera);
        const float n = slice_near;

        float zc, radius;
        if (c.ortho) {
            zc = (n + far_fit) * 0.5f;
            float hd = (far_fit - n) * 0.5f;
            radius = fsqrt(hd * hd + c.tx * c.tx + c.ty * c.ty);
        } else {
            float t2 = c.tx * c.tx + c.ty * c.ty;
            zc = (far_fit + n) * (1.0f + t2) * 0.5f;
            if (zc > far_fit) zc = far_fit;
            float rf = fsqrt((far_fit - zc) * (far_fit - zc) + far_fit * far_fit * t2);
            float rn = fsqrt((zc - n) * (zc - n) + n * n * t2);
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
        // the depth along the light is snapped too (coarsely): the matrix only changes when the fit really moves
        const float zstep = 0.125f * radius;
        cz = floorf_(cz / zstep + 0.5f) * zstep;
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
        out.center = snapped;
        out.radius = radius;
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
    static float fmodf_(float x, float m) { return x - floorf_(x / m) * m; }

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
        shaders::hq_lit_vertex(vs, shaders::kHqSourceBytes, key.shadow != 0 ? key.cascades : 0);
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

    // Does a world space box touch the sun's view volume of this fit (a box around the sphere, along the light)?
    static bool in_cascade(const Bounds& b, const ShadowFit& f) {
        if (!b.any) return false;
        Vec3 c = (b.lo + b.hi) * 0.5f;
        float hr = length(b.hi - b.lo) * 0.5f;
        Vec3 d = c - f.center;
        return fabsf_(dot(d, f.right)) <= f.radius + hr && fabsf_(dot(d, f.up)) <= f.radius + hr;
    }

    // The casters that reach cascade `f`, with a hash of them (0 when one has no mesh key: not cacheable).
    uint64_t cascade_casters(const RenderScene& s, const ShadowFit& f, int* count) {
        uint64_t hash = 1469598103934665603ull;
        bool cacheable = true;
        int n = 0;
        for (int i = 0; i < s.item_count; i++) {
            const DrawItem& it = s.items[i];
            if (!it.cast_shadows || !in_cascade(item_bounds(it), f)) continue;
            hash = fnv(hash, it.model.m, sizeof it.model.m);
            hash = fnv(hash, &it.mesh.key, sizeof it.mesh.key);
            hash = fnv(hash, &it.mesh.vertex_count, sizeof it.mesh.vertex_count);
            if (!it.mesh.key) cacheable = false;
            n++;
        }
        *count = n;
        return cacheable ? hash : 0;
    }

    // Redraws one cascade's tile: depth is cleared (the whole surface; it is scratch), the tile is filled
    // white by a full screen triangle under its viewport, and the casters are drawn into it.
    bool draw_cascade(const RenderScene& s, const PassHost& host, const ShadowFit& fit, const CascadeSet& cs, int c) {
        const int T = cs.tile;
        host.set_viewport(host.user, (c % cs.grid) * T, (c / cs.grid) * T, T, T);
        fglClear(gl, FGL_DEPTH_BUFFER_BIT);
        fglDisable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
        fglUseShader(gl, white_p);
        fglSetModel(gl, to_fgl(Mat4::identity()));
        fglDraw(gl, tri, 0, 3);
        fglSetProjection(gl, to_fgl(fit.proj));
        fglSetView(gl, to_fgl(fit.view));
        fglEnable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
        fglUseShader(gl, shadow_p);
        bool failed = false;
        for (int i = 0; i < s.item_count && !failed; i++) {
            const DrawItem& it = s.items[i];
            if (!it.cast_shadows || !in_cascade(item_bounds(it), fit)) continue;
            host.draw_mesh(host.user, it, false, &failed);
        }
        host.set_viewport(host.user, 0, 0, 0, 0);
        return !failed;
    }

    // The fraction of the shadow range each cascade ends at.
    static void cascade_splits(int n, float out[kMaxCascades]) {
        static const float s1[1] = { 1.0f }, s2[2] = { 0.3f, 1.0f }, s3[3] = { 0.14f, 0.38f, 1.0f };
        const float* p = n >= 3 ? s3 : (n == 2 ? s2 : s1);
        for (int i = 0; i < n; i++) out[i] = p[i];
    }

    // Fits and draws the sun's cascades (stable, texel snapped fits; a cascade is skipped while its tile is still
    // valid; far cascades hold their fit for 2 / 4 frames while the camera stays inside it). Fills `cs`.
    bool cascades_pass(const RenderScene& s, const PassHost& host, const GraphicsSettings& gs, bool hw, const Bounds& recv,
                       const Bounds& cast, Vec3 travel, CascadeSet* cs) {
        *cs = CascadeSet();
        int nc = gs.shadow_cascades < 1 ? 1 : (gs.shadow_cascades > kMaxCascades ? kMaxCascades : gs.shadow_cascades);
        int size = gs.shadow_resolution;
        if (size < 256) size = 256;
        if (size > 4096) size = 4096;
        if (!hw && size > 1024) size = 1024;     // clears and fills cost too much on the CPU backend
        int grid = nc > 1 ? 2 : 1;
        int tile = size / grid;
        bool fresh = false;
        if (!ensure(shadow, size, size, true, &fresh)) return false;
        if (!make(&shadow_p, shaders::hq_shadow_vertex, shaders::hq_shadow_fragment, "shadow")) return false;
        if (!make(&white_p, shaders::pp_vertex, shaders::pp_white_fragment, "white")) return false;
        if (fresh) {
            // never drawn: white = the far end everywhere (a tile that is not used stays lit)
            double t0 = now_ms();
            set_target(shadow.color, shadow.depth);
            fglClearColor(gl, 1, 1, 1);
            fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
            if (!end_pass(t0, &stats.shadow_ms)) return false;
            for (CascadeState& st : cstate) st = CascadeState();
        }
        Cam c = read_camera(s.camera);
        float far_total = shadow_far(c, recv, fclamp(gs.shadow_distance, 1.0f, 2000.0f));
        Vec3 dir = normalize(travel);
        float splits[kMaxCascades];
        cascade_splits(nc, splits);
        cs->count = nc;
        cs->grid = grid;
        cs->tile = tile;
        stats.cascades = nc;
        int drawn = 0, reused = 0;
        double t0 = now_ms();
        bool began = false;
        for (int k = 0; k < nc; k++) {
            CascadeState& st = cstate[k];
            float slice_near = k == 0 ? c.n : fmaxf_(c.n, far_total * splits[k - 1]);
            float slice_far = far_total * splits[k];
            if (!(slice_far > slice_near)) slice_far = slice_near + 0.1f;
            uint64_t fit_key = 1469598103934665603ull;
            fit_key = fnv(fit_key, &dir, sizeof dir);
            fit_key = fnv(fit_key, &tile, sizeof tile);
            fit_key = fnv(fit_key, &nc, sizeof nc);
            fit_key = fnv(fit_key, &slice_near, sizeof slice_near);
            fit_key = fnv(fit_key, &slice_far, sizeof slice_far);
            fit_key = fnv(fit_key, &gs.soft_shadows, sizeof gs.soft_shadows);
            // a far cascade may keep its fit while the camera stays well inside it
            bool keep = false;
            if (gs.cascade_cache && k > 0 && st.valid && st.fit_key == fit_key) {
                float moved = length(c.pos - st.fitted_at);
                bool due = (frame - st.fitted_frame) >= (uint64_t)(1u << k);
                keep = !due && moved < 0.2f * st.held.radius;
            }
            ShadowFit fit;
            if (keep) {
                fit = st.held;
            } else {
                fit = fit_shadow(s, recv, cast, travel, tile, slice_near, slice_far, gs.soft_shadows);
                if (!fit.valid) { *cs = CascadeSet(); return true; }
                st.held = fit;
                st.fit_key = fit_key;
                st.fitted_frame = frame;
                st.fitted_at = c.pos;
            }
            cs->fit[k] = fit;
            stats.cascade_texel[k] = fit.meters_per_uv / (float)tile;
            int n_casters = 0;
            uint64_t hash = cascade_casters(s, fit, &n_casters);
            if (hash) { hash = fnv(hash, fit.viewproj.m, sizeof fit.viewproj.m); hash = fnv(hash, &tile, sizeof tile); }
            st.valid = true;
            if (hash && hash == st.hash) { reused++; continue; }
            if (!began) { set_target(shadow.color, shadow.depth); began = true; }
            if (!draw_cascade(s, host, fit, *cs, k)) return false;
            st.hash = hash;
            drawn++;
        }
        cs->to_light = cs->fit[0].to_light;
        cs->right = cs->fit[0].right;
        cs->up = cs->fit[0].up;
        stats.cascades_drawn = drawn;
        stats.cascades_reused = reused;
        stats.shadow_reused = drawn == 0;
        if (began) return end_pass(t0, &stats.shadow_ms);
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

    // The half resolution G-buffer (linear depth, view normals, and with SSR the roughness / metal), then the AO:
    // SSAO (12 taps) or GTAO (2 slices x 6 steps, accumulated over frames while nothing moves), blurred twice.
    bool ao_passes(const RenderScene& s, const PassHost& host, const GraphicsSettings& gs, bool want_ao, bool gtao, bool want_ssr) {
        int w = iw, h = ih;
        int hw = (w + 1) / 2, hh = (h + 1) / 2;
        if (!ensure(gdepth, hw, hh, true) || !ensure(gnormal, hw, hh, false)) return false;
        if (want_ao && (!ensure(ao_a, hw, hh, false) || !ensure(ao_b, hw, hh, false))) return false;
        if (want_ssr && !ensure(gmat, hw, hh, false)) return false;
        bool hist_fresh = false;
        if (want_ao && gtao && (!ensure(ao_c, hw, hh, false) || !ensure(ao_hist, hw, hh, false, &hist_fresh))) return false;
        if (!make_tri()) return false;
        Cam c = read_camera(s.camera);
        float far_m = fminf_(c.f, 1000.0f);
        bool failed = false;

        // linear depth, then view space normals (and materials), of all items at half resolution
        double t0 = now_ms();
        if (!make(&depth_p, shaders::hq_depth_vertex, shaders::hq_depth_fragment, "depth")) return false;
        if (!make(&normal_p, shaders::hq_normal_vertex, shaders::hq_normal_fragment, "normal")) return false;
        if (want_ssr && !make(&material_p, shaders::hq_material_vertex, shaders::hq_material_fragment, "material")) return false;
        const int passes = want_ssr ? 3 : 2;
        for (int pass = 0; pass < passes && !failed; pass++) {
            FGLShader* sh = pass == 0 ? depth_p : (pass == 1 ? normal_p : material_p);
            set_target(pass == 0 ? gdepth.color : (pass == 1 ? gnormal.color : gmat.color), gdepth.depth);
            fglClearColor(gl, 1, 1, 1);
            fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
            fglSetProjection(gl, to_fgl(s.camera.proj));
            fglSetView(gl, to_fgl(s.camera.view));
            fglEnable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
            if (pass == 0) u4(sh, "u_pp", 1.0f / far_m, 0, 0, 0);
            fglUseShader(gl, sh);
            for (int i = 0; i < s.item_count && !failed; i++) {
                if (pass == 2) u4(sh, "u_mm", fclamp(s.items[i].roughness, 0.0f, 1.0f), fclamp(s.items[i].metallic, 0.0f, 1.0f), 0, 0);
                host.draw_mesh(host.user, s.items[i], false, &failed);
            }
        }
        if (!end_pass(t0, &stats.depth_ms) || failed) return false;
        if (!want_ao) return true;

        // ao: the sky is masked out; its pixels stay white (no occlusion)
        t0 = now_ms();
        if (!make(&skymask_p, shaders::pp_vertex, shaders::pp_skymask_fragment, "sky mask")) return false;
        if (gtao) {
            if (!make(&gtao_p, shaders::pp_vertex, shaders::pp_gtao_fragment, "gtao")) return false;
            if (!make(&ao_accum_p, shaders::pp_vertex, shaders::pp_ao_accum_fragment, "ao accumulate")) return false;
            if (!make(&copy_p, shaders::pp_vertex, shaders::pp_copy_fragment, "copy")) return false;
        } else if (!make(&ssao_p, shaders::pp_vertex, shaders::pp_ssao_fragment, "ssao")) {
            return false;
        }
        float px = 1.0f / (float)hw, py = 1.0f / (float)hh;
        const Target& first_dst = gtao ? ao_c : ao_a;
        set_target(first_dst.color, gdepth.depth);
        fglClearColor(gl, 1, 1, 1);
        fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
        fglEnable(gl, FGL_DEPTH_TEST);
        fglDisable(gl, FGL_CULL_FACE);
        u4(skymask_p, "u_p", px, py, 0, 0);
        u4(skymask_p, "u_z", kMaskZ, 0, 0, 0);
        bind_tex(skymask_p, "u_depth", gdepth, FGL_TEXTURE_CLAMP);
        fs_draw(skymask_p);
        FGLShader* ao_sh = gtao ? gtao_p : ssao_p;
        u4(ao_sh, "u_p", px, py, px, py);
        u4(ao_sh, "u_ao", fclamp(gs.ao_radius, 0.05f, 5.0f), fclamp(gs.ao_strength, 0.0f, 2.0f),
           c.ortho ? 1.0f : 0.0f, far_m);
        u4(ao_sh, "u_proj", c.tx, c.ty, 0, 0);
        u4(ao_sh, "u_z", 0, 0, 0, 0);
        // the slice rotation moves every frame (golden ratio steps), so the accumulated frames see different slices
        if (gtao) u4(ao_sh, "u_gt", fmodf_(0.381966f * (float)(frame % 64), 1.0f), 0, 0, 0);
        bind_tex(ao_sh, "u_depth", gdepth, FGL_TEXTURE_CLAMP);
        bind_tex(ao_sh, "u_normal", gnormal, FGL_TEXTURE_CLAMP);
        fs_draw(ao_sh);
        float zb0 = -0.2f, zb1 = -0.3f;                  // the blurs' depths, each nearer than the pass before
        if (gtao) {
            // temporal accumulation while the camera and the items stand still, else the current frame alone
            uint64_t sh = 1469598103934665603ull;
            sh = fnv(sh, s.camera.view.m, sizeof s.camera.view.m);
            sh = fnv(sh, s.camera.proj.m, sizeof s.camera.proj.m);
            for (int i = 0; i < s.item_count; i++) sh = fnv(sh, s.items[i].model.m, sizeof s.items[i].model.m);
            bool still = !hist_fresh && sh == ao_scene_hash;
            ao_scene_hash = sh;
            set_target(ao_a.color, gdepth.depth);
            fglClearColor(gl, 1, 1, 1);
            fglClear(gl, FGL_COLOR_BUFFER_BIT);
            fglEnable(gl, FGL_DEPTH_TEST);
            fglDisable(gl, FGL_CULL_FACE);
            u4(ao_accum_p, "u_p", px, py, 0, 0);
            u4(ao_accum_p, "u_q", still ? 0.2f : 1.0f, 0, 0, 0);
            u4(ao_accum_p, "u_z", -0.2f, 0, 0, 0);
            bind_tex(ao_accum_p, "u_cur", ao_c, FGL_TEXTURE_CLAMP);
            bind_tex(ao_accum_p, "u_hist", ao_hist, FGL_TEXTURE_CLAMP);
            fs_draw(ao_accum_p);
            set_target(ao_hist.color, gdepth.depth);
            fglClearColor(gl, 1, 1, 1);
            fglClear(gl, FGL_COLOR_BUFFER_BIT);
            fglEnable(gl, FGL_DEPTH_TEST);
            fglDisable(gl, FGL_CULL_FACE);
            u4(copy_p, "u_p", px, py, 0, 0);
            u4(copy_p, "u_z", -0.25f, 0, 0, 0);
            bind_tex(copy_p, "u_src", ao_a, FGL_TEXTURE_CLAMP);
            fs_draw(copy_p);
            zb0 = -0.36f; zb1 = -0.38f;
        }
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
            u4(blur_p, "u_z", pass == 0 ? zb0 : zb1, 0, 0, 0);
            bind_tex(blur_p, "u_src", src, FGL_TEXTURE_CLAMP);
            fs_draw(blur_p);
        }
        return end_pass(t0, &stats.blur_ms);
    }

    // Screen space reflections, half resolution: the pixels that get one (smooth enough geometry) are picked by
    // a depth mask, the ray march adds the scene colour found at the hit. The result is read by the composite pass.
    bool ssr_pass(const RenderScene& s, const Target& scene_t, const GraphicsSettings& gs) {
        (void)gs;
        int hw = gdepth.w, hh = gdepth.h;
        if (!ensure(ssr_t, hw, hh, false)) return false;
        double t0 = now_ms();
        if (!make(&ssrmask_p, shaders::pp_vertex, shaders::pp_ssrmask_fragment, "ssr mask")) return false;
        if (!make(&ssr_p, shaders::pp_vertex, shaders::pp_ssr_fragment, "ssr")) return false;
        Cam c = read_camera(s.camera);
        float far_m = fminf_(c.f, 1000.0f);
        float px = 1.0f / (float)hw, py = 1.0f / (float)hh;
        const float cutoff = 0.55f;
        set_target(ssr_t.color, gdepth.depth);
        fglClearColor(gl, 0, 0, 0);
        fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
        fglEnable(gl, FGL_DEPTH_TEST);
        fglDisable(gl, FGL_CULL_FACE);
        u4(ssrmask_p, "u_p", px, py, 0, 0);
        u4(ssrmask_p, "u_q", cutoff, 0, 0, 0);
        u4(ssrmask_p, "u_z", kMaskZ, 0, 0, 0);
        bind_tex(ssrmask_p, "u_depth", gdepth, FGL_TEXTURE_CLAMP);
        bind_tex(ssrmask_p, "u_mat", gmat, FGL_TEXTURE_CLAMP);
        fs_draw(ssrmask_p);
        u4(ssr_p, "u_p", px, py, 0, 0);
        u4(ssr_p, "u_ao", c.ortho ? 1.0f : 0.0f, far_m, (float)(frame % 8) * 0.125f, 0);
        u4(ssr_p, "u_proj", c.tx, c.ty, 0, 0);
        u4(ssr_p, "u_q", 10.0f, 0.4f, cutoff, 0);
        u4(ssr_p, "u_z", 0, 0, 0, 0);
        bind_tex(ssr_p, "u_depth", gdepth, FGL_TEXTURE_CLAMP);
        bind_tex(ssr_p, "u_normal", gnormal, FGL_TEXTURE_CLAMP);
        bind_tex(ssr_p, "u_mat", gmat, FGL_TEXTURE_CLAMP);
        bind_tex(ssr_p, "u_scene", scene_t, FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP);
        fs_draw(ssr_p);
        if (!end_pass(t0, &stats.ssr_ms)) return false;
        return true;
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
                  const GraphicsSettings& gs, const CascadeSet& cs, int shadow_light, const Target& dst, bool hdr) {
        int w = iw, h = ih;
        bool any_lit = false;
        for (int i = 0; i < s.item_count; i++) if (!s.items[i].unlit) { any_lit = true; break; }
        bool use_shadow = cs.any();
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
            key.cascades = use_shadow ? cs.count : 1;
            key.ao = use_ao;
            key.fog = plan.fog;
            key.hdr = hdr;
            key.nps = ls.nps;
            key.nss = ls.nss;
            key.local_soft = (ls.nps + ls.nss) > 0 && gs.soft_shadows;
            key.post = pf->key;
            key.gi = host.gi_on;
            lit = lit_variant(key);
            if (!lit) return false;
        }
        int ui = (plan.fog ? 2 : 0) + (hdr ? 1 : 0);
        bool fog = plan.fog, h2 = hdr;
        shaders::PostKey pk = pf->key;
        if (!make_keyed(&unlit_p[ui], &unlit_key[ui], shaders::hq_unlit_vertex,
                        [fog, h2, pk](char* o, size_t c) { shaders::hq_unlit_fragment(o, c, fog, h2, pk); }, "unlit")) return false;
        if (!make_keyed(&sky_p[hdr ? 1 : 0], &sky_key[hdr ? 1 : 0], shaders::sky_vertex,
                        [h2, pk](char* o, size_t c) { shaders::hq_sky_fragment(o, c, h2, pk); }, "sky")) return false;
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
            if (!upload_frame_data(ls, cs)) return false;
            if (host.gi_on) {
                u4(sh, "u_gi0", host.gi0[0], host.gi0[1], host.gi0[2], host.gi0[3]);
                u4(sh, "u_gi1", host.gi1[0], host.gi1[1], host.gi1[2], host.gi1[3]);
            }
        }

        set_target(dst.color, dst.depth ? dst.depth : main_depth);
        if (lit) {
            if (use_shadow) bind_tex(lit->sh, "u_shadow", shadow, FGL_TEXTURE_CLAMP);
            if (use_ao) bind_tex(lit->sh, "u_ao", ao_a, FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP);
            if (ls.nps + ls.nss > 0) bind_tex(lit->sh, "u_atlas", atlas, FGL_TEXTURE_CLAMP);
            if (host.gi_on) fglUniformTexture(gl, lit->sh, fglUniformLocation(lit->sh, "u_gi"), host.gi_texture, FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP);
        }
        fglClearColor(gl, 0, 0, 0);
        fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
        fglSetProjection(gl, to_fgl(cur_proj));
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

    bool bloom_passes(const RenderScene& s, const PostProcess& post, const Target& hdr, bool chain) {
        int w = gl->desc.width, h = gl->desc.height;
        // Levels stop where a side would be under 4 texels (a small image gets fewer, so the glow of a
        // thumbnail is not a haze over the whole picture); bl_levels is at least 2.
        int levels = 2;
        if (!chain) levels = 4;                  // the simple bloom: always 4 levels
        while (chain && levels < kBloomLevels && (w >> (2 + levels)) >= 4 && (h >> (2 + levels)) >= 4) levels++;
        bl_levels = levels;
        for (int i = 0; i < levels; i++) {
            if (!ensure(bl_down[i], w >> (2 + i), h >> (2 + i), false)) return false;
            if (i < levels - 1 && !ensure(bl_up[i], w >> (2 + i), h >> (2 + i), false)) return false;
        }
        double t0 = now_ms();
        const int ci = chain ? 1 : 0;
        FGLShader*& bright_p = this->bright_p[ci];
        FGLShader*& down_p = this->down_p[ci];
        if (!make(&bright_p, shaders::pp_vertex, [chain](char* o, size_t c) { shaders::pp_bright_fragment(o, c, chain); }, "bloom bright")) return false;
        if (!make(&down_p, shaders::pp_vertex, [chain](char* o, size_t c) { shaders::pp_down_fragment(o, c, chain); }, "bloom down")) return false;
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
        for (int i = 1; i < levels; i++) {
            const Target& d = bl_down[i];
            const Target& sr = bl_down[i - 1];
            fs_begin(d);
            u4(down_p, "u_p", 1.0f / (float)d.w, 1.0f / (float)d.h, 1.0f / (float)sr.w, 1.0f / (float)sr.h);
            bind_tex(down_p, "u_src", sr, FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP);
            fs_draw(down_p);
        }
        for (int i = levels - 2; i >= 0; i--) {
            const Target& d = bl_up[i];
            const Target& fine = bl_down[i];
            const Target& coarse = i == levels - 2 ? bl_down[levels - 1] : bl_up[i + 1];
            fs_begin(d);
            u4(up_p, "u_p", 1.0f / (float)d.w, 1.0f / (float)d.h, 1.0f / (float)coarse.w, 1.0f / (float)coarse.h);
            u4(up_p, "u_q", chain ? 2.0f * pf->bloom_radius : 1.0f, 0, 0, 0);
            bind_tex(up_p, "u_fine", fine, FGL_TEXTURE_CLAMP);
            bind_tex(up_p, "u_coarse", coarse, FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP);
            fs_draw(up_p);
        }
        return end_pass(t0, &stats.bloom_ms);
    }

    bool composite(const RenderScene& s, const Target& scene_t, bool with_bloom, bool with_ssr, FGLSurface dst_color) {
        int w = gl->desc.width, h = gl->desc.height;
        double t0 = now_ms();
        bool ca = pf->chromatic > 0.001f;
        int ci = (with_ssr ? 4 : 0) + (ca ? 2 : 0) + (with_bloom ? 1 : 0);
        FGLShader** slot = &comp_p[ci];
        bool b = with_bloom;
        shaders::PostKey pk = pf->key;
        if (!make_keyed(slot, &comp_key[ci], shaders::pp_vertex,
                        [b, pk, ca, with_ssr](char* o, size_t c) { shaders::pp_composite_fragment(o, c, b, pk, ca, with_ssr); }, "composite"))
            return false;
        FGLShader* sh = *slot;
        set_target(dst_color, 0);
        fglDisable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
        set_post(sh, s.post, w, h);
        u4(sh, "u_p", 1.0f / (float)w, 1.0f / (float)h, 0, 0);
        bind_tex(sh, "u_scene", scene_t, ca ? (FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP) : FGL_TEXTURE_CLAMP);
        if (ca) u4(sh, "u_ca", pf->chromatic * 0.012f, 0, 0, 0);
        if (with_ssr) bind_tex(sh, "u_ssr", ssr_t, FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP);
        if (with_bloom) {
            u4(sh, "u_q", fclamp(s.post.bloom, 0.0f, 1.0f) * 0.3f, 0, 0, 0);
            bind_tex(sh, "u_bloom", bl_up[0], FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP);
        }
        fs_draw(sh);
        return end_pass(t0, &stats.composite_ms);
    }

    // The projection with a sub-pixel jitter (pixels of the internal image): ndc moves by 2 j / size.
    static Mat4 jittered(const Mat4& p, float jx, float jy, int w, int h) {
        Mat4 r = p;
        float dx = 2.0f * jx / (float)w, dy = 2.0f * jy / (float)h;
        bool ortho = p.m[15] == 1.0f && p.m[11] == 0.0f;
        if (ortho) { r.m[12] += dx; r.m[13] += dy; }
        else { r.m[8] -= dx; r.m[9] -= dy; }
        return r;
    }

    // TAA: depth at the jittered camera, then the resolve into the other history target (output size). Returns
    // the resolved image in *out.
    bool taa_pass(const RenderScene& s, const PassHost& host, const Target& scene_t, float jx, float jy, const Target** out) {
        int ow = gl->desc.width, oh = gl->desc.height;
        double t0 = now_ms();
        bool fresh = false;
        if (!ensure(taa_hist[0], ow, oh, false, &fresh) || !ensure(taa_hist[1], ow, oh, false, &fresh)) return false;
        if (!ensure(tdepth, iw, ih, true)) return false;
        if (fresh) taa_valid = false;
        if (!make(&depth_p, shaders::hq_depth_vertex, shaders::hq_depth_fragment, "depth")) return false;
        if (!make(&taa_p, shaders::pp_vertex, shaders::pp_taa_fragment, "taa")) return false;
        Cam c = read_camera(s.camera);
        float far_m = fminf_(c.f, 1000.0f);
        bool failed = false;
        set_target(tdepth.color, tdepth.depth);
        fglClearColor(gl, 1, 1, 1);
        fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
        fglSetProjection(gl, to_fgl(cur_proj));
        fglSetView(gl, to_fgl(s.camera.view));
        fglEnable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
        u4(depth_p, "u_pp", 1.0f / far_m, 0, 0, 0);
        fglUseShader(gl, depth_p);
        for (int i = 0; i < s.item_count && !failed; i++) host.draw_mesh(host.user, s.items[i], false, &failed);
        if (failed) return false;
        // reprojection: previous view-projection * inverse(current), both unjittered
        Mat4 vp = s.camera.proj * s.camera.view, inv, rep = Mat4::identity();
        bool ok = mat4_inverse(vp, &inv);
        if (ok) rep = taa_prev_vp * inv;
        int dst = 1 - taa_cur;
        set_target(taa_hist[dst].color, 0);
        fglDisable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
        u4(taa_p, "u_p", 1.0f / (float)ow, 1.0f / (float)oh, 1.0f / (float)iw, 1.0f / (float)ih);
        u4(taa_p, "u_j", jx / (float)iw, -jy / (float)ih, far_m, 0.1f);
        const Mat4& pj = s.camera.proj;
        u4(taa_p, "u_zp", -pj.m[10], pj.m[14], c.ortho ? 1.0f : 0.0f, (taa_valid && ok) ? 1.0f : 0.0f);
        u4(taa_p, "u_zq", pj.m[10], pj.m[14], 0, 0);
        u4(taa_p, "u_r0", rep.m[0], rep.m[1], rep.m[2], rep.m[3]);
        u4(taa_p, "u_r1", rep.m[4], rep.m[5], rep.m[6], rep.m[7]);
        u4(taa_p, "u_r2", rep.m[8], rep.m[9], rep.m[10], rep.m[11]);
        u4(taa_p, "u_r3", rep.m[12], rep.m[13], rep.m[14], rep.m[15]);
        u4(taa_p, "u_z", 0, 0, 0, 0);
        bind_tex(taa_p, "u_curl", scene_t, FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP);
        bind_tex(taa_p, "u_curn", scene_t, FGL_TEXTURE_CLAMP);
        bind_tex(taa_p, "u_depth", tdepth, FGL_TEXTURE_CLAMP);
        bind_tex(taa_p, "u_hist", taa_hist[taa_cur], FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP);
        fs_draw(taa_p);
        if (!end_pass(t0, &stats.taa_ms)) return false;
        taa_cur = dst;
        taa_valid = true;
        taa_prev_vp = vp;
        *out = &taa_hist[taa_cur];
        return true;
    }

    // SMAA 1x style (shaders_hq.cpp): edges, then blend weights from the area texture over the edge pixels only,
    // then the neighbourhood blend over a copy of the image.
    bool smaa(const Target& src) {
        int w = gl->desc.width, h = gl->desc.height;
        double t0 = now_ms();
        if (!ensure(sm_edges, w, h, false) || !ensure(sm_wh, w, h, false) || !ensure(sm_wv, w, h, false)) return false;
        if (!sm_area) {
            uint32_t px[kSmaaAreaSize * kSmaaAreaSize];
            smaa_area_texture(px);
            sm_area = fglCreateTexture(gl, kSmaaAreaSize, kSmaaAreaSize, px);
            if (!sm_area) return fail("FarcadeGL: cannot create the SMAA area texture");
        }
        if (!make(&sm_edge_p, shaders::pp_vertex, shaders::pp_smaa_edge_fragment, "smaa edges")) return false;
        if (!make(&sm_mask_p, shaders::pp_vertex, shaders::pp_smaa_mask_fragment, "smaa mask")) return false;
        for (int v = 0; v < 2; v++)
            if (!make(&sm_w_p[v], shaders::pp_vertex, [v](char* o, size_t c) { shaders::pp_smaa_weights_fragment(o, c, v != 0); }, "smaa weights")) return false;
        if (!make(&sm_blend_p, shaders::pp_vertex, shaders::pp_smaa_blend_fragment, "smaa blend")) return false;
        if (!make(&copy_p, shaders::pp_vertex, shaders::pp_copy_fragment, "copy")) return false;
        float px = 1.0f / (float)w, py = 1.0f / (float)h;
        // 1. edges (nearest taps of the LDR image); pixels without an edge stay black
        set_target(sm_edges.color, 0);
        fglClearColor(gl, 0, 0, 0);
        fglClear(gl, FGL_COLOR_BUFFER_BIT);
        fglDisable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
        u4(sm_edge_p, "u_p", px, py, 0, 0);
        u4(sm_edge_p, "u_z", 0, 0, 0, 0);
        bind_tex(sm_edge_p, "u_src", src, FGL_TEXTURE_CLAMP);
        fs_draw(sm_edge_p);
        // 2. the weights, only where there is an edge: a mask first (it takes a depth where there is none)
        set_target(sm_wh.color, main_depth);
        fglClearColor(gl, 0, 0, 0);
        fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
        fglEnable(gl, FGL_DEPTH_TEST);
        fglDisable(gl, FGL_CULL_FACE);
        u4(sm_mask_p, "u_p", px, py, 0, 0);
        u4(sm_mask_p, "u_z", kMaskZ, 0, 0, 0);
        bind_tex(sm_mask_p, "u_edges", sm_edges, FGL_TEXTURE_CLAMP);
        fs_draw(sm_mask_p);
        for (int v = 0; v < 2; v++) {
            if (v == 1) {
                set_target(sm_wv.color, main_depth);
                fglClearColor(gl, 0, 0, 0);
                fglClear(gl, FGL_COLOR_BUFFER_BIT);
                fglEnable(gl, FGL_DEPTH_TEST);
                fglDisable(gl, FGL_CULL_FACE);
            }
            u4(sm_w_p[v], "u_p", px, py, 0, 0);
            u4(sm_w_p[v], "u_z", v == 0 ? 0.0f : -0.1f, 0, 0, 0);
            bind_tex(sm_w_p[v], "u_edges", sm_edges, FGL_TEXTURE_CLAMP);
            fglUniformTexture(gl, sm_w_p[v], fglUniformLocation(sm_w_p[v], "u_area"), sm_area, FGL_TEXTURE_CLAMP);
            fs_draw(sm_w_p[v]);
        }
        // 3. a copy of the image, then the blend over the edge pixels
        set_target(main_color, main_depth);
        fglDisable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
        u4(copy_p, "u_p", px, py, 0, 0);
        u4(copy_p, "u_z", 0, 0, 0, 0);
        bind_tex(copy_p, "u_src", src, FGL_TEXTURE_CLAMP);
        fs_draw(copy_p);
        fglEnable(gl, FGL_DEPTH_TEST);
        u4(sm_blend_p, "u_p", px, py, 0, 0);
        u4(sm_blend_p, "u_z", -0.3f, 0, 0, 0);
        bind_tex(sm_blend_p, "u_src", src, FGL_TEXTURE_CLAMP);
        bind_tex(sm_blend_p, "u_wh", sm_wh, FGL_TEXTURE_CLAMP);
        bind_tex(sm_blend_p, "u_wv", sm_wv, FGL_TEXTURE_CLAMP);
        fs_draw(sm_blend_p);
        return end_pass(t0, &stats.smaa_ms);
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
                const PassHost& host, const PostFrame& post_frame, char* error, int error_cap) {
        gl = ctx;
        pf = &post_frame;
        err = error;
        err_cap = error_cap;
        frame++;
        stats = GpuTierStats();
        main_color = gl->color;
        main_depth = gl->depth;
        int w = gl->desc.width, h = gl->desc.height;
        iw = host.iw > 0 ? host.iw : w;
        ih = host.ih > 0 ? host.ih : h;
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

        // shadow: the strongest directional light with shadows, found again in the packed lights
        CascadeSet cs;
        int shadow_light = 0;
        if (plan.shadows && ls.nd > 0) {
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
                        for (int j = 0; j < s.item_count; j++) {
                            const DrawItem& it = s.items[j];
                            Bounds b = item_bounds(it);
                            recv.add(b);
                            if (it.cast_shadows) cast.add(b);
                        }
                        if (!cascades_pass(s, host, gs, plan.hw, recv, cast, best->direction, &cs)) return false;
                        break;
                    }
                }
            }
        }

        stats.local_shadows = 0;
        stats.atlas_reused = false;
        stats.local_shadow_ms = 0;
        if (ls.nps + ls.nss > 0 && !atlas_pass(s, ls, host, gs, plan.hw)) return false;
        if ((plan.ao || plan.ssr) && !ao_passes(s, host, gs, plan.ao, plan.gtao, plan.ssr)) return false;

        // the scene
        bool bloom = plan.bloom, aa = plan.aa, ssr = plan.ssr;
        const bool taa = aa && plan.aa_mode == AAMode::TAA;
        const bool fx = aa && !taa;                  // FXAA or SMAA after the composite
        const bool hdr = bloom || ssr || taa;        // the lit pass writes HDR encoded values, the composite pass finishes
        cur_proj = s.camera.proj;
        if (!taa) taa_valid = false;                 // the history is only good for consecutive TAA frames
        float jx = 0, jy = 0;
        if (taa) {
            taa_jitter((uint32_t)frame, &jx, &jy);
            cur_proj = jittered(s.camera.proj, jx, jy, iw, ih);
        }
        if (hdr || aa) {
            // the scene is drawn at the internal size (with its own depth when that is not the context's)
            if (!ensure(scene, iw, ih, iw != w || ih != h)) return false;
            if (hdr && fx && !ensure(ldr, w, h, false)) return false;
        }
        Target main_t;
        main_t.color = main_color;
        main_t.w = w;
        main_t.h = h;
        if (!lit_pass(s, ls, plan, host, gs, cs, shadow_light, (hdr || aa) ? scene : main_t, hdr)) return false;

        if (ssr && !ssr_pass(s, scene, gs)) return false;
        const Target* resolved = &scene;
        if (taa && !taa_pass(s, host, scene, jx, jy, &resolved)) return false;
        if (hdr) {
            if (bloom && !bloom_passes(s, s.post, *resolved, gs.bloom_mode == BloomMode::Chain)) return false;
            if (!composite(s, *resolved, bloom, ssr, fx ? ldr.color : main_color)) return false;
        }
        if (fx) {
            const Target& src = hdr ? ldr : scene;
            if (plan.aa_mode == AAMode::SMAA ? !smaa(src) : !fxaa(src)) return false;
        }
        return true;
    }

    void release_all() {
        if (!gl) return;
        drop(&shadow_p); drop(&white_p); drop(&local_p); drop(&depth_p); drop(&normal_p); drop(&skymask_p); drop(&fxaa_mask_p); drop(&ssao_p); drop(&blur_p); for (FGLShader*& b : bright_p) drop(&b); for (FGLShader*& d : down_p) drop(&d);
        drop(&up_p); for (FGLShader*& c : comp_p) drop(&c); drop(&fxaa_p);
        drop(&taa_p);
        free_target(taa_hist[0]); free_target(taa_hist[1]); free_target(tdepth);
        taa_valid = false;
        drop(&sm_edge_p); drop(&sm_mask_p); drop(&sm_w_p[0]); drop(&sm_w_p[1]); drop(&sm_blend_p);
        if (sm_area) fglDestroyTexture(gl, sm_area);
        sm_area = 0;
        free_target(sm_edges); free_target(sm_wh); free_target(sm_wv);
        drop(&gtao_p); drop(&ao_accum_p); drop(&copy_p); drop(&material_p); drop(&ssrmask_p); drop(&ssr_p);
        for (FGLShader*& s : unlit_p) drop(&s);
        for (FGLShader*& s : sky_p) drop(&s);
        for (LitProg& v : lit_p) { drop(&v.sh); v = LitProg(); }
        free_target(shadow); free_target(atlas); free_target(gdepth); free_target(gnormal); free_target(ao_a); free_target(ao_b);
        free_target(gmat); free_target(ao_c); free_target(ao_hist); free_target(ssr_t);
        free_target(scene); free_target(ldr);
        for (Target& t : bl_down) free_target(t);
        for (Target& t : bl_up) free_target(t);
        if (tri) fglDestroyBuffer(gl, tri);
        if (frame_data) fglDestroyBuffer(gl, frame_data);
        tri = 0;
        frame_data = 0;
        for (CascadeState& st : cstate) st = CascadeState();
        ao_scene_hash = 0;
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
    p.aa_mode = f.aa_mode;
    p.gtao = f.ambient_occlusion && f.ao_mode == AOMode::GTAO;
    p.ssr = f.ssr && scene.item_count > 0;
    p.pbr = f.pbr;
    p.fog = scene.sky.fog > 0.0005f;
    p.hw = f.gpu;
    return p;
}

bool GpuTier::render(FGLContext* gl, const RenderScene& scene, const PackedLights& lights, const GraphicsSettings& settings,
                     const GpuPlan& plan, const PassHost& host, const PostFrame& post_frame, char* error, int error_cap) {
    return impl_->render(gl, scene, lights, settings, plan, host, post_frame, error, error_cap);
}

void GpuTier::release(FGLContext*) { impl_->release_all(); }

const GpuTierStats& GpuTier::stats() const { return impl_->stats; }
int GpuTier::live_surfaces() const { return impl_->surfaces; }
int GpuTier::live_programs() const { return impl_->programs; }

}  // namespace fe::gfx
