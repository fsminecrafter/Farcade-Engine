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
};

struct DrawItem {
    RenderMesh mesh;
    Mat4 model;                         // object to world
    Vec3 color = Vec3(0.8f, 0.8f, 0.8f);  // sRGB albedo
    float roughness = 0.6f;
    float metallic = 0.0f;              // GPU tier (PBR)
    bool unlit = false;
    bool cast_shadows = true;           // GPU tier
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
    const RenderLight* lights = nullptr;
    int light_count = 0;
    // The current sky, sky light and post processing (defaults when the
    // scene has none current).
    Sky sky;
    SkyLight sky_light;
    PostProcess post;
    // Cheaper shading for images that are replaced at once (a camera drag):
    // no highlights, sun disc, contrast, saturation or vignette (shaders.h).
    bool draft = false;
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
};

// Milliseconds per stage of the last frame that ran the GPU tier (0 = the stage
// did not run), for tools and tests (see engine/gfx/passes.h).
struct GpuTierStats {
    double shadow_ms = 0, depth_ms = 0, ssao_ms = 0, blur_ms = 0, lit_ms = 0, bloom_ms = 0, composite_ms = 0, fxaa_ms = 0;
    double local_shadow_ms = 0;             // the point / spot shadow atlas
    int passes = 0;                         // render passes of the frame
    bool shadow_reused = false;             // the shadow map of the last frame was still valid
    int local_shadows = 0;                  // point / spot lights that got an atlas shadow
    bool atlas_reused = false;              // the atlas of the last frame was still valid
    double total_ms() const { return shadow_ms + depth_ms + ssao_ms + blur_ms + lit_ms + bloom_ms + composite_ms + fxaa_ms + local_shadow_ms; }
};

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
    // Drops cached GPU meshes (for example when a model file changed).
    void forget_meshes();

    // Quality settings (graphics_settings.h). The GPU tier runs only when
    // FarcadeGL is on a graphics card, unless the settings force it.
    void set_settings(const GraphicsSettings& s);
    const GraphicsSettings& settings() const;
    // What runs with the current settings and backend.
    GraphicsFeatures features() const;
    // The FarcadeGL backend, from fglGetDeviceInfo ("cpu ...", "nvidia 3D engine").
    const char* backend() const;
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
