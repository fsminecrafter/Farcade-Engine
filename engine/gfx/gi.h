// FL0 hybrid global illumination, CPU side (plan C4.5 / E6): a camera centred voxel grid built from the
// scene's meshes, lit by the sun and the local lights (with the probes of the last frame as the bounce),
// and a grid of L1 spherical harmonics irradiance probes that shoot rays through the voxels and blend the
// result over time. The probes are packed into an ARGB8 texture the lit shaders sample (4 coefficient tiles,
// 8 taps) in place of the hemisphere ambient. No FarcadeGL here: the host tests run it directly.
//
// Budget: voxelizing runs when the scene changes (at most every 100 ms), light injection touches 1/4 of
// the surface voxels a frame, the probe update a fraction of the probes (settings: density, update rate).
// Typically 1 to 4 ms of one core for the demo scene (docs/graphics.md has the numbers).
//
// Limits (documented, not hidden): one voxel level of 48^3 at 0.5 m (24 m around the camera; it is
// rebuilt when the camera leaves its middle third, not scrolled toroidally), no shadows from local
// lights, probes are toroidal (a probe keeps its world cell while the window slides).
#pragma once

#include "gfx/renderer.h"
#include <stdint.h>

namespace fe::gfx {

// GiMode, GiDensity and GiUpdate are in graphics_settings.h.

struct GiParams {
    GiDensity density = GiDensity::Normal;
    GiUpdate update = GiUpdate::Normal;
    float hysteresis = 0.93f;               // weight of the old value of a probe
    int rays = 32;                          // per probe update
    float bounce = 0.8f;                    // how much of the last frame's irradiance a surface re-emits
    float intensity = 1.0f;                 // scales what the shaders read
};

// What the GI needs of a frame.
struct GiInput {
    const DrawItem* items = nullptr;
    int item_count = 0;
    const RenderLight* lights = nullptr;
    int light_count = 0;
    Vec3 sky_color, ground_color;           // the hemisphere ambient (intensity applied): what a ray that leaves sees
    Vec3 camera;
    float dt = 1.0f / 60.0f;
};

struct GiStats {
    int voxels = 0;                         // occupied
    int probes_updated = 0;                 // this frame
    int probes_invalid = 0;                 // inside geometry / seeing back faces, using the fallback
    bool revoxelized = false;
    bool settled = false;                   // converged and nothing changed: this frame updated nothing
    double ms = 0;                          // the whole update
};

class GiSystem {
public:
    static constexpr int kVoxelDim = 48;
    static constexpr float kVoxelSize = 0.5f;
    static constexpr int kProbesX = 16, kProbesY = 8, kProbesZ = 16;
    static constexpr int kProbeCount = kProbesX * kProbesY * kProbesZ;
    static constexpr int kTexW = 4 * kProbesX, kTexH = kProbesY * kProbesZ;

    GiSystem();
    ~GiSystem();
    GiSystem(const GiSystem&) = delete;
    GiSystem& operator=(const GiSystem&) = delete;

    // One frame: re-voxelize when the scene changed, inject light into a slice of the surface voxels, update
    // a fraction of the probes. `force_all`: do everything now (tests, the first frame of a screenshot).
    void update(const GiInput& in, const GiParams& p, bool force_all = false);
    void reset();                           // forget everything (the scene changed completely)

    // The probe texture (kTexW x kTexH, XRGB words) of the current window, and what the shaders need:
    // u_gi0 = (window origin xyz, 1 / spacing), u_gi1 = (probes x, y, z, intensity).
    void encode(uint32_t* out) const;
    void uniforms(float gi0[4], float gi1[4]) const;
    uint64_t version() const;               // changes whenever a probe was updated (re-upload then)

    // The ambient colour the probes give at a point for a normal (CPU twin of the shader), for tests and
    // for the bounce.
    Vec3 ambient(const Vec3& pos, const Vec3& n) const;
    bool occupied(const Vec3& pos) const;   // is there geometry in the voxel at a world position
    const GiStats& stats() const;
    float spacing() const;

    struct Impl;
private:
    Impl* impl_;
};

// The spherical harmonics basis the probes use (exposed for tests): Y00 and Y1m.
constexpr float kShY0 = 0.282095f, kShY1 = 0.488603f;

}  // namespace fe::gfx
