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
#include "gfx/feature_level.h"
#include "gfx/post_stack.h"
#include "gfx/gi.h"
#include "gfx/passes.h"
#include "gfx/shader_file.h"
#include "stb/stb_image.h"

#if defined(FE_HOST)
#include "gfx/host/farcadegl_host.h"
#else
#include "farcadegl.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

namespace fe::gfx {

namespace {

constexpr int kMaxLights = shaders::kMaxLights;
constexpr uint32_t kVertexBytes = 32;               // position xyz, normal xyz, UV
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
    int fwd = -1, rx = -1, ry = -1, zen = -1, hor = -1, grd = -1, sun = -1, suncol = -1, texture = -1;
    int lift = -1, gamma = -1, gain = -1, grain = -1;
    int gi = -1, gi0 = -1, gi1 = -1;
};

struct Shader {
    FGLShader* sh = nullptr;
    Locs loc;
    shaders::PostKey key;                   // the post chain compiled in
};

struct Variant {
    Shader shader;
    int nd = 0, np = 0, ns = 0;
    shaders::PostKey key;
    bool gi = false;
    uint64_t last_use = 0;
    bool used = false;
};

struct TextureEntry {
    char path[256] = {};
    FGLTexture texture = 0;
    uint64_t last_use = 0;
    bool missing = false;           // could not be read or decoded: `texture` is the shared white one
};

struct CustomShaderEntry {
    char path[256] = {};
    Shader shader;
    uint64_t last_use = 0;
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
    int nps = 0, nss = 0;                   // shadowed point / spot lights at the start of their groups
    const RenderLight* local[kMaxLocalShadows] = {};
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

GraphicsFeatures make_features(const DeviceCaps& caps, const GraphicsSettings& settings) {
    GraphicsFeatures f;
    FeatureLevelChoice c = pick_feature_level(caps, settings.feature_level, settings.allow_gpu_effects_on_cpu);
    f.level = c.level;
    f.level_forced = c.forced;
    f.level_clamped = c.clamped;
    f.pass_caps = pass_caps(caps);
    f.gpu = caps.hw_3d;
    f.gpu_tier = c.level == FeatureLevel::GpuTier;
    f.shadows = f.gpu_tier && settings.shadows;
    f.ambient_occlusion = f.gpu_tier && settings.ambient_occlusion;
    f.bloom = f.gpu_tier && settings.bloom;
    f.antialiasing = f.gpu_tier && settings.antialiasing;
    f.pbr = f.gpu_tier && settings.pbr;
    f.local_shadows = f.gpu_tier && settings.local_shadows;
    f.gi = settings.gi_mode == GiMode::Probes;
    f.aa_mode = settings.aa_mode;
    f.ao_mode = settings.ao_mode;
    f.ssr = f.gpu_tier && settings.ssr;
    return f;
}

}  // namespace

struct Renderer::Impl {
    int width = 0, height = 0;              // the image
    int surf_w = 0, surf_h = 0;             // the surfaces: the image, or a band of it (render_rows)
    int band_y0 = 0;                        // the band being drawn: its first row in the image
    bool banding = false;
    FGLContext* gl = nullptr;
    Shader unlit[2], sky[2];                // [draft]
    Variant variants[kMaxVariants];
    FGLBuffer sky_buffer = 0;
    // Screen shaders (RenderScene::screen_shaders): their programs, the image as a texture, the readback.
    struct ScreenShader { char path[256] = {}; Shader shader; bool failed = false; uint64_t last_use = 0; };
    ScreenShader screen_progs[4];
    FGLTexture screen_tex = 0;
    int screen_tex_w = 0, screen_tex_h = 0;
    uint32_t* screen_buf = nullptr;         // XRGB: the image uploaded, then the pass read back
    size_t screen_cap = 0;
    double screen_time = 0;
    uint32_t* pixels = nullptr;
    char error[200] = "";
    char warning[256] = "";             // take_warning()
    RendererStats stats;
    double init_ms = 0, init_shader_ms = 0; // init(): all, and the shader compiles in it
    bool in_render = false;
    uint64_t frame = 0;

    // Post stack (post_stack.h): the internal resolution, auto exposure, the
    // values of this frame's post chain.
    int rw = 0, rh = 0;                     // the surfaces being drawn (internal size; a band: the image)
    uint32_t* ipixels = nullptr;            // the internal-size readback, when it differs from the image
    size_t ipixels_cap = 0;
    AutoExposure ae;
    DynamicResolution dyn;
    PostFrame pf;
    double last_render_end_ms = 0;
    uint32_t post_frames = 0;

    // GPU tier (graphics_settings.h, passes.h)
    GraphicsSettings settings;
    bool hw = false;                        // FGPU_CAP_HW_3D: FarcadeGL runs on a graphics card
    DeviceCaps caps;                        // from fglGetDeviceInfo (feature_level.h)
    char backend[40] = "";
    GpuTier tier;
    // Probe GI (gi.h): updated once a frame on the CPU, uploaded as a texture both tiers sample.
    GiSystem gi;
    FGLTexture gi_tex = 0;
    uint64_t gi_uploaded = 0;
    uint32_t* gi_pixels = nullptr;
    bool gi_started = false;
    struct GiFrame { bool on = false; float u0[4] = {}, u1[4] = {}; } gi_frame;
    bool tier_ran = false;                  // the last frame used the GPU tier

    // Particles (particles.h): drawn over the read-back image.
    ParticleRaster particle_raster;
    Occluder* occluders = nullptr;
    int occluder_cap = 0;
    void draw_particles(const RenderScene& scene, uint32_t* dst, int y0, int rows, bool rgba) {
        if ((!scene.particles || scene.particle_batch_count <= 0) && (!scene.labels || scene.label_count <= 0)) return;
        int n = scene.particles_depth_test ? scene.item_count : 0;
        if (occluder_cap < n) {
            Occluder* nb = (Occluder*)realloc(occluders, sizeof(Occluder) * (n < 64 ? 64 : n * 2));
            if (nb) { occluders = nb; occluder_cap = n < 64 ? 64 : n * 2; }
        }
        int m = 0;
        for (int i = 0; i < n && occluders && m < occluder_cap; i++) {
            const DrawItem& it = scene.items[i];
            occluders[m].positions = it.mesh.positions;
            occluders[m].vertex_count = it.mesh.vertex_count;
            occluders[m].model = it.model;
            m++;
        }
        particle_raster.draw(scene.particles, scene.particle_batch_count, scene.labels, scene.label_count, scene.camera.view, scene.camera.proj, occluders, m,
                             dst, width, height, y0, rows, rgba, scene.read_asset, scene.asset_user);
    }

    // Mesh cache: open addressing on key, entries never move between rebuilds.
    MeshEntry* table = nullptr;
    uint32_t table_cap = 0, table_used = 0;
    int cached_buffers = 0;
    FGLBuffer temp[kMaxTempBuffers];
    int temp_count = 0;
    float* scratch = nullptr;               // kUploadSlice interleaved vertices
    FGLTexture white_texture = 0;
    TextureEntry textures[128];
    CustomShaderEntry custom_shaders[32];

    void set_error(const char* what, int code = 0) {
        if (code) snprintf(error, sizeof error, "%s (error %d)", what, code);
        else snprintf(error, sizeof error, "%s", what);
    }

    // ---- context

    bool make_context(int w, int h) {
        FGLContextDesc d;
        memset(&d, 0, sizeof d);
        surf_w = w;
        surf_h = h;
        d.width = (uint32_t)w;
        d.height = (uint32_t)h;
        d.window_id = 0;                    // never presented: offscreen
        d.depth = 1;
        d.page_flip = 0;
        gl = fglCreateContext(&d);
        if (!gl) { set_error("FarcadeGL: cannot create the render surfaces"); return false; }
        static const FGLAttribute attrs[3] = { { 0, 3 }, { 12, 3 }, { 24, 2 } };
        fglVertexLayout(gl, kVertexBytes, 3, attrs);
        return true;
    }

    // Surfaces of w x h (buffers and shaders belong to the device, not to the
    // context: only the surfaces are recreated).
    bool ensure_surface(int w, int h) {
        if (gl && surf_w == w && surf_h == h) return true;
        if (gl) fglDestroyContext(gl);
        gl = nullptr;
        return make_context(w, h);
    }

    // A band is drawn through a viewport as big as the image, shifted up by
    // the band's first row: the rasterizer clips to the (smaller) surface.
    void emit_viewport() {
        if (pass_vp[2] > 0) fglViewport(gl, pass_vp[0], pass_vp[1], pass_vp[2], pass_vp[3]);
        else if (banding) fglViewport(gl, 0, -band_y0, width, height);
    }
    int pass_vp[4] = {};                    // a GPU tier pass's viewport (an atlas tile), 0 = none

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
        l->texture = fglUniformLocation(sh, "u_texture");
        l->lift = fglUniformLocation(sh, "u_lift");
        l->gamma = fglUniformLocation(sh, "u_gamma");
        l->gain = fglUniformLocation(sh, "u_gain");
        l->grain = fglUniformLocation(sh, "u_grain");
        l->gi = fglUniformLocation(sh, "u_gi");
        l->gi0 = fglUniformLocation(sh, "u_gi0");
        l->gi1 = fglUniformLocation(sh, "u_gi1");
    }

    CompileGate gate;

    bool compile(Shader* out, const char* vs, const char* fs, const char* what) {
        double t0 = now_ms();
        if (!gate.may_compile(t0)) { set_error("compiling shaders"); return false; }
        FGLShader* sh = fglCreateShader(gl, vs, fs);
        if (sh) gate.compiled();
        stats.shader_ms += now_ms() - t0;
        stats.shaders_compiled++;
        if (!in_render) init_shader_ms += now_ms() - t0;
        if (!sh) {
            snprintf(error, sizeof error, "%s shader: %s", what, fglShaderLog(gl));
            return false;
        }
        out->sh = sh;
        find_locs(sh, &out->loc);
        return true;
    }

    // A texture that cannot be read or decoded is not fatal: it is drawn
    // plain white (the object's color still shows), remembered so it is not
    // retried every frame, and reported once through take_warning().
    FGLTexture missing_texture(const char* path, const char* why) {
        snprintf(warning, sizeof warning, "texture %s: %s (drawn without it)", path, why);
        TextureEntry* slot = nullptr;
        for (TextureEntry& e : textures) if (!e.texture) { slot = &e; break; }
        if (!slot) {
            slot = &textures[0];
            for (TextureEntry& e : textures) if (e.last_use < slot->last_use) slot = &e;
            if (!slot->missing) fglDestroyTexture(gl, slot->texture);
        }
        snprintf(slot->path, sizeof slot->path, "%s", path);
        slot->texture = white_texture;
        slot->missing = true;
        slot->last_use = frame;
        return white_texture;
    }

    FGLTexture texture_for(const char* path, const RenderScene& scene) {
        for (TextureEntry& e : textures) {
            if (e.texture && strcmp(e.path, path) == 0) { e.last_use = frame; return e.texture; }
        }
        if (!scene.read_asset) { set_error("FarcadeGL: no asset reader for texture"); return 0; }
        size_t size = 0;
        uint8_t* bytes = (uint8_t*)scene.read_asset(path, &size, scene.asset_user);
        if (!bytes) return missing_texture(path, "cannot be read");
        if (size == 0 || size > 128u * 1024u * 1024u || size > 0x7FFFFFFFu) {
            free(bytes);
            return missing_texture(path, "has an invalid size");
        }
        int w = 0, h = 0, comp = 0;
        if (!stbi_info_from_memory(bytes, (int)size, &w, &h, &comp) || w < 1 || h < 1 || w > 4096 || h > 4096) {
            free(bytes);
            return missing_texture(path, "is not an image this renderer reads (or larger than 4096)");
        }
        stbi_uc* rgba = stbi_load_from_memory(bytes, (int)size, &w, &h, &comp, 4);
        free(bytes);
        if (!rgba) return missing_texture(path, "cannot be decoded");
        uint32_t* pixels = (uint32_t*)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
        if (!pixels) { stbi_image_free(rgba); set_error("out of memory decoding texture"); return 0; }
        for (size_t i = 0, n = (size_t)w * h; i < n; i++)
            pixels[i] = ((uint32_t)rgba[i * 4] << 16) | ((uint32_t)rgba[i * 4 + 1] << 8) | rgba[i * 4 + 2];
        stbi_image_free(rgba);
        FGLTexture texture = fglCreateTexture(gl, (uint32_t)w, (uint32_t)h, pixels);
        free(pixels);
        if (!texture) { snprintf(error, sizeof error, "FarcadeGL: cannot upload texture %s", path); return 0; }
        TextureEntry* slot = nullptr;
        for (TextureEntry& e : textures) if (!e.texture) { slot = &e; break; }
        if (!slot) {
            slot = &textures[0];
            for (TextureEntry& e : textures) if (e.last_use < slot->last_use) slot = &e;
            if (!slot->missing) fglDestroyTexture(gl, slot->texture);
        }
        snprintf(slot->path, sizeof slot->path, "%s", path);
        slot->texture = texture;
        slot->missing = false;
        slot->last_use = frame;
        return texture;
    }

    Shader* custom_shader(const char* path, const RenderScene& scene) {
        for (CustomShaderEntry& e : custom_shaders)
            if (e.shader.sh && strcmp(e.path, path) == 0) { e.last_use = frame; return &e.shader; }
        if (!scene.read_asset) { set_error("FarcadeGL: no asset reader for shader"); return nullptr; }
        size_t size = 0;
        char* source = (char*)scene.read_asset(path, &size, scene.asset_user);
        if (!source) { snprintf(error, sizeof error, "FarcadeGL: cannot read shader %s", path); return nullptr; }
        if (size > shaders::kSourceBytes * 2) {
            free(source);
            snprintf(error, sizeof error, "FarcadeGL: shader file is too large (%s)", path);
            return nullptr;
        }
        char* vs = (char*)malloc(shaders::kSourceBytes);
        char* fs = (char*)malloc(shaders::kSourceBytes);
        if (!vs || !fs) {
            free(source); free(vs); free(fs);
            set_error("out of memory loading shader");
            return nullptr;
        }
        char parse_error[96];
        bool parsed = parse_shader_file(source, size, vs, shaders::kSourceBytes, fs, shaders::kSourceBytes,
                                        parse_error, sizeof parse_error);
        free(source);
        if (!parsed) {
            snprintf(error, sizeof error, "FarcadeGL: %s (%s)", path, parse_error);
            free(vs); free(fs);
            return nullptr;
        }
        Shader shader;
        bool ok = compile(&shader, vs, fs, path);
        free(vs); free(fs);
        if (!ok) return nullptr;
        CustomShaderEntry* slot = nullptr;
        for (CustomShaderEntry& e : custom_shaders) if (!e.shader.sh) { slot = &e; break; }
        if (!slot) {
            slot = &custom_shaders[0];
            for (CustomShaderEntry& e : custom_shaders) if (e.last_use < slot->last_use) slot = &e;
            drop(&slot->shader);
        }
        snprintf(slot->path, sizeof slot->path, "%s", path);
        slot->shader = shader;
        slot->last_use = frame;
        return &slot->shader;
    }

    // ---- screen shaders

    Shader* screen_shader(const char* path, const RenderScene& scene) {
        for (ScreenShader& e : screen_progs)
            if (e.path[0] && strcmp(e.path, path) == 0) { e.last_use = frame; return e.failed ? nullptr : &e.shader; }
        ScreenShader* slot = &screen_progs[0];
        for (ScreenShader& e : screen_progs) {
            if (!e.path[0]) { slot = &e; break; }
            if (e.last_use < slot->last_use) slot = &e;
        }
        if (slot->path[0]) { drop(&slot->shader); *slot = ScreenShader(); }
        if (!gate.may_compile(now_ms())) return nullptr;            // next frame (compile budget)
        snprintf(slot->path, sizeof slot->path, "%s", path);
        slot->last_use = frame;
        slot->failed = true;                                        // until it compiled
        if (!scene.read_asset) return nullptr;
        size_t size = 0;
        char* source = (char*)scene.read_asset(path, &size, scene.asset_user);
        if (!source) { snprintf(warning, sizeof warning, "screen shader %s: cannot read it", path); return nullptr; }
        // The fragment part alone gets the standard vertex part: a full screen triangle with `uv`.
        static const char kVertex[] =
            "// @vertex\n"
            "attribute vec3 position;\n"
            "varying vec2 uv;\n"
            "void main() {\n"
            "    uv = vec2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);\n"
            "    gl_Position = vec4(position.xy, 0.0, 1.0);\n"
            "}\n";
        bool has_vertex = strstr(source, "@vertex") != nullptr;
        bool has_fragment = strstr(source, "@fragment") != nullptr;
        size_t cap = size + sizeof kVertex + 32;
        char* full = (char*)malloc(cap);
        char* vs = (char*)malloc(shaders::kSourceBytes);
        char* fs = (char*)malloc(shaders::kSourceBytes);
        bool ok = full && vs && fs && size < shaders::kSourceBytes;
        if (ok) {
            size_t n = 0;
            if (!has_vertex) { memcpy(full, kVertex, sizeof kVertex - 1); n = sizeof kVertex - 1; }
            if (!has_vertex && !has_fragment) { memcpy(full + n, "// @fragment\n", 13); n += 13; }
            memcpy(full + n, source, size);
            n += size;
            char perr[96];
            ok = parse_shader_file(full, n, vs, shaders::kSourceBytes, fs, shaders::kSourceBytes, perr, sizeof perr);
            if (!ok) snprintf(warning, sizeof warning, "screen shader %s: %s", path, perr);
        } else {
            snprintf(warning, sizeof warning, "screen shader %s: too large", path);
        }
        free(source);
        free(full);
        if (ok) {
            char keep[sizeof error];
            snprintf(keep, sizeof keep, "%s", error);
            ok = compile(&slot->shader, vs, fs, path);
            if (!ok) {
                snprintf(warning, sizeof warning, "screen shader %s", error);
                snprintf(error, sizeof error, "%s", keep);
            }
        }
        free(vs);
        free(fs);
        slot->failed = !ok;
        return ok ? &slot->shader : nullptr;
    }

    // Runs the screen shaders over `pixels` (the finished RGBA image): each pass draws a full screen
    // triangle into the context's surface (rw x rh) reading the image as a texture, and is read back.
    // A shader that cannot be read or compiled is skipped (take_warning tells once).
    void apply_screen_shaders(const RenderScene& scene, float dt) {
        screen_time += dt;
        size_t img = (size_t)width * (size_t)height, rb = (size_t)rw * (size_t)rh;
        size_t need = img > rb ? img : rb;
        if (screen_cap < need) {
            free(screen_buf);
            screen_buf = (uint32_t*)malloc(need * 4);
            screen_cap = screen_buf ? need : 0;
            if (!screen_buf) return;
        }
        for (int i = 0; i < scene.screen_shader_count && i < 4; i++) {
            const char* path = scene.screen_shaders[i];
            if (!path || !path[0]) continue;
            Shader* sh = screen_shader(path, scene);
            if (!sh || !sh->sh) continue;
            // The image so far, as XRGB, into the texture.
            for (size_t k = 0; k < img; k++) {
                uint32_t p = pixels[k];
                screen_buf[k] = ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
            }
            if (!screen_tex || screen_tex_w != width || screen_tex_h != height) {
                if (screen_tex) fglDestroyTexture(gl, screen_tex);
                screen_tex = fglCreateTexture(gl, (uint32_t)width, (uint32_t)height, screen_buf);
                screen_tex_w = width;
                screen_tex_h = height;
                if (!screen_tex) { screen_tex_w = screen_tex_h = 0; set_warning_once("screen shaders: cannot create the image texture"); return; }
            } else if (fglTextureData(gl, screen_tex, screen_buf, (uint32_t)width * 4) != 0) {
                set_warning_once("screen shaders: cannot upload the image");
                return;
            }
            FGLShader* p = sh->sh;
            fglClearColor(gl, 0, 0, 0);
            fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
            fglDisable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
            fglUseShader(gl, p);
            fglUniform4f(p, fglUniformLocation(p, "u_res"), (float)width, (float)height, 1.0f / (float)width, 1.0f / (float)height);
            fglUniform4f(p, fglUniformLocation(p, "u_time"), (float)screen_time, dt, (float)frame, 0.0f);
            fglUniform4f(p, fglUniformLocation(p, "u_params"), scene.screen_params[0], scene.screen_params[1], scene.screen_params[2], scene.screen_params[3]);
            fglUniformTexture(gl, p, fglUniformLocation(p, "u_screen"), screen_tex, FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP);
            fglSetModel(gl, to_fgl(Mat4::identity()));
            fglDraw(gl, sky_buffer, 0, 3);
            if (submit() != 0 || fglReadPixels(gl, screen_buf, (uint32_t)rw * 4) != 0) {
                set_warning_once("screen shaders: the pass failed");
                return;
            }
            upscale_xrgb_to_rgba(screen_buf, rw, rh, pixels, width, height);
            stats.screen_passes++;
        }
    }
    void set_warning_once(const char* w) {
        if (!warning[0]) snprintf(warning, sizeof warning, "%s", w);
    }

    void drop(Shader* s) {
        if (s->sh) fglDestroyShader(gl, s->sh);
        *s = Shader();
    }

    // The unlit and the sky shader (compiled on first use, cached).
    bool use_shader(Shader* s, bool sky_shader, const shaders::PostKey& key) {
        if (s->sh && s->key == key) return true;
        if (s->sh) drop(s);
        char* vs = (char*)malloc(shaders::kSourceBytes);
        char* fs = (char*)malloc(shaders::kSourceBytes);
        if (!vs || !fs) { free(vs); free(fs); set_error("out of memory"); return false; }
        if (sky_shader) { shaders::sky_vertex(vs, shaders::kSourceBytes); shaders::sky_fragment(fs, shaders::kSourceBytes, key); }
        else { shaders::unlit_vertex(vs, shaders::kSourceBytes); shaders::unlit_fragment(fs, shaders::kSourceBytes, key); }
        bool ok = compile(s, vs, fs, sky_shader ? "sky" : "unlit");
        if (ok) s->key = key;
        free(vs);
        free(fs);
        return ok;
    }

    bool make_shaders() {
        const uint32_t white = 0x00FFFFFFu;
        white_texture = fglCreateTexture(gl, 1, 1, &white);
        if (!white_texture) { set_error("FarcadeGL: cannot create the default texture"); return false; }
        // The sky: one triangle that covers the screen.
        static const float tri[24] = {
            -1, -1, 0, 0, 0, 0, 0, 0,
             3, -1, 0, 0, 0, 0, 1, 0,
            -1,  3, 0, 0, 0, 0, 0, 1
        };
        sky_buffer = fglCreateBuffer(gl, sizeof tri);
        if (!sky_buffer || fglBufferData(gl, sky_buffer, 0, tri, sizeof tri) != 0) {
            set_error("FarcadeGL: cannot create the sky buffer");
            return false;
        }
        return true;
    }

    // The lit shader for this mix of lights (compiled once, least recently
    // used ones are dropped).
    Shader* lit_variant(int nd, int np, int ns, const shaders::PostKey& key, bool gi) {
        for (Variant& v : variants) {
            if (v.used && v.nd == nd && v.np == np && v.ns == ns && v.key == key && v.gi == gi) { v.last_use = frame; return &v.shader; }
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
        shaders::lit_fragment(fs, shaders::kSourceBytes, nd, np, ns, key, gi);
        Shader s;
        bool ok = compile(&s, vs, fs, "lit");
        free(vs);
        free(fs);
        if (!ok) return nullptr;
        slot->shader = s;
        slot->nd = nd; slot->np = np; slot->ns = ns; slot->key = key; slot->gi = gi;
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
                        float* o = scratch + (v + t) * 8;
                        o[0] = ps[t].x; o[1] = ps[t].y; o[2] = ps[t].z;
                        o[3] = ns[t].x; o[4] = ns[t].y; o[5] = ns[t].z;
                        if (m.uvs) {
                            o[6] = m.uvs[(size_t)(at + t) * 2]; o[7] = m.uvs[(size_t)(at + t) * 2 + 1];
                        } else if (fabsf(ns[t].y) > fabsf(ns[t].x) && fabsf(ns[t].y) > fabsf(ns[t].z)) {
                            o[6] = ps[t].x + 0.5f; o[7] = ps[t].z + 0.5f;
                        } else if (fabsf(ns[t].x) > fabsf(ns[t].z)) {
                            o[6] = ps[t].z + 0.5f; o[7] = ps[t].y + 0.5f;
                        } else {
                            o[6] = ps[t].x + 0.5f; o[7] = ps[t].y + 0.5f;
                        }
                    }
                }
                double tu = now_ms();
                int ur = fglBufferData(gl, b, s * kVertexBytes, scratch, k * kVertexBytes);
                stats.upload_ms += now_ms() - tu;
                if (ur != 0) {
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
    // max_local > 0: up to that many point / spot lights with Shadows on get
    // an atlas shadow (best score first, while the atlas has tiles: a spot
    // takes 1, a point 6); they go first in their group.
    static void pack_lights(const RenderScene& s, LightSet* out, int max_local = 0) {
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
        out->nps = out->nss = 0;
        bool shadowed[kMaxLights] = {};
        if (max_local > 0) {
            if (max_local > kMaxLocalShadows) max_local = kMaxLocalShadows;
            int tiles = kAtlasTiles * kAtlasTiles, count = 0;
            for (;;) {
                int best = -1;
                for (int k = 0; k < n; k++) {
                    const RenderLight& l = *pick[k].light;
                    if (shadowed[k] || !l.shadows || l.type == LightType::Directional) continue;
                    if ((l.type == LightType::Point ? 6 : 1) > tiles) continue;
                    if (best < 0 || pick[k].score > pick[best].score) best = k;
                }
                if (best < 0 || count == max_local) break;
                shadowed[best] = true;
                tiles -= pick[best].light->type == LightType::Point ? 6 : 1;
                count++;
            }
        }
        // Group by type: directional, point, spot; within a group the
        // shadowed lights first.
        int idx = 0;
        for (int pass = 0; pass < 5; pass++) {
            // 0 directional, 1 shadowed points, 2 points, 3 shadowed spots, 4 spots
            int type = pass == 0 ? 0 : (pass <= 2 ? 1 : 2);
            bool want_shadow = pass == 1 || pass == 3;
            LightType want = type == 0 ? LightType::Directional : (type == 1 ? LightType::Point : LightType::Spot);
            for (int k = 0; k < n; k++) {
                const RenderLight& l = *pick[k].light;
                if (l.type != want) continue;
                if (type != 0 && shadowed[k] != want_shadow) continue;
                if (want_shadow) {
                    out->local[out->nps + out->nss] = &l;
                    if (type == 1) out->nps++; else out->nss++;
                }
                Vec3 dir = length(l.direction) > 0.0f ? normalize(l.direction) : Vec3(0, -1, 0);
                Vec3 col = l.color * l.intensity;
                float range = l.range > 0.001f ? l.range : 0.001f;
                put4(out->l0, idx, l.position.x, l.position.y, l.position.z, range);
                // The spot cone is in l3 and, for the GPU tier's shaders (short of
                // uniform space), also in the w of l1 / l2 (the base tier reads xyz only).
                float outer_c = fcos(radians(fclamp(l.spot_angle * 0.5f, 0.5f, 89.0f)));
                float inner_c = fcos(radians(fclamp(l.spot_angle * 0.5f, 0.5f, 89.0f)) * (1.0f - fclamp(l.spot_blend, 0.0f, 1.0f)));
                if (inner_c < outer_c + 1e-4f) inner_c = outer_c + 1e-4f;
                put4(out->l1, idx, dir.x, dir.y, dir.z, outer_c);
                put4(out->l2, idx, col.x, col.y, col.z, inner_c);
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
        fglUniform4f(s.sh, l.post, pf.exposure_ev, p.contrast, p.saturation, fclamp(p.vignette, 0.0f, 1.0f));
        fglUniform4f(s.sh, l.tint, p.tint.x, p.tint.y, p.tint.z, (float)(int)p.tonemap);
        fglUniform4f(s.sh, l.res, (float)rw, (float)rh, 0, banding ? (float)band_y0 : 0.0f);
        if (l.lift >= 0) {
            fglUniform4f(s.sh, l.lift, pf.lift[0], pf.lift[1], pf.lift[2], 0);
            fglUniform4f(s.sh, l.gamma, pf.inv_gamma[0], pf.inv_gamma[1], pf.inv_gamma[2], 0);
            fglUniform4f(s.sh, l.gain, pf.gain[0], pf.gain[1], pf.gain[2], 0);
        }
        if (l.grain >= 0) fglUniform4f(s.sh, l.grain, pf.grain[0], pf.grain[1], pf.grain[2], 0);
    }

    void set_lit_frame(const Shader& s, const RenderScene& scene, const LightSet& ls) {
        const Locs& l = s.loc;
        set_post(s, scene.post);
        const Vec3& c = scene.camera.position;
        fglUniform4f(s.sh, l.cam, c.x, c.y, c.z, 0);
        const SkyLight& sl = scene.sky_light;
        fglUniform4f(s.sh, l.sky, sl.sky_color.x * sl.intensity, sl.sky_color.y * sl.intensity, sl.sky_color.z * sl.intensity, 0);
        fglUniform4f(s.sh, l.gnd, sl.ground_color.x * sl.intensity, sl.ground_color.y * sl.intensity, sl.ground_color.z * sl.intensity, 0);
        if (gi_frame.on && l.gi >= 0) {
            fglUniform4f(s.sh, l.gi0, gi_frame.u0[0], gi_frame.u0[1], gi_frame.u0[2], gi_frame.u0[3]);
            fglUniform4f(s.sh, l.gi1, gi_frame.u1[0], gi_frame.u1[1], gi_frame.u1[2], gi_frame.u1[3]);
            fglUniformTexture(gl, s.sh, l.gi, gi_tex, FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP);
        }
        uint32_t total = (uint32_t)(ls.nd + ls.np + ls.ns);
        if (total) {
            fglUniformArray(s.sh, l.l0, 0, ls.l0, total);
            fglUniformArray(s.sh, l.l1, 0, ls.l1, total);
            fglUniformArray(s.sh, l.l2, 0, ls.l2, total);
            fglUniformArray(s.sh, l.l3, 0, ls.l3, total);
        }
    }

    void set_sky_frame(Shader& sky, const RenderScene& scene, const LightSet& ls) {
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

    int submit() {
        double t = now_ms();
        int r = fglSubmit(gl);
        stats.submit_ms += now_ms() - t;
        return r;
    }

    // Binds the mesh of `it` and draws it with the shader and uniforms set by the caller.
    bool draw_mesh(const DrawItem& it, bool count, bool* failed) {
        MeshEntry temp_entry;
        MeshEntry* e = mesh_buffers(it.mesh, &temp_entry);
        if (!e) {
            // An item without vertices is skipped; a real failure has set the error.
            *failed = error[0] != 0;
            return false;
        }
        // Keep the command buffer under the kernel's size limit.
        if ((gl->cmd_len + 4096u) * 4u > kCommandBudget) {
            if (submit() != 0) {
                set_error("FarcadeGL: submit failed", gl->error);
                *failed = true;
                return false;
            }
            emit_viewport();                // the next buffer binds the target again, which resets it
        }
        // A mirroring model matrix (negative determinant) turns the triangles
        // inside out: draw both faces, the depth test keeps the right ones.
        const float* m = it.model.m;
        Vec3 c0(m[0], m[1], m[2]), c1(m[4], m[5], m[6]), c2(m[8], m[9], m[10]);
        if (dot(c0, cross(c1, c2)) < 0.0f) fglDisable(gl, FGL_CULL_FACE);
        else fglEnable(gl, FGL_CULL_FACE);
        fglSetModel(gl, to_fgl(it.model));
        uint32_t first = 0;
        for (uint32_t c = 0; c < e->chunks; c++) {
            fglDraw(gl, e->buffer[c], 0, e->chunk_vertices[c]);
            first += e->chunk_vertices[c];
        }
        if (count) {
            stats.draws++;
            stats.triangles += (int)(first / 3);
        }
        return true;
    }

    bool draw_item(const DrawItem& it, const Shader& s, const RenderScene& scene, bool* failed) {
        FGLTexture texture = it.texture_path && it.texture_path[0] ? texture_for(it.texture_path, scene) : white_texture;
        if (!texture) { *failed = true; return false; }
        fglUseShader(gl, s.sh);
        fglUniform4f(s.sh, s.loc.mat, it.color.x, it.color.y, it.color.z, it.roughness);
        fglUniformTexture(gl, s.sh, s.loc.texture, texture, FGL_TEXTURE_LINEAR | FGL_TEXTURE_CLAMP);
        return draw_mesh(it, true, failed);
    }

    // ---- GPU tier hook: the passes draw their meshes through this
    static bool tier_draw_mesh(void* user, const DrawItem& it, bool count, bool* failed) {
        return static_cast<Impl*>(user)->draw_mesh(it, count, failed);
    }
    static void tier_set_viewport(void* user, int x, int y, int w, int h) {
        Impl* I = static_cast<Impl*>(user);
        I->pass_vp[0] = x; I->pass_vp[1] = y; I->pass_vp[2] = w; I->pass_vp[3] = h;
        if (w > 0) fglViewport(I->gl, x, y, w, h);
    }

    // The one place where caps + settings become what runs (feature_level.h).
    GraphicsFeatures feature_set() const { return make_features(caps, settings); }

    bool update_gi(const RenderScene& scene, float dt) {
        double t0 = now_ms();
        GiInput in;
        in.items = scene.items;
        in.item_count = scene.item_count;
        in.lights = scene.lights;
        in.light_count = scene.light_count;
        const SkyLight& sl = scene.sky_light;
        in.sky_color = sl.sky_color * sl.intensity;
        in.ground_color = sl.ground_color * sl.intensity;
        in.camera = scene.camera.position;
        in.dt = dt;
        GiParams gp;
        gp.density = settings.gi_density;
        gp.update = settings.gi_update;
        gp.bounce = scene.sky_light.gi_bounce;
        gp.intensity = scene.sky_light.gi_intensity;
        gi.update(in, gp, !gi_started);
        gi_started = true;
        if (!gi_pixels) gi_pixels = (uint32_t*)malloc((size_t)GiSystem::kTexW * GiSystem::kTexH * 4);
        if (!gi_pixels) { set_error("out of memory"); return false; }
        if (!gi_tex || gi_uploaded != gi.version()) {
            gi.encode(gi_pixels);
            if (!gi_tex) gi_tex = fglCreateTexture(gl, GiSystem::kTexW, GiSystem::kTexH, gi_pixels);
            else if (fglTextureData(gl, gi_tex, gi_pixels, GiSystem::kTexW * 4) != 0) { set_error("FarcadeGL: cannot upload the GI probes"); return false; }
            if (!gi_tex) { set_error("FarcadeGL: cannot create the GI texture"); return false; }
            gi_uploaded = gi.version();
        }
        gi.uniforms(gi_frame.u0, gi_frame.u1);
        gi_frame.on = true;
        stats.gi_ms = now_ms() - t0;
        return true;
    }

    // rows == 0: the whole image into `pixels` (as RGBA); else rows
    // [y0, y0 + rows) into dst (XRGB, width * rows words). Bands are not
    // available while the GPU tier or fog is on (render_rows fails then).
    bool render(const RenderScene& scene, int y0 = 0, int rows = 0, uint32_t* dst = nullptr) {
        double t0 = now_ms();
        stats = RendererStats();
        in_render = true;
        error[0] = 0;
        banding = rows > 0;
        band_y0 = y0;

        // Internal resolution (post_stack.h): the passes draw at iw x ih and the image is upscaled
        // when read back. Bands are always drawn at full size (the editor has its own adaptive resolution).
        float scale = 1.0f;
        if (!banding) {
            scale = fclamp(settings.render_scale, 0.25f, 1.0f);
            if (settings.dynamic_resolution) {
                float lo = fclamp(settings.min_render_scale, 0.25f, scale);
                if (dyn.scale > scale || dyn.scale < lo) dyn.scale = fclamp(dyn.scale, lo, scale);
                scale = dyn.scale;
            } else {
                dyn.scale = scale;
            }
        }
        int iw = DynamicResolution::internal_size(width, scale);
        int ih = DynamicResolution::internal_size(height, scale);
        // Supersampling: more pixels than the image, averaged down when read back (never in bands, never
        // above 4096 a side).
        if (!banding && settings.ssaa != SSAAMode::Off && !scene.draft) {
            float ss = (settings.ssaa == SSAAMode::X4 ? 2.0f : 1.41421356f) * scale;
            float lim = 4096.0f / (float)(width > height ? width : height);
            if (ss > lim) ss = lim;
            if (ss > 1.01f) {
                iw = (int)((float)width * ss + 0.5f);
                ih = (int)((float)height * ss + 0.5f);
            }
        }
        // TAA drives the upscale: the passes before its resolve draw at the internal size and the resolve writes the
        // full size image (so there is no CPU upscale). Only when the GPU tier will really run.
        bool taa_up = false;
        if (!banding && (iw < width || ih < height) && settings.antialiasing && settings.aa_mode == AAMode::TAA && !scene.draft) {
            GpuPlan tp = GpuTier::plan(scene, feature_set());
            bool assets = false;
            for (int i = 0; i < scene.item_count; i++)
                assets |= (scene.items[i].texture_path && scene.items[i].texture_path[0]) ||
                          (scene.items[i].shader_path && scene.items[i].shader_path[0]);
            taa_up = tp.any() && tp.aa && !assets;
        }
        rw = banding || taa_up ? width : iw;
        rh = banding || taa_up ? height : ih;
        stats.render_scale = (float)iw / (float)width;
        stats.taa_upscale = taa_up;
        stats.internal_width = taa_up ? iw : rw;
        stats.internal_height = banding ? rows : (taa_up ? ih : rh);
        if (!ensure_surface(rw, banding ? rows : rh)) return false;
        gl->error = 0;
        frame++;

        // This frame's post chain values: the exposure is the manual one plus the automatic one (measured
        // from the previous frame). Bands of one image share the values of the image.
        float dt = scene.delta_time;
        if (dt < 0) dt = last_render_end_ms > 0 ? fclamp((float)((t0 - last_render_end_ms) * 0.001), 0.0f, 0.25f) : 1.0f / 60.0f;
        if (!banding) post_frames++;
        pf = PostFrame::make(scene.post, scene.post.auto_exposure ? ae.ev : 0.0f, post_frames, scene.draft, false);
        stats.exposure_ev = pf.exposure_ev;

        // Probe GI: update on the CPU, upload when the probes changed (not for bands, not for draft frames)
        gi_frame.on = false;
        if (settings.gi_mode == GiMode::Probes && !scene.draft && !banding) {
            if (!update_gi(scene, dt)) return false;
        } else if (settings.gi_mode == GiMode::Probes && !scene.draft && gi_tex && gi_started) {
            gi_frame.on = true;                     // bands of one image use the probes of that image
            gi.uniforms(gi_frame.u0, gi_frame.u1);
        }

        // The GPU tier replaces the main pass below (passes.cpp); the readback is shared.
        GpuPlan plan = GpuTier::plan(scene, feature_set());
        LightSet ls;
        pack_lights(scene, &ls, plan.local_shadows ? settings.local_shadow_lights : 0);
        bool has_asset_material = false;
        for (int i = 0; i < scene.item_count; i++)
            has_asset_material |= (scene.items[i].texture_path && scene.items[i].texture_path[0]) ||
                                  (scene.items[i].shader_path && scene.items[i].shader_path[0]);
        bool run_tier = plan.any() && !has_asset_material;
        if (run_tier && banding) {
            set_error("FarcadeGL: bands are not available with the GPU tier or fog");
            in_render = false;
            return false;
        }
        bool failed = false;
        bool tier_done = false;
        tier_ran = run_tier;
        if (tier_ran) {
            PackedLights pl;
            pl.l0 = ls.l0; pl.l1 = ls.l1; pl.l2 = ls.l2; pl.l3 = ls.l3;
            pl.nd = ls.nd; pl.np = ls.np; pl.ns = ls.ns;
            pl.has_sun = ls.has_sun; pl.sun_dir = ls.sun_dir; pl.sun_color = ls.sun_color;
            pl.nps = ls.nps; pl.nss = ls.nss;
            for (int i = 0; i < ls.nps + ls.nss; i++) pl.local[i] = ls.local[i];
            PassHost host;
            host.user = this;
            host.draw_mesh = &Impl::tier_draw_mesh;
            host.set_viewport = &Impl::tier_set_viewport;
            if (taa_up) { host.iw = iw; host.ih = ih; }
            host.gi_on = gi_frame.on;
            host.gi_texture = gi_tex;
            memcpy(host.gi0, gi_frame.u0, sizeof host.gi0);
            memcpy(host.gi1, gi_frame.u1, sizeof host.gi1);
            failed = !tier.render(gl, scene, pl, settings, plan, host, pf, error, (int)sizeof error);
            if (failed && !error[0]) set_error("FarcadeGL: the GPU tier failed");
            tier_done = true;
        }
        if (!tier_done) {
        bool any_lit = false;
        for (int i = 0; i < scene.item_count; i++) if (!scene.items[i].unlit) { any_lit = true; break; }
        Shader* lit = nullptr;
        if (any_lit) {
            lit = lit_variant(ls.nd, ls.np, ls.ns, pf.key, gi_frame.on);
            if (!lit) return false;
            set_lit_frame(*lit, scene, ls);
        }
        Shader& unlit_s = unlit[scene.draft ? 1 : 0];
        Shader& sky_s = sky[scene.draft ? 1 : 0];
        bool any_unlit = false;
        for (int i = 0; i < scene.item_count; i++) if (scene.items[i].unlit) { any_unlit = true; break; }
        if ((any_unlit && !use_shader(&unlit_s, false, pf.key)) || !use_shader(&sky_s, true, pf.key)) return false;
        if (any_unlit) set_post(unlit_s, scene.post);
        set_sky_frame(sky_s, scene, ls);

        fglClearColor(gl, 0, 0, 0);
        fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
        emit_viewport();
        fglSetProjection(gl, to_fgl(scene.camera.proj));
        fglSetView(gl, to_fgl(scene.camera.view));
        fglEnable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);

        for (int i = 0; i < scene.item_count && !failed; i++) {
            const DrawItem& it = scene.items[i];
            Shader* item_shader = it.shader_path && it.shader_path[0] ? custom_shader(it.shader_path, scene) :
                                  (it.unlit ? &unlit_s : lit);
            if (!item_shader) { failed = true; break; }
            draw_item(it, *item_shader, scene, &failed);
        }
        if (!failed) {
            // The sky last, at the far plane: only uncovered pixels are shaded.
            fglDisable(gl, FGL_CULL_FACE);
            fglUseShader(gl, sky_s.sh);
            fglSetModel(gl, to_fgl(Mat4::identity()));
            fglDraw(gl, sky_buffer, 0, 3);
            if (submit() != 0) { set_error("FarcadeGL: submit failed", gl->error); failed = true; }
        } else {
            submit();
        }
        }   // !tier_done
        free_temps();
        if (failed) return false;
        double tr = now_ms();
        bool scaled = !banding && (rw != width || rh != height);
        uint32_t* target = banding ? dst : pixels;
        if (scaled) {
            size_t need = (size_t)rw * (size_t)rh;
            if (ipixels_cap < need) {
                free(ipixels);
                ipixels = (uint32_t*)malloc(need * 4);
                ipixels_cap = ipixels ? need : 0;
                if (!ipixels) { set_error("out of memory"); return false; }
            }
            target = ipixels;
        }
        int rr = fglReadPixels(gl, target, (uint32_t)rw * 4);
        stats.read_ms = now_ms() - tr;
        if (rr != 0) {
            set_error("FarcadeGL: readback failed", gl->error);
            return false;
        }
        double ts = now_ms();
        if (banding) {
            draw_particles(scene, dst, band_y0, rows, false);   // particles hook (bands)
            stats.ms = now_ms() - t0;
            return true;
        }
        // Auto exposure: meter the frame just drawn (XRGB), for the next one.
        if (scene.post.auto_exposure) {
            float l2 = AutoExposure::meter(target, rw, rh, false, scene.post.tonemap, pf.exposure_ev);
            float tgt = AutoExposure::target_for(l2, scene.post.auto_exposure_min, scene.post.auto_exposure_max);
            ae.step(tgt, scene.post.exposure_speed, dt);
            stats.meter_log2 = l2;
        } else {
            ae.reset();
        }
        // XRGB (blue in the low byte) to RGBA (red in the low byte), opaque; upscaled from the internal size.
        upscale_xrgb_to_rgba(target, rw, rh, pixels, width, height);
        stats.swizzle_ms = now_ms() - ts;
        draw_particles(scene, pixels, 0, height, true);         // particles hook
        if (scene.screen_shader_count > 0) {
            double tss = now_ms();
            apply_screen_shaders(scene, dt);
            stats.screen_ms = now_ms() - tss;
        }
        stats.ms = now_ms() - t0;
        last_render_end_ms = now_ms();
        if (settings.dynamic_resolution) dyn.update((float)stats.ms, settings.target_frame_ms, settings.min_render_scale, settings.render_scale);
        return true;
    }

    void destroy_gl_objects() {
        if (gl) {
            free_temps();
            forget_all();
            for (Variant& v : variants) { if (v.used) drop(&v.shader); v.used = false; }
            for (int i = 0; i < 2; i++) { drop(&unlit[i]); drop(&sky[i]); }
            for (CustomShaderEntry& e : custom_shaders) { drop(&e.shader); e.path[0] = 0; }
            for (ScreenShader& e : screen_progs) { drop(&e.shader); e = ScreenShader(); }
            if (screen_tex) fglDestroyTexture(gl, screen_tex);
            screen_tex = 0;
            screen_tex_w = screen_tex_h = 0;
            for (TextureEntry& e : textures) {
                if (e.texture && !e.missing) fglDestroyTexture(gl, e.texture);     // missing ones share white_texture
                e = TextureEntry();
            }
            if (white_texture) fglDestroyTexture(gl, white_texture);
            white_texture = 0;
            if (gi_tex) fglDestroyTexture(gl, gi_tex);
            gi_tex = 0;
            gi_started = false;
            gi.reset();
            if (sky_buffer) fglDestroyBuffer(gl, sky_buffer);
            sky_buffer = 0;
            tier.release(gl);
            fglDestroyContext(gl);
            gl = nullptr;
        }
        free(gi_pixels);
        gi_pixels = nullptr;
        free(ipixels);
        ipixels = nullptr;
        ipixels_cap = 0;
        free(screen_buf);
        screen_buf = nullptr;
        screen_cap = 0;
    }
};

Renderer::Renderer() = default;
Renderer::~Renderer() { shutdown(); }

bool Renderer::init(int width, int height) {
    shutdown();
    if (width < 1 || height < 1 || width > 4096 || height > 4096) {
        impl_ = new Impl();
        impl_->settings = settings_;
        impl_->set_error("render size out of range (1..4096)");
        return false;
    }
    impl_ = new Impl();
    impl_->settings = settings_;
    Impl& m = *impl_;
    m.width = width;
    m.height = height;
    if (!fglHasShaders()) { m.set_error("FarcadeGL: the GPU has no shader support"); return false; }
    {
        syscall_gpu_info_t info;
        if (fglGetDeviceInfo(&info) == 0) {
            m.hw = (info.caps & FGPU_CAP_HW_3D) != 0;
            m.caps = DeviceCaps::from_abi(info.caps, info.backend);
            snprintf(m.backend, sizeof m.backend, "%s", info.backend);
        }
    }
    m.scratch = (float*)malloc((size_t)kUploadSlice * kVertexBytes);
    m.pixels = (uint32_t*)calloc((size_t)width * (size_t)height, 4);
    if (!m.scratch || !m.pixels) { m.set_error("out of memory"); return false; }
    double t0 = now_ms();
    bool ok = m.make_context(width, height) && m.make_shaders();
    m.init_ms = now_ms() - t0;
    return ok;
}

double Renderer::init_ms() const { return impl_ ? impl_->init_ms : 0; }
double Renderer::init_shader_ms() const { return impl_ ? impl_->init_shader_ms : 0; }

void Renderer::shutdown() {
    if (!impl_) return;
    impl_->destroy_gl_objects();
    free(impl_->scratch);
    free(impl_->occluders);
    free(impl_->pixels);
    delete impl_;
    impl_ = nullptr;
}

bool Renderer::ready() const { return impl_ && impl_->gl && impl_->sky_buffer; }

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
    if (m.gl) { m.tier.release(m.gl); fglDestroyContext(m.gl); }
    m.gl = nullptr;
    free(m.pixels);
    m.pixels = px;
    m.width = width;
    m.height = height;
    return m.make_context(width, height);
}

bool Renderer::render_rows(const RenderScene& scene, int y0, int rows, uint32_t* dst) {
    if (!ready()) {
        if (impl_ && !impl_->error[0]) impl_->set_error("the renderer is not initialised");
        return false;
    }
    if (!dst || rows < 1 || y0 < 0 || y0 + rows > impl_->height) {
        impl_->set_error("render_rows: rows outside the image");
        return false;
    }
    CompileGate& g = impl_->gate;
    g.deferred = false;
    g.frame_compiles = 0;
    g.deadline = now_ms() + g.budget_ms;
    impl_->tier.set_gate(&g);
    return impl_->render(scene, y0, rows, dst);
}

bool Renderer::render(const RenderScene& scene) {
    if (!ready()) {
        if (impl_ && !impl_->error[0]) impl_->set_error("the renderer is not initialised");
        return false;
    }
    CompileGate& g = impl_->gate;
    g.deferred = false;
    g.frame_compiles = 0;
    g.deadline = now_ms() + g.budget_ms;
    impl_->tier.set_gate(&g);
    return impl_->render(scene);
}

ShaderCacheInfo shader_cache_info() {
    fgl_shader_cache_stats_t s;
    fglShaderCacheStats(&s);
    ShaderCacheInfo i;
    i.enabled = s.enabled != 0;
    i.entries = s.entries;
    i.hits = s.hits;
    i.misses = s.misses;
    i.bytes = s.bytes;
    i.limit = s.limit;
    return i;
}
void shader_cache_clear() { fglShaderCacheClear(); }

void Renderer::set_compile_budget(double budget_ms) {
    if (!impl_) return;
    impl_->gate.budget_ms = budget_ms > 0 ? budget_ms : 0;
    impl_->gate.total = 0;
    impl_->gate.deferred = false;
}
bool Renderer::compiling() const { return impl_ && impl_->gate.deferred; }
int Renderer::shaders_compiled() const { return impl_ ? impl_->gate.total : 0; }
bool Renderer::uses_gpu_tier() const { return impl_ && impl_->tier_ran; }

const uint32_t* Renderer::pixels() const { return impl_ ? impl_->pixels : nullptr; }
int Renderer::width() const { return impl_ ? impl_->width : 0; }
int Renderer::height() const { return impl_ ? impl_->height : 0; }
const char* Renderer::error() const { return impl_ ? impl_->error : "the renderer is not initialised"; }

const char* Renderer::take_warning() {
    static char out[256];
    if (!impl_ || !impl_->warning[0]) return nullptr;
    snprintf(out, sizeof out, "%s", impl_->warning);
    impl_->warning[0] = 0;
    return out;
}

const RendererStats& Renderer::stats() const {
    static const RendererStats none;
    return impl_ ? impl_->stats : none;
}

// ---- quality settings (graphics_settings.h)

void Renderer::set_settings(const GraphicsSettings& s) {
    settings_ = s;
    if (impl_) impl_->settings = s;
}

const GraphicsSettings& Renderer::settings() const { return settings_; }

GraphicsFeatures Renderer::features() const {
    if (impl_) return impl_->feature_set();
    return make_features(DeviceCaps(), settings_);      // before init: no backend known
}

const char* Renderer::backend() const { return impl_ ? impl_->backend : ""; }
const DeviceCaps& Renderer::caps() const {
    static const DeviceCaps none;
    return impl_ ? impl_->caps : none;
}

const GpuTierStats& Renderer::gpu_tier_stats() const {
    static const GpuTierStats none;
    return impl_ && impl_->tier_ran ? impl_->tier.stats() : none;   // zeros after a base tier frame
}
int Renderer::gpu_tier_surfaces() const { return impl_ ? impl_->tier.live_surfaces() : 0; }
int Renderer::gpu_tier_programs() const { return impl_ ? impl_->tier.live_programs() : 0; }

void Renderer::forget_meshes() {
    if (impl_ && impl_->gl) impl_->forget_all();
}

}  // namespace fe::gfx
