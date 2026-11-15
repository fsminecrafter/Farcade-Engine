// The FarcadeGL renderer (engine/gfx/renderer.h).
//
// How a frame is drawn:
//   - A purely offscreen context: fglCreateContext with window_id 0 and
//     page_flip 0 only creates a color and a depth surface (CREATE_SURFACE);
//     the screen is not touched until fglPresent, which is never called.
//     The frame is run with fglSubmit and copied out with fglReadPixels.
//   - Meshes are uploaded once per RenderMesh.key as buffers of
//     (position, normal) floats and reused; key 0 is uploaded for the frame
//     and freed after it.
//   - One lit shader per mix of directional / point / spot lights (FGSL
//     unrolls loops and the shader machine runs no branches, so the counts are
//     compiled in), at most kMaxLights lights per frame, cached.
//   - Unlit items use a flat shader, the sky one full-screen triangle at the
//     far plane drawn last (only the pixels no mesh covers are shaded; geometry
//     beyond about 1e6 * near is lost to the sky: depth 0.9999995).
//   - Post processing (exposure, tint, tonemap, contrast, saturation,
//     vignette, linear to sRGB) is the last part of each fragment shader:
//     the render targets are 8 bit, so a separate pass could not tonemap HDR.
//     Bloom is not implemented.
#include "gfx/renderer.h"
#include "gfx/shaders.h"

#if defined(FE_HOST)
#include "gfx/host/farcadegl_host.h"
#else
#include "farcadegl.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fe::gfx {

namespace {

constexpr int kMaxLights = shaders::kMaxLights;
constexpr uint32_t kVertexBytes = 24;               // position xyz, normal xyz
constexpr uint32_t kChunkVertices = 600000;         // a buffer holds up to 16 MiB; multiple of 3
constexpr int kMaxChunks = 8;
constexpr int kMaxCachedBuffers = 2048;             // the kernel allows 4096 per program
constexpr int kMaxTempBuffers = 1024;
constexpr int kMaxVariants = 12;                    // lit shaders kept
constexpr uint32_t kCommandBudget = 900 * 1024;     // submit before FGPU_MAX_COMMAND_BYTES (1 MiB)
constexpr uint32_t kUploadSlice = 8190;             // vertices converted per upload call (a multiple of 3)

struct MeshEntry {
    uint64_t key = 0;
    uint64_t last_frame = 0;
    uint32_t vertex_count = 0;
    uint32_t chunks = 0;
    FGLBuffer buffer[kMaxChunks] = {};
    uint32_t chunk_vertices[kMaxChunks] = {};
    bool used = false;
};

// Uniform locations of one shader (-1: the shader does not have it).
struct Locs {
    int post = -1, tint = -1, res = -1, mat = -1, cam = -1, sky = -1, gnd = -1;
    int l0 = -1, l1 = -1, l2 = -1, l3 = -1;
    int fwd = -1, rx = -1, ry = -1, zen = -1, hor = -1, grd = -1, sun = -1, suncol = -1;
};

struct Shader {
    FGLShader* sh = nullptr;
    Locs loc;
};

struct Variant {
    Shader shader;
    int nd = 0, np = 0, ns = 0;
    uint64_t last_use = 0;
    bool used = false;
};

// The lights of a frame, packed for the lit shader.
struct LightSet {
    float l0[kMaxLights * 4];
    float l1[kMaxLights * 4];
    float l2[kMaxLights * 4];
    float l3[kMaxLights * 4];
    int nd = 0, np = 0, ns = 0;
    bool has_sun = false;
    Vec3 sun_dir;                           // towards the sun
    Vec3 sun_color;
};

struct Chosen {
    const RenderLight* light;
    float score;
};

float luma(const Vec3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

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

}  // namespace

struct Renderer::Impl {
    int width = 0, height = 0;
    FGLContext* gl = nullptr;
    Shader unlit, sky;
    Variant variants[kMaxVariants];
    FGLBuffer sky_buffer = 0;
    uint32_t* pixels = nullptr;
    char error[200] = "";
    RendererStats stats;
    uint64_t frame = 0;

    // Mesh cache: open addressing on key, entries never move between rebuilds.
    MeshEntry* table = nullptr;
    uint32_t table_cap = 0, table_used = 0;
    int cached_buffers = 0;
    FGLBuffer temp[kMaxTempBuffers];
    int temp_count = 0;
    float* scratch = nullptr;               // kUploadSlice interleaved vertices

    void set_error(const char* what, int code = 0) {
        if (code) snprintf(error, sizeof error, "%s (error %d)", what, code);
        else snprintf(error, sizeof error, "%s", what);
    }

    // ---- context

    bool make_context() {
        FGLContextDesc d;
        memset(&d, 0, sizeof d);
        d.width = (uint32_t)width;
        d.height = (uint32_t)height;
        d.window_id = 0;                    // never presented: offscreen
        d.depth = 1;
        d.page_flip = 0;
        gl = fglCreateContext(&d);
        if (!gl) { set_error("FarcadeGL: cannot create the render surfaces"); return false; }
        static const FGLAttribute attrs[2] = { { 0, 3 }, { 12, 3 } };
        fglVertexLayout(gl, kVertexBytes, 2, attrs);
        return true;
    }

    // ---- shaders

    static void find_locs(FGLShader* sh, Locs* l) {
        l->post = fglUniformLocation(sh, "u_post");
        l->tint = fglUniformLocation(sh, "u_tint");
        l->res = fglUniformLocation(sh, "u_res");
        l->mat = fglUniformLocation(sh, "u_mat");
        l->cam = fglUniformLocation(sh, "u_cam");
        l->sky = fglUniformLocation(sh, "u_sky");
        l->gnd = fglUniformLocation(sh, "u_gnd");
        l->l0 = fglUniformLocation(sh, "u_l0");
        l->l1 = fglUniformLocation(sh, "u_l1");
        l->l2 = fglUniformLocation(sh, "u_l2");
        l->l3 = fglUniformLocation(sh, "u_l3");
        l->fwd = fglUniformLocation(sh, "u_fwd");
        l->rx = fglUniformLocation(sh, "u_rx");
        l->ry = fglUniformLocation(sh, "u_ry");
        l->zen = fglUniformLocation(sh, "u_zen");
        l->hor = fglUniformLocation(sh, "u_hor");
        l->grd = fglUniformLocation(sh, "u_grd");
        l->sun = fglUniformLocation(sh, "u_sun");
        l->suncol = fglUniformLocation(sh, "u_suncol");
    }

    bool compile(Shader* out, const char* vs, const char* fs, const char* what) {
        FGLShader* sh = fglCreateShader(gl, vs, fs);
        if (!sh) {
            snprintf(error, sizeof error, "%s shader: %s", what, fglShaderLog(gl));
            return false;
        }
        out->sh = sh;
        find_locs(sh, &out->loc);
        return true;
    }

    void drop(Shader* s) {
        if (s->sh) fglDestroyShader(gl, s->sh);
        *s = Shader();
    }

    bool make_shaders() {
        char* vs = (char*)malloc(shaders::kSourceBytes);
        char* fs = (char*)malloc(shaders::kSourceBytes);
        if (!vs || !fs) { free(vs); free(fs); set_error("out of memory"); return false; }
        bool ok = true;
        shaders::unlit_vertex(vs, shaders::kSourceBytes);
        shaders::unlit_fragment(fs, shaders::kSourceBytes);
        ok = compile(&unlit, vs, fs, "unlit");
        if (ok) {
            shaders::sky_vertex(vs, shaders::kSourceBytes);
            shaders::sky_fragment(fs, shaders::kSourceBytes);
            ok = compile(&sky, vs, fs, "sky");
        }
        free(vs);
        free(fs);
        if (!ok) return false;
        // The sky: one triangle that covers the screen.
        static const float tri[18] = { -1, -1, 0, 0, 0, 0,   3, -1, 0, 0, 0, 0,   -1, 3, 0, 0, 0, 0 };
        sky_buffer = fglCreateBuffer(gl, sizeof tri);
        if (!sky_buffer || fglBufferData(gl, sky_buffer, 0, tri, sizeof tri) != 0) {
            set_error("FarcadeGL: cannot create the sky buffer");
            return false;
        }
        return true;
    }

    // The lit shader for this mix of lights (compiled once, least recently
    // used ones are dropped).
    Shader* lit_variant(int nd, int np, int ns) {
        for (Variant& v : variants) {
            if (v.used && v.nd == nd && v.np == np && v.ns == ns) { v.last_use = frame; return &v.shader; }
        }
        Variant* slot = nullptr;
        for (Variant& v : variants) if (!v.used) { slot = &v; break; }
        if (!slot) {
            slot = &variants[0];
            for (Variant& v : variants) if (v.last_use < slot->last_use) slot = &v;
            drop(&slot->shader);
            slot->used = false;
        }
        char* vs = (char*)malloc(shaders::kSourceBytes);
        char* fs = (char*)malloc(shaders::kSourceBytes);
        if (!vs || !fs) { free(vs); free(fs); set_error("out of memory"); return nullptr; }
        shaders::lit_vertex(vs, shaders::kSourceBytes);
        shaders::lit_fragment(fs, shaders::kSourceBytes, nd, np, ns);
        Shader s;
        bool ok = compile(&s, vs, fs, "lit");
        free(vs);
        free(fs);
        if (!ok) return nullptr;
        slot->shader = s;
        slot->nd = nd; slot->np = np; slot->ns = ns;
        slot->used = true;
        slot->last_use = frame;
        return &slot->shader;
    }

    // ---- mesh cache

    static uint32_t hash_key(uint64_t k) {
        k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ull; k ^= k >> 33;
        return (uint32_t)k;
    }

    MeshEntry* find_entry(uint64_t key) {
        if (!table_cap) return nullptr;
        for (uint32_t i = hash_key(key) & (table_cap - 1);; i = (i + 1) & (table_cap - 1)) {
            MeshEntry& e = table[i];
            if (!e.used) return nullptr;
            if (e.key == key) return &e;
        }
    }

    void free_entry_buffers(MeshEntry& e) {
        for (uint32_t c = 0; c < e.chunks; c++) {
            if (e.buffer[c]) { fglDestroyBuffer(gl, e.buffer[c]); cached_buffers--; }
            e.buffer[c] = 0;
        }
        e.chunks = 0;
    }

    // Rebuilds the table with the entries accepted by keep (and grows it).
    bool rebuild(uint32_t want_cap, bool only_current_frame) {
        uint32_t cap = 64;
        while (cap < want_cap) cap *= 2;
        MeshEntry* nt = (MeshEntry*)calloc(cap, sizeof(MeshEntry));
        if (!nt) { set_error("out of memory"); return false; }
        uint32_t used = 0;
        for (uint32_t i = 0; i < table_cap; i++) {
            MeshEntry& e = table[i];
            if (!e.used) continue;
            if (only_current_frame && e.last_frame != frame) { free_entry_buffers(e); continue; }
            uint32_t j = hash_key(e.key) & (cap - 1);
            while (nt[j].used) j = (j + 1) & (cap - 1);
            nt[j] = e;
            used++;
        }
        free(table);
        table = nt;
        table_cap = cap;
        table_used = used;
        return true;
    }

    MeshEntry* insert_entry(uint64_t key) {
        if ((table_used + 1) * 2 > table_cap && !rebuild(table_cap * 2, false)) return nullptr;
        uint32_t i = hash_key(key) & (table_cap - 1);
        while (table[i].used) i = (i + 1) & (table_cap - 1);
        table[i] = MeshEntry();
        table[i].used = true;
        table[i].key = key;
        table_used++;
        return &table[i];
    }

    void forget_all() {
        for (uint32_t i = 0; i < table_cap; i++) if (table[i].used) free_entry_buffers(table[i]);
        free(table);
        table = nullptr;
        table_cap = table_used = 0;
        cached_buffers = 0;
    }

    // Creates the buffers of a mesh and fills them (position and normal
    // interleaved; a missing normal is the face normal).
    bool upload(const RenderMesh& m, uint32_t vc, FGLBuffer* bufs, uint32_t* counts, uint32_t* nchunks) {
        uint32_t chunks = (vc + kChunkVertices - 1) / kChunkVertices;
        if (chunks > (uint32_t)kMaxChunks) { set_error("mesh too large"); return false; }
        *nchunks = 0;
        for (uint32_t c = 0; c < chunks; c++) {
            uint32_t first = c * kChunkVertices;
            uint32_t n = vc - first < kChunkVertices ? vc - first : kChunkVertices;
            FGLBuffer b = fglCreateBuffer(gl, n * kVertexBytes);
            if (!b) { set_error("FarcadeGL: cannot create a vertex buffer"); return false; }
            bufs[c] = b;
            counts[c] = n;
            (*nchunks)++;
            for (uint32_t s = 0; s < n; s += kUploadSlice) {
                uint32_t k = n - s < kUploadSlice ? n - s : kUploadSlice;
                for (uint32_t v = 0; v < k; v += 3) {
                    uint32_t at = first + s + v;
                    Vec3 p0 = m.positions[at], p1 = m.positions[at + 1], p2 = m.positions[at + 2];
                    Vec3 n0, n1, n2;
                    if (m.normals) {
                        n0 = m.normals[at]; n1 = m.normals[at + 1]; n2 = m.normals[at + 2];
                    } else {
                        n0 = n1 = n2 = normalize(cross(p1 - p0, p2 - p0));
                    }
                    const Vec3 ps[3] = { p0, p1, p2 };
                    const Vec3 ns[3] = { n0, n1, n2 };
                    for (int t = 0; t < 3; t++) {
                        float* o = scratch + (v + t) * 6;
                        o[0] = ps[t].x; o[1] = ps[t].y; o[2] = ps[t].z;
                        o[3] = ns[t].x; o[4] = ns[t].y; o[5] = ns[t].z;
                    }
                }
                if (fglBufferData(gl, b, s * kVertexBytes, scratch, k * kVertexBytes) != 0) {
                    set_error("FarcadeGL: cannot upload a vertex buffer");
                    return false;
                }
            }
        }
        return true;
    }

    // The buffers of a mesh for this frame (nullptr and error set on failure).
    MeshEntry* mesh_buffers(const RenderMesh& m, MeshEntry* temp_entry) {
        uint32_t vc = m.vertex_count / 3 * 3;
        if (!m.positions || !vc) return nullptr;
        if (m.key == 0) {
            if (temp_count + kMaxChunks > kMaxTempBuffers) { set_error("too many uncached meshes in one frame"); return nullptr; }
            *temp_entry = MeshEntry();
            if (!upload(m, vc, temp_entry->buffer, temp_entry->chunk_vertices, &temp_entry->chunks)) {
                for (uint32_t c = 0; c < temp_entry->chunks; c++) fglDestroyBuffer(gl, temp_entry->buffer[c]);
                return nullptr;
            }
            for (uint32_t c = 0; c < temp_entry->chunks; c++) temp[temp_count++] = temp_entry->buffer[c];
            temp_entry->vertex_count = vc;
            stats.uploads++;
            return temp_entry;
        }
        MeshEntry* e = find_entry(m.key);
        if (e && e->vertex_count == vc) { e->last_frame = frame; return e; }
        if (e) free_entry_buffers(*e);                  // same key, other data: upload again
        uint32_t need = (vc + kChunkVertices - 1) / kChunkVertices;
        if (cached_buffers + (int)need > kMaxCachedBuffers) {
            // Drop what this frame has not used.
            if (!rebuild(table_cap, true)) return nullptr;
            e = find_entry(m.key);
            if (cached_buffers + (int)need > kMaxCachedBuffers) { set_error("too many meshes in one frame"); return nullptr; }
        }
        if (!e) {
            e = insert_entry(m.key);
            if (!e) return nullptr;
        }
        FGLBuffer bufs[kMaxChunks];
        uint32_t counts[kMaxChunks], chunks = 0;
        if (!upload(m, vc, bufs, counts, &chunks)) {
            for (uint32_t c = 0; c < chunks; c++) fglDestroyBuffer(gl, bufs[c]);
            e->vertex_count = 0;
            e->chunks = 0;
            return nullptr;
        }
        e->vertex_count = vc;
        e->chunks = chunks;
        e->last_frame = frame;
        for (uint32_t c = 0; c < chunks; c++) { e->buffer[c] = bufs[c]; e->chunk_vertices[c] = counts[c]; }
        cached_buffers += (int)chunks;
        stats.uploads++;
        return e;
    }

    void free_temps() {
        for (int i = 0; i < temp_count; i++) fglDestroyBuffer(gl, temp[i]);
        temp_count = 0;
    }

    // ---- lights

    static void put4(float* a, int i, float x, float y, float z, float w) {
        a[i * 4 + 0] = x; a[i * 4 + 1] = y; a[i * 4 + 2] = z; a[i * 4 + 3] = w;
    }

    // Picks up to kMaxLights lights, strongest first (directional lights
    // before the others, nearer and brighter point and spot lights first) and
    // packs them directional, point, spot.
    static void pack_lights(const RenderScene& s, LightSet* out) {
        Chosen pick[kMaxLights];
        int n = 0;
        float sun_score = 0;
        for (int i = 0; i < s.light_count; i++) {
            const RenderLight& l = s.lights[i];
            float power = l.intensity * luma(l.color);
            if (!(power > 0.0f)) continue;
            float score;
            if (l.type == LightType::Directional) {
                score = 1.0e6f + power;
                if (power > sun_score) {
                    sun_score = power;
                    out->has_sun = true;
                    out->sun_dir = normalize(-l.direction);
                    out->sun_color = l.color * l.intensity;
                }
            } else {
                Vec3 d = l.position - s.camera.position;
                score = power / (1.0f + dot(d, d));
            }
            // keep the best kMaxLights
            if (n < kMaxLights) {
                pick[n++] = { &l, score };
            } else {
                int worst = 0;
                for (int k = 1; k < n; k++) if (pick[k].score < pick[worst].score) worst = k;
                if (score > pick[worst].score) pick[worst] = { &l, score };
            }
        }
        out->nd = out->np = out->ns = 0;
        // Group by type: directional, point, spot.
        int idx = 0;
        for (int type = 0; type < 3; type++) {
            LightType want = type == 0 ? LightType::Directional : (type == 1 ? LightType::Point : LightType::Spot);
            for (int k = 0; k < n; k++) {
                const RenderLight& l = *pick[k].light;
                if (l.type != want) continue;
                Vec3 dir = length(l.direction) > 0.0f ? normalize(l.direction) : Vec3(0, -1, 0);
                Vec3 col = l.color * l.intensity;
                float range = l.range > 0.001f ? l.range : 0.001f;
                put4(out->l0, idx, l.position.x, l.position.y, l.position.z, range);
                put4(out->l1, idx, dir.x, dir.y, dir.z, 0);
                put4(out->l2, idx, col.x, col.y, col.z, 0);
                float outer = radians(fclamp(l.spot_angle * 0.5f, 0.5f, 89.0f));
                float blend = fclamp(l.spot_blend, 0.0f, 1.0f);
                float c_outer = fcos(outer);
                float c_inner = fcos(outer * (1.0f - blend));
                if (c_inner < c_outer + 1e-4f) c_inner = c_outer + 1e-4f;
                put4(out->l3, idx, c_outer, c_inner, 0, 0);
                idx++;
                if (type == 0) out->nd++;
                else if (type == 1) out->np++;
                else out->ns++;
            }
        }
    }

    // ---- uniforms

    void set_post(const Shader& s, const PostProcess& p) {
        const Locs& l = s.loc;
        fglUniform4f(s.sh, l.post, p.exposure, p.contrast, p.saturation, fclamp(p.vignette, 0.0f, 1.0f));
        fglUniform4f(s.sh, l.tint, p.tint.x, p.tint.y, p.tint.z, (float)(int)p.tonemap);
        fglUniform4f(s.sh, l.res, (float)width, (float)height, 0, 0);
    }

    void set_lit_frame(const Shader& s, const RenderScene& scene, const LightSet& ls) {
        const Locs& l = s.loc;
        set_post(s, scene.post);
        const Vec3& c = scene.camera.position;
        fglUniform4f(s.sh, l.cam, c.x, c.y, c.z, 0);
        const SkyLight& sl = scene.sky_light;
        fglUniform4f(s.sh, l.sky, sl.sky_color.x * sl.intensity, sl.sky_color.y * sl.intensity, sl.sky_color.z * sl.intensity, 0);
        fglUniform4f(s.sh, l.gnd, sl.ground_color.x * sl.intensity, sl.ground_color.y * sl.intensity, sl.ground_color.z * sl.intensity, 0);
        uint32_t total = (uint32_t)(ls.nd + ls.np + ls.ns);
        if (total) {
            fglUniformArray(s.sh, l.l0, 0, ls.l0, total);
            fglUniformArray(s.sh, l.l1, 0, ls.l1, total);
            fglUniformArray(s.sh, l.l2, 0, ls.l2, total);
            fglUniformArray(s.sh, l.l3, 0, ls.l3, total);
        }
    }

    void set_sky_frame(const RenderScene& scene, const LightSet& ls) {
        const Locs& l = sky.loc;
        const Sky& k = scene.sky;
        set_post(sky, scene.post);
        // The camera basis from the view matrix (its rows) and the projection.
        const Mat4& v = scene.camera.view;
        Vec3 right(v.m[0], v.m[4], v.m[8]), up(v.m[1], v.m[5], v.m[9]), back(v.m[2], v.m[6], v.m[10]);
        bool ortho = scene.camera.proj.m[15] == 1.0f && scene.camera.proj.m[11] == 0.0f;
        float px = scene.camera.proj.m[0], py = scene.camera.proj.m[5];
        Vec3 rx = ortho || px == 0 ? Vec3(0, 0, 0) : right * (1.0f / px);
        Vec3 ry = ortho || py == 0 ? Vec3(0, 0, 0) : up * (1.0f / py);
        fglUniform4f(sky.sh, l.fwd, -back.x, -back.y, -back.z, 0);
        fglUniform4f(sky.sh, l.rx, rx.x, rx.y, rx.z, 0);
        fglUniform4f(sky.sh, l.ry, ry.x, ry.y, ry.z, 0);
        float i = k.intensity;
        bool flat = k.mode == SkyMode::Color;
        fglUniform4f(sky.sh, l.zen, k.zenith.x * i, k.zenith.y * i, k.zenith.z * i, flat ? 1.0f : 0.0f);
        fglUniform4f(sky.sh, l.hor, k.horizon.x * i, k.horizon.y * i, k.horizon.z * i, 0);
        fglUniform4f(sky.sh, l.grd, k.ground.x * i, k.ground.y * i, k.ground.z * i, 0);
        if (ls.has_sun && k.sun_size > 0.0f && !flat) {
            float r = radians(fclamp(k.sun_size, 0.05f, 60.0f) * 0.5f);
            fglUniform4f(sky.sh, l.sun, ls.sun_dir.x, ls.sun_dir.y, ls.sun_dir.z, fcos(r));
            Vec3 c = ls.sun_color * 4.0f;
            fglUniform4f(sky.sh, l.suncol, c.x, c.y, c.z, 0);
        } else {
            fglUniform4f(sky.sh, l.sun, 0, 1, 0, 2.0f);         // cos > 1: no disc
            fglUniform4f(sky.sh, l.suncol, 0, 0, 0, 0);
        }
    }

    // ---- frame

    bool draw_item(const DrawItem& it, const Shader& s, bool* failed) {
        MeshEntry temp_entry;
        MeshEntry* e = mesh_buffers(it.mesh, &temp_entry);
        if (!e) {
            // An item without vertices is skipped; a real failure has set the error.
            *failed = error[0] != 0;
            return false;
        }
        // Keep the command buffer under the kernel's size limit.
        if ((gl->cmd_len + 4096u) * 4u > kCommandBudget && fglSubmit(gl) != 0) {
            set_error("FarcadeGL: submit failed", gl->error);
            *failed = true;
            return false;
        }
        // A mirroring model matrix (negative determinant) turns the triangles
        // inside out: draw both faces, the depth test keeps the right ones.
        const float* m = it.model.m;
        Vec3 c0(m[0], m[1], m[2]), c1(m[4], m[5], m[6]), c2(m[8], m[9], m[10]);
        if (dot(c0, cross(c1, c2)) < 0.0f) fglDisable(gl, FGL_CULL_FACE);
        else fglEnable(gl, FGL_CULL_FACE);
        fglUseShader(gl, s.sh);
        fglUniform4f(s.sh, s.loc.mat, it.color.x, it.color.y, it.color.z, it.roughness);
        fglSetModel(gl, to_fgl(it.model));
        uint32_t first = 0;
        for (uint32_t c = 0; c < e->chunks; c++) {
            fglDraw(gl, e->buffer[c], 0, e->chunk_vertices[c]);
            first += e->chunk_vertices[c];
        }
        stats.draws++;
        stats.triangles += (int)(first / 3);
        return true;
    }

    bool render(const RenderScene& scene) {
        double t0 = now_ms();
        stats = RendererStats();
        error[0] = 0;
        gl->error = 0;
        frame++;

        LightSet ls;
        pack_lights(scene, &ls);
        bool any_lit = false;
        for (int i = 0; i < scene.item_count; i++) if (!scene.items[i].unlit) { any_lit = true; break; }
        Shader* lit = nullptr;
        if (any_lit) {
            lit = lit_variant(ls.nd, ls.np, ls.ns);
            if (!lit) return false;
            set_lit_frame(*lit, scene, ls);
        }
        set_post(unlit, scene.post);
        set_sky_frame(scene, ls);

        fglClearColor(gl, 0, 0, 0);
        fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
        fglSetProjection(gl, to_fgl(scene.camera.proj));
        fglSetView(gl, to_fgl(scene.camera.view));
        fglEnable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);

        bool failed = false;
        for (int i = 0; i < scene.item_count && !failed; i++) {
            const DrawItem& it = scene.items[i];
            draw_item(it, it.unlit ? unlit : *lit, &failed);
        }
        if (!failed) {
            // The sky last, at the far plane: only uncovered pixels are shaded.
            fglDisable(gl, FGL_CULL_FACE);
            fglUseShader(gl, sky.sh);
            fglSetModel(gl, to_fgl(Mat4::identity()));
            fglDraw(gl, sky_buffer, 0, 3);
            if (fglSubmit(gl) != 0) { set_error("FarcadeGL: submit failed", gl->error); failed = true; }
        } else {
            fglSubmit(gl);
        }
        free_temps();
        if (failed) return false;
        if (fglReadPixels(gl, pixels, (uint32_t)width * 4) != 0) {
            set_error("FarcadeGL: readback failed", gl->error);
            return false;
        }
        // XRGB (blue in the low byte) to RGBA (red in the low byte), opaque.
        uint32_t n = (uint32_t)width * (uint32_t)height;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t p = pixels[i];
            pixels[i] = 0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
        }
        stats.ms = now_ms() - t0;
        return true;
    }

    void destroy_gl_objects() {
        if (gl) {
            free_temps();
            forget_all();
            for (Variant& v : variants) { if (v.used) drop(&v.shader); v.used = false; }
            drop(&unlit);
            drop(&sky);
            if (sky_buffer) fglDestroyBuffer(gl, sky_buffer);
            sky_buffer = 0;
            fglDestroyContext(gl);
            gl = nullptr;
        }
    }
};

Renderer::Renderer() = default;
Renderer::~Renderer() { shutdown(); }

bool Renderer::init(int width, int height) {
    shutdown();
    if (width < 1 || height < 1 || width > 4096 || height > 4096) {
        impl_ = new Impl();
        impl_->set_error("render size out of range (1..4096)");
        return false;
    }
    impl_ = new Impl();
    Impl& m = *impl_;
    m.width = width;
    m.height = height;
    if (!fglHasShaders()) { m.set_error("FarcadeGL: the GPU has no shader support"); return false; }
    m.scratch = (float*)malloc((size_t)kUploadSlice * kVertexBytes);
    m.pixels = (uint32_t*)calloc((size_t)width * (size_t)height, 4);
    if (!m.scratch || !m.pixels) { m.set_error("out of memory"); return false; }
    if (!m.make_context() || !m.make_shaders()) return false;
    return true;
}

void Renderer::shutdown() {
    if (!impl_) return;
    impl_->destroy_gl_objects();
    free(impl_->scratch);
    free(impl_->pixels);
    delete impl_;
    impl_ = nullptr;
}

bool Renderer::ready() const { return impl_ && impl_->gl && impl_->sky.sh; }

bool Renderer::resize(int width, int height) {
    if (!impl_) return init(width, height);
    Impl& m = *impl_;
    if (width < 1 || height < 1 || width > 4096 || height > 4096) {
        m.set_error("render size out of range (1..4096)");
        return false;
    }
    if (m.gl && width == m.width && height == m.height) return true;
    uint32_t* px = (uint32_t*)calloc((size_t)width * (size_t)height, 4);
    if (!px) { m.set_error("out of memory"); return false; }
    // Buffers and shaders belong to the device, not to the context: only the
    // surfaces are recreated.
    if (m.gl) fglDestroyContext(m.gl);
    m.gl = nullptr;
    free(m.pixels);
    m.pixels = px;
    m.width = width;
    m.height = height;
    return m.make_context();
}

bool Renderer::render(const RenderScene& scene) {
    if (!ready()) {
        if (impl_ && !impl_->error[0]) impl_->set_error("the renderer is not initialised");
        return false;
    }
    return impl_->render(scene);
}

const uint32_t* Renderer::pixels() const { return impl_ ? impl_->pixels : nullptr; }
int Renderer::width() const { return impl_ ? impl_->width : 0; }
int Renderer::height() const { return impl_ ? impl_->height : 0; }
const char* Renderer::error() const { return impl_ ? impl_->error : "the renderer is not initialised"; }

const RendererStats& Renderer::stats() const {
    static const RendererStats none;
    return impl_ ? impl_->stats : none;
}

void Renderer::forget_meshes() {
    if (impl_ && impl_->gl) impl_->forget_all();
}

}  // namespace fe::gfx
