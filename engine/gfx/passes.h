// The GPU tier of the renderer: shadow maps, SSAO, bloom, FXAA, PBR lighting
// and fog, as separate passes over offscreen FarcadeGL color targets. See
// docs/graphics.md for the pipeline, the encodings and the costs.
//
// Internal to engine/gfx: renderer.cpp owns one GpuTier and calls it from a
// single place in Impl::render (it replaces the base tier's main pass when
// GpuTier::plan() says something is on). The tier draws meshes through
// PassHost, so the mesh cache stays in renderer.cpp.
#pragma once

#include "gfx/renderer.h"
#include "gfx/shaders_hq.h"
#include "gfx/post_stack.h"

#if defined(FE_HOST)
#include "gfx/host/farcadegl_host.h"
#else
#include "farcadegl.h"
#endif

namespace fe::gfx {

constexpr int kMaxLocalShadows = 4;         // point / spot lights with shadows at a time
constexpr int kAtlasTiles = 4;              // the shadow atlas is kAtlasTiles x kAtlasTiles tiles

// The lights of a frame, packed for the lit shaders (renderer.cpp's LightSet).
struct PackedLights {
    const float* l0 = nullptr;              // xyz position, w range
    const float* l1 = nullptr;              // xyz direction the light travels
    const float* l2 = nullptr;              // rgb color * intensity
    const float* l3 = nullptr;              // x cos outer, y cos inner
    int nd = 0, np = 0, ns = 0;             // directional, point, spot: the order of the arrays
    bool has_sun = false;
    Vec3 sun_dir;                           // towards the sun
    Vec3 sun_color;
    // Point and spot lights with an atlas shadow: the first nps point lights
    // and the first nss spot lights of the packed order; `local` holds them
    // in that order (points, then spots).
    int nps = 0, nss = 0;
    const RenderLight* local[kMaxLocalShadows] = {};
};

// What the tier needs from the renderer.
struct PassHost {
    void* user = nullptr;
    // Binds the mesh of `it` and draws it with the shader and uniforms already
    // set (sets the model matrix, handles mirroring and the command budget).
    // `count` adds it to the frame's draw statistics (the main pass only).
    bool (*draw_mesh)(void* user, const DrawItem& it, bool count, bool* failed) = nullptr;
    // A viewport that draw_mesh sets again after its own submits (a submit
    // resets it); w == 0 turns it off.
    void (*set_viewport)(void* user, int x, int y, int w, int h) = nullptr;
    // Probe GI (gi.h): the probe texture and the window uniforms (u_gi0, u_gi1) when it is on.
    bool gi_on = false;
    FGLTexture gi_texture = 0;
    float gi0[4] = {}, gi1[4] = {};
};

// What runs this frame: the features, limited by what the scene has.
struct GpuPlan {
    bool shadows = false, ao = false, bloom = false, aa = false, pbr = false, fog = false;
    bool local_shadows = false;             // a point or spot light with Shadows on
    bool hw = false;                        // FarcadeGL runs on a graphics card
    bool any() const { return shadows || ao || bloom || aa || pbr || fog || local_shadows; }
};

class GpuTier {
public:
    GpuTier();
    ~GpuTier();
    GpuTier(const GpuTier&) = delete;
    GpuTier& operator=(const GpuTier&) = delete;

    // Which stages run for this scene.
    static GpuPlan plan(const RenderScene& scene, const GraphicsFeatures& f);

    // Draws the frame into the context's color surface (`gl->color`), leaving
    // the context bound to it. False on a FarcadeGL error (`error` says which).
    bool render(FGLContext* gl, const RenderScene& scene, const PackedLights& lights, const GraphicsSettings& settings,
                const GpuPlan& plan, const PassHost& host, const PostFrame& post, char* error, int error_cap);

    // Frees every GL object (targets, shaders); call before the context goes.
    void release(FGLContext* gl);

    const GpuTierStats& stats() const;
    // Live FarcadeGL surfaces and programs the tier holds (tests: no leaks).
    int live_surfaces() const;
    int live_programs() const;

    struct Impl;
private:
    Impl* impl_;
};

}  // namespace fe::gfx
