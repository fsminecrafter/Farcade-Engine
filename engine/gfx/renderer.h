// The FarcadeGL renderer: draws a RenderScene (a camera, meshes, lights, a
// sky, ambient light, post processing) through FarcadeGL into an offscreen
// color buffer and hands back the pixels.
//
// The editor fills a RenderScene from its Scene and shows the pixels in the
// viewport (FarcadeGL renders, fglReadPixels reads back, ImGui shows the
// image); the runtime fills one from the ECS world. On Minimal-OS FarcadeGL
// runs on the kernel's FGPU; on the host build the same farcadegl.h calls
// run on a host copy of the FGPU CPU backend (engine/gfx/host), so the
// renderer is tested on the PC.
//
// This header has no FarcadeGL types in it: callers don't include
// farcadegl.h.
#pragma once

#include "core/math.h"
#include "scene/modules.h"
#include "gfx/graphics_settings.h"
#include "gfx/particles.h"
#include <stddef.h>
#include <stdint.h>

namespace fe::gfx {

// A triangle list in object space. `key` identifies the mesh data so the
// renderer uploads it once and reuses the GPU buffer: the same key (with
// the same vertex_count) must mean the same vertices. 0 = upload every frame.
struct RenderMesh {
    const Vec3* positions = nullptr;
    const Vec3* normals = nullptr;
    uint32_t vertex_count = 0;
    uint64_t key = 0;
    const float* uvs = nullptr;         // optional, 2 floats per vertex (else a box projection is used)
    // Optional object space box (else it is computed from the vertices, cached by key): culling, shadows.
    bool has_bounds = false;
    Vec3 bounds_min = Vec3(), bounds_max = Vec3();
    // Draw only the first draw_count vertices (0 = all; rounded down to whole triangles). The whole mesh is
    // still uploaded once per key: foliage draws fewer of a chunk's instances as it gets further away.
    uint32_t draw_count = 0;
    // Vertices per instance of such a batch (its instances in random order): shadow maps draw a shorter
    // prefix of it (GraphicsSettings::foliage_shadow_density). 0 = not a batch.
    uint32_t instance_vertices = 0;
    // Optional triangles for occlusion culling (object space) that never stick out of the real surface,
    // e.g. a terrain chunk's coarse mesh. Without them small meshes occlude with their own triangles.
    const Vec3* occluder_positions = nullptr;
    uint32_t occluder_count = 0;
};

struct DrawItem {
    RenderMesh mesh;
    Mat4 model;                         // object to world
    const char* texture_path = nullptr;
    const char* shader_path = nullptr;
    Vec3 color = Vec3(0.8f, 0.8f, 0.8f);  // sRGB albedo
    float roughness = 0.6f;
    float metallic = 0.0f;              // GPU tier (PBR)
    bool unlit = false;
    bool cast_shadows = true;           // GPU tier
    bool occluder = true;               // may hide other items from the camera (occlusion culling)
    bool foliage = false;               // grass and the like: never an occluder, left out of the probe GI
    // Leaf cards and the like: pixels where the texture's alpha is below alpha_cut are not drawn (also in
    // shadow maps and depth); 0 = opaque. double_sided: no back face culling, back faces lit from their side.
    float alpha_cut = 0.0f;
    bool double_sided = false;
};

// A light in world space (filled from a Light module and its object).
struct RenderLight {
    LightType type = LightType::Point;
    Vec3 position;                      // Point, Spot
    Vec3 direction = Vec3(0, -1, 0);    // Directional, Spot: where the light goes (normalized)
    Vec3 color = Vec3(1, 1, 1);         // linear
    float intensity = 1.0f;
    float range = 10.0f;
    float spot_angle = 45.0f;           // full cone, degrees
    float spot_blend = 0.15f;
    bool shadows = true;
};

struct RenderCamera {
    Mat4 view;                          // world to view (engine Mat4 conventions, core/math.h)
    Mat4 proj;                          // view to clip (Mat4::perspective / ortho)
    Vec3 position;
    float near_plane = 0.1f, far_plane = 500.0f;
};

struct RenderScene {
    RenderCamera camera;
    const DrawItem* items = nullptr;
    int item_count = 0;
    // Set by the renderer, not by callers: the items the camera passes draw are the ones that survived
    // culling (items); shadows and GI use every item (shadow_items, or items when null).
    const DrawItem* shadow_items = nullptr;
    int shadow_item_count = 0;
    const RenderLight* lights = nullptr;
    int light_count = 0;
    // The current sky, sky light and post processing (defaults when the
    // scene has none current).
    Sky sky;
    SkyLight sky_light;
    PostProcess post;
    // The current Clouds object's settings (clouds_on false: no clouds).
    bool clouds_on = false;
    Clouds clouds;
    // Optional project asset reader. It returns malloc-compatible file bytes
    // (or null); the renderer releases them with free(). Required when a draw
    // item references a texture or custom shader.
    void* (*read_asset)(const char* path, size_t* size, void* user) = nullptr;
    void* asset_user = nullptr;
    // Cheaper shading for images that are replaced at once (a camera drag):
    // no highlights, sun disc, contrast, saturation or vignette (shaders.h).
    bool draft = false;
    // For views drawn only when something changes (the editor): TAA forgets its history whenever the camera
    // or an item moved, and averages the still frames that follow evenly (1/n) instead of keeping 90% of an
    // older view, so the caller can draw a few more frames to let the picture settle (Renderer::settling()).
    bool accumulate_still = false;
    // Particles and sprites drawn over the finished image (particles.h). The
    // items are only read during render().
    // Screen shaders (the current Camera's Shader 1..3): full screen passes over the finished image, in
    // order. A file is the fragment part only (or a full .fshader with // @vertex and // @fragment). They
    // get u_screen (the image so far; sample it at `uv`), u_res (w, h, 1/w, 1/h), u_time (seconds, delta,
    // frame, 0) and u_params (the camera's Shader params). Read through read_asset; not drawn in bands.
    const char* screen_shaders[4] = {};
    int screen_shader_count = 0;
    float screen_params[4] = {};
    const ParticleBatch* particles = nullptr;
    int particle_batch_count = 0;
    const TextLabel* labels = nullptr;      // Text 3D labels (particles.h)
    int label_count = 0;
    bool particles_depth_test = true;   // meshes hide particles (false: cheaper, no depth map)
    // Seconds since the last frame, for the auto exposure's adaptation (and
    // other time based effects). Negative: measured with the wall clock.
    float delta_time = -1.0f;
};

struct RendererStats {
    int draws = 0;
    int triangles = 0;
    int uploads = 0;                    // mesh buffers uploaded this frame
    double ms = 0;                      // render + readback, wall clock
    // Where the time of the last render() went (ms): compiling shaders,
    // uploading meshes, the GPU run (fglSubmit), the readback syscall, the
    // CPU pass that turns XRGB into RGBA. The rest is recording commands.
    double shader_ms = 0, upload_ms = 0, submit_ms = 0, read_ms = 0, swizzle_ms = 0;
    int shaders_compiled = 0;
    // The post stack (post_stack.h): the internal resolution that was drawn,
    // the exposure that was applied (manual + automatic, EV) and what the
    // meter saw (log2 of the scene's average luminance, auto exposure on).
    float render_scale = 1.0f;
    bool taa_upscale = false;               // TAA drew at the internal size and resolved to the full size
    int internal_width = 0, internal_height = 0;
    float exposure_ev = 0, meter_log2 = 0;
    double gi_ms = 0;                       // the probe GI's CPU update (gi.h)
    double screen_ms = 0;                   // the camera's screen shaders (upload, passes, readback)
    int screen_passes = 0;
    // Culling (culling.h): items given, left out by the view frustum and by occluders, the occluders drawn
    // into the CPU depth buffer (and their triangles), and the time it took.
    int items_in = 0, culled_frustum = 0, culled_occlusion = 0, occluders = 0, occluder_triangles = 0;
    double cull_ms = 0;
    int cloud_quality = 0;                  // clouds drawn: 0 none, 1 low, 2 medium, 3 ultra (after the caps)
};

// Milliseconds per stage of the last frame that ran the GPU tier (0 = the stage
// did not run), for tools and tests (see engine/gfx/passes.h).
struct GpuTierStats {
    double shadow_ms = 0, depth_ms = 0, ssao_ms = 0, blur_ms = 0, lit_ms = 0, bloom_ms = 0, composite_ms = 0, fxaa_ms = 0;
    double local_shadow_ms = 0;             // the point / spot shadow atlas
    double ssr_ms = 0, taa_ms = 0, smaa_ms = 0;
    int passes = 0;                         // render passes of the frame
    bool shadow_reused = false;             // every sun cascade of the last frame was still valid
    int cascades = 0, cascades_drawn = 0, cascades_reused = 0;   // the sun's cascades and what the frame did with them
    // Cascades whose static casters came from their cached copy, with only the moving (displaced) casters drawn.
    int cascades_dynamic = 0;
    float cascade_texel[3] = {};            // meters per shadow texel of each cascade
    int local_shadows = 0;                  // point / spot lights that got an atlas shadow
    bool atlas_reused = false;              // the atlas of the last frame was still valid
    double total_ms() const { return shadow_ms + depth_ms + ssao_ms + blur_ms + lit_ms + bloom_ms + composite_ms + fxaa_ms + local_shadow_ms + ssr_ms + taa_ms + smaa_ms; }
};

// FarcadeGL's shader cache (0:/cache/shaders, the SDK's fglcache.h), shared
// by every program on the system. Off on the host unless a directory is set.
struct ShaderCacheInfo {
    bool enabled = false;
    uint32_t entries = 0, hits = 0, misses = 0;
    uint64_t bytes = 0, limit = 0;
};
ShaderCacheInfo shader_cache_info();
void shader_cache_clear();

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Creates the FarcadeGL context and shaders (offscreen, width x height).
    // False when FarcadeGL is not available; error() says why.
    bool init(int width, int height);
    void shutdown();
    bool ready() const;
    // Changes the size (recreates the targets; cached meshes stay).
    bool resize(int width, int height);

    // Draws the scene and reads the result back. Returns false on a
    // FarcadeGL error (error() says which).
    bool render(const RenderScene& scene);

    // Draws only rows [y0, y0 + rows) of the image (the same picture render()
    // gives, up to rasterizer rounding at triangle edges) and writes them to
    // dst as XRGB words (0x00RRGGBB, width() * rows of them), not to pixels().
    // The cost is that of rows * width pixels: a full-size image can be drawn
    // in bands, with the caller doing other work between them. The surfaces
    // are recreated when `rows` changes.
    bool render_rows(const RenderScene& scene, int y0, int rows, uint32_t* dst);

    // RGBA8 (R in the low byte, alpha 255), width() * height(), rows top to
    // bottom; valid until the next render() or resize().
    const uint32_t* pixels() const;
    int width() const;
    int height() const;
    const RendererStats& stats() const;
    // init(): context + shader creation time, and the shader part of it (ms).
    double init_ms() const;
    double init_shader_ms() const;
    const char* error() const;
    // A problem that did not stop the frame (a texture that is missing or not
    // an image draws plain white); returns it once, then null.
    const char* take_warning();
    // Drops cached GPU meshes (for example when a model file changed).
    void forget_meshes();

    // Shader compiling spread over frames, for a loading bar: with a budget,
    // a render() that has compiled shaders for `budget_ms` stops before the
    // next one it needs and returns false with compiling() true; nothing is
    // drawn, the shaders made so far are kept, and the next render() goes
    // on. 0 (the default) compiles everything a frame needs at once.
    void set_compile_budget(double budget_ms);
    bool compiling() const;
    // Shaders compiled since the budget was set (reset by set_compile_budget).
    int shaders_compiled() const;
    // The last frame (also one that stopped to compile) used the GPU tier.
    bool uses_gpu_tier() const;

    // Quality settings (graphics_settings.h). The GPU tier runs only when
    // FarcadeGL is on a graphics card, unless the settings force it.
    void set_settings(const GraphicsSettings& s);
    const GraphicsSettings& settings() const;
    // What runs with the current settings and backend.
    GraphicsFeatures features() const;
    // The FarcadeGL backend, from fglGetDeviceInfo ("cpu ...", "nvidia 3D engine").
    const char* backend() const;
    // The device caps and the feature level that runs (feature_level.h):
    // features().level, or ask directly.
    const DeviceCaps& caps() const;
    FeatureLevel feature_level() const { return features().level; }
    // Stage timings of the last frame, and the render targets and shader
    // programs the GPU tier holds right now (they are freed on resize and shutdown).
    const GpuTierStats& gpu_tier_stats() const;
    int gpu_tier_surfaces() const;
    int gpu_tier_programs() const;

    struct Impl;
private:
    Impl* impl_ = nullptr;
    GraphicsSettings settings_;             // survives init / shutdown
};

}  // namespace fe::gfx
