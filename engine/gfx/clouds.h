// Volumetric clouds (the Clouds object of scene/modules.h), internal to engine/gfx.
//
// They are ray marched in the sky shader of both tiers, on the sky's pixels only (geometry hides them):
// for each layer the view ray's stretch inside the layer's slab is sampled a fixed number of times
// (FGSL unrolls loops; the step counts are chosen to fit its 2048 instruction limit). At each sample the
// density comes from a tileable 3D noise (a 32^3 atlas texture, two lookups and a lerp between slices)
// shaped by a height profile (flat bottoms, rounded tops), a 2D coverage map that drifts with the wind and
// the coverage setting; Medium and Ultra erode the edges with a finer noise. Light: a few samples towards
// the sun give the optical depth (Beer-Lambert, plus a "powder" term that darkens thin edges facing the
// sun), a Henyey-Greenstein phase brightens the clouds around the sun (sun glow), and the Sky Light's sky
// colour lights them from above and around (darker at the bottom). Samples fade out with distance so the
// layer melts into the horizon. The result is blended over the sky and the sun disc: col * T + clouds.
//
// Quality (the object's preset, capped by GraphicsSettings::clouds):
//   Low     10 samples along the ray, 1 towards the sun, no detail noise
//   Medium  12 samples, 2 towards the sun, detail noise
//   Ultra   16 samples, 3 towards the sun, detail noise, a second shape octave
// The samples are split between the layers (at least 5 each). A start offset jittered per pixel hides
// banding (TAA averages it away). When a variant does not compile (too many instructions on a backend)
// the renderer steps down to the next lower one.
#pragma once

#include "core/math.h"
#include "scene/modules.h"
#include <stddef.h>
#include <stdint.h>

namespace fe::gfx {

// What gets compiled in: 0 = no clouds, else quality (1..3) and layer count (1..3).
struct CloudKey {
    uint8_t quality = 0;                    // 0 off, 1 low, 2 medium, 3 ultra
    uint8_t layers = 1;
    bool operator==(const CloudKey& o) const { return quality == o.quality && layers == o.layers; }
    bool operator!=(const CloudKey& o) const { return !(*this == o); }
    int index() const { return quality ? (quality - 1) * 3 + (layers - 1) + 1 : 0; }    // 0..9
};

// The uniforms of a frame (see clouds_shader for their meaning).
struct CloudFrame {
    CloudKey key;
    float cl0[4] = {}, cl1[4] = {}, cl2[4] = {}, cl3[4] = {}, cpos[4] = {}, csky[4] = {}, csun[4] = {};
};

// Fills the frame's uniforms from the settings. `time` is seconds of wind drift, `sun_dir` points towards
// the sun (sun_color zero when there is none), sky_ambient is the Sky Light's sky colour * intensity.
void clouds_frame(const Clouds& c, CloudKey key, const Vec3& camera, float time, const Vec3& sun_dir, const Vec3& sun_color,
                  const Vec3& sky_ambient, uint32_t frame, CloudFrame* out);

// The FGSL of the clouds for a sky fragment shader. part 0, before main(): the uniforms u_cl0..u_cl3,
// u_cpos, u_csky, u_csun, the samplers u_cnoise and u_cweather and helper functions (the shader must declare
// `uniform vec4 u_sun;`, xyz towards the sun, before them). part 1, a block inside main() after the sky and
// the sun disc: reads `vec3 d` (the view direction) and the varying v_ndc, and blends the clouds over
// `vec3 col` (col * transmittance + their light). Returns the length written.
size_t clouds_shader(char* out, size_t cap, CloudKey key, int part);

// The noise textures (XRGB words, malloc'd): the 3D atlas (8 x 4 slices of 32 x 32 with a 1 texel border:
// 272 x 136; r shape, g detail, b a finer shape octave) and the 128 x 128 coverage map (r, g).
uint32_t* clouds_noise_atlas(int* w, int* h);
uint32_t* clouds_weather_map(int* w, int* h);

}  // namespace fe::gfx
