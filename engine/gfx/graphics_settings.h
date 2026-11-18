// Graphics quality settings for the FarcadeGL renderer.
//
// Two tiers. The base tier (forward lighting, sky, post processing folded
// into the shaders) runs everywhere, including FarcadeGL's CPU fallback.
// The GPU tier adds effects that need many passes and per-pixel work a CPU
// can't afford: shadow maps, ambient occlusion, bloom, anti-aliasing and
// physically based (GGX) shading. They are switched on only when FarcadeGL
// runs on a graphics card (the kernel's hardware backend:
// FGPU_CAP_HW_3D), unless `allow_gpu_effects_on_cpu` forces them (slow; for
// development, tests and screenshots on machines without a supported card).
#pragma once

#include "gfx/feature_level.h"
#include <stddef.h>
#include <stdint.h>

namespace fe::gfx {

enum class GraphicsPreset : uint8_t { Low, Medium, High, Ultra, Custom };

// Bloom: Off, Simple (the first version: 4 levels, 4 tap filters, fixed radius) or Chain (5 levels, Karis
// average, 13 tap downsamples, the Bloom radius of the Post Processing object).
enum class BloomMode : uint8_t { Simple, Chain };

// Anti-aliasing (with the `antialiasing` switch on): FXAA (one cheap pass, the original), SMAA (edge detection,
// blend weights from an area texture, neighbourhood blending) or TAA (jittered camera, a history buffer,
// reprojection and a neighbourhood clamp; with render_scale < 1 it also upscales). All GPU tier.
enum class AAMode : uint8_t { FXAA, SMAA, TAA };
// Ambient occlusion (with `ambient_occlusion` on): SSAO (the original, 12 taps) or GTAO (half resolution,
// 2 slices of 6 steps, horizon based, with a temporal and spatial denoise).
enum class AOMode : uint8_t { SSAO, GTAO };

// Ambient light: Hemisphere (the sky / ground colours of the Sky Light, free) or Probes (the CPU voxel and
// L1 SH probe GI of gi.h: bounce light and colour bleeding, a few core-ms). Density is the probe spacing
// (2 / 1 / 0.5 m around the camera), Update how much of the grid is refreshed a frame (1/32, 1/16, 1/8).
enum class GiMode : uint8_t { Hemisphere, Probes };
enum class GiDensity : uint8_t { Coarse, Normal, Fine };
enum class GiUpdate : uint8_t { Slow, Normal, Fast };

struct GraphicsSettings {
    GraphicsPreset preset = GraphicsPreset::High;
    // Which feature level to run (feature_level.h). Auto picks from the
    // device caps: the GPU tier on a graphics card, else FL1-ready when the
    // kernel has scissor + mipmaps + 16 texture units, else FL0.
    FeatureLevelMode feature_level = FeatureLevelMode::Auto;

    // ---- base tier (CPU and GPU)
    float render_scale = 1.0f;              // internal resolution, 0.25..1 (upscaled bilinearly when read back)
    // Dynamic resolution: the internal scale follows the frame time, between
    // min_render_scale and render_scale, to hold target_frame_ms (post_stack.h).
    bool dynamic_resolution = false;
    float target_frame_ms = 33.3f;
    float min_render_scale = 0.5f;

    // ---- GPU tier (ignored on the CPU backend unless forced)
    bool shadows = true;                    // directional sun shadows from a shadow map
    int shadow_resolution = 2048;           // 512..4096
    float shadow_distance = 40.0f;          // meters from the camera that get shadows
    // The sun shadow is split into cascades by view distance (1..3): near ones are sharp, far ones cover
    // more. They share one map of shadow_resolution (an atlas: 2 x 2 tiles of half the size when there is
    // more than one), are stable (texel snapped) and are redrawn only when something in them changed.
    int shadow_cascades = 3;
    // Far cascades keep their fit for 2 / 4 frames while the camera stays inside it (less redrawing,
    // moving shadow casters in them lag by up to 3 frames).
    bool cascade_cache = true;
    bool soft_shadows = true;               // PCF filtering (also for point and spot shadows)
    // Point and spot light shadows: a shadow atlas of 4 x 4 tiles. A spot
    // light takes one tile, a point light six (a cube). The lights with
    // Shadows on that are brightest near the camera get them first.
    bool local_shadows = true;
    int local_shadow_lights = 2;            // 1..4 lights with shadows at a time
    int local_shadow_resolution = 1024;     // the atlas: 512..4096 (a tile is a quarter of it)
    bool ambient_occlusion = true;          // screen-space AO
    float ao_radius = 0.5f;                 // meters
    float ao_strength = 1.0f;               // 0..2
    bool bloom = true;                      // uses PostProcess::bloom and bloom_threshold
    BloomMode bloom_mode = BloomMode::Chain;
    bool antialiasing = true;               // see aa_mode
    AAMode aa_mode = AAMode::FXAA;
    AOMode ao_mode = AOMode::SSAO;
    bool ssr = false;                       // screen space reflections (half resolution, roughness cutoff; GPU tier)
    GiMode gi_mode = GiMode::Hemisphere;    // works on every feature level (CPU work, not a pass)
    GiDensity gi_density = GiDensity::Normal;
    GiUpdate gi_update = GiUpdate::Normal;
    bool pbr = true;                        // GGX specular, Fresnel, energy-conserving (else Blinn-Phong)

    // Run the GPU tier even on the CPU backend (slow).
    bool allow_gpu_effects_on_cpu = false;

    // The values of a preset (render_scale and the forcing flag are kept).
    void apply_preset(GraphicsPreset p);
};

// The settings as named properties (graphics_settings.cpp). Every option is an explicit choice (the older,
// cheaper technique stays selectable next to the newer one), listed by name:
//
//   preset            low | medium | high | ultra | custom   (setting it applies the preset; custom is only read)
//   feature_level     auto | fl0 | fl1 | gpu
//   render_scale      0.25 .. 1        dynamic_resolution  bool    target_frame_ms 8..100    min_render_scale 0.25..1
//   shadows           off | single | cascades      shadow_cascades 2..3 (with cascades)
//   shadow_resolution 512 | 1024 | 2048 | 4096     shadow_distance 1..2000 (m)
//   soft_shadows      bool             cascade_cache       bool
//   local_shadows     bool             local_shadow_lights 1..4     local_shadow_resolution 512..4096
//   ao                off | ssao | gtao       ao_radius 0.05..5    ao_strength 0..2
//   aa                off | fxaa | smaa | taa
//   ssr               off | on
//   bloom             off | simple | chain
//   gi                hemisphere | probes     gi_density coarse | normal | fine     gi_update slow | normal | fast
//   pbr               bool             allow_gpu_effects_on_cpu  bool
//
// to_text / from_text store them as `name value` lines (like .fscene): unknown names and invalid values are
// ignored (counted), missing names keep their defaults. Games keep this text in the project and apply it at start.
enum class GraphicsPropKind : uint8_t { Bool, Integer, Number, Choice };

struct GraphicsProperty {
    const char* name;
    GraphicsPropKind kind;
    float min, max;                         // Integer, Number
    const char* const* choices;             // Choice: names, nullptr terminated
    const char* help;
};

int graphics_property_count();
const GraphicsProperty& graphics_property(int index);
int graphics_property_find(const char* name);       // -1 when unknown
// The value as text ("fxaa", "true", "0.5", "2048"). False for an unknown name.
bool graphics_get_text(const GraphicsSettings& s, const char* name, char* out, int cap);
// Sets a property from text (a Choice by name; Bool: true/false/on/off/1/0). On failure returns false and,
// when `err` is not null, says why. Setting any property except `preset` makes the preset Custom.
bool graphics_set_text(GraphicsSettings* s, const char* name, const char* value, char* err = nullptr, int err_cap = 0);
bool graphics_apply_preset_name(GraphicsSettings* s, const char* name);      // "low" ...

// "farcade-graphics 1" and one `name value` line per property. Returns the length (without the NUL, which
// is written when cap > 0); a result >= cap means the text was truncated.
size_t graphics_settings_to_text(const GraphicsSettings& s, char* out, size_t cap);
// Starts from the defaults and applies the lines of `text`. Returns the number of lines ignored (unknown
// name or invalid value); -1 for a null text.
int graphics_settings_from_text(GraphicsSettings* s, const char* text, size_t size);

// Convenience forms for hosts that keep the text in a file or a project: malloc'd text (free() it; *size is the
// length) and a bool load (true when every line was understood).
char* graphics_settings_to_text(const GraphicsSettings& s, size_t* size);
bool graphics_settings_from_text(const char* text, size_t size, GraphicsSettings* out);

// What the renderer actually does this frame (the settings, limited by the
// backend).
struct GraphicsFeatures {
    FeatureLevel level = FeatureLevel::FL0; // the level that runs (feature_level.h)
    bool level_forced = false;              // GpuTier on a backend without a graphics card
    bool level_clamped = false;             // the requested level needs caps the device lacks
    PassCaps pass_caps;                     // optional hardware features the device has
    bool gpu = false;                       // FarcadeGL runs on a graphics card
    bool gpu_tier = false;                  // the GPU tier is on (gpu, or forced)
    bool shadows = false, ambient_occlusion = false, bloom = false, antialiasing = false, pbr = false;
    bool local_shadows = false;             // point and spot light shadows
    bool gi = false;                        // probe GI feeds the ambient light
    AAMode aa_mode = AAMode::FXAA;          // meaningful when antialiasing
    AOMode ao_mode = AOMode::SSAO;          // meaningful when ambient_occlusion
    bool ssr = false;
};

inline void GraphicsSettings::apply_preset(GraphicsPreset p) {
    preset = p;
    switch (p) {
        case GraphicsPreset::Low:
            shadows = false; ambient_occlusion = false; bloom = false; antialiasing = false; pbr = false;
            local_shadows = false; bloom_mode = BloomMode::Simple; shadow_cascades = 1;
            gi_mode = GiMode::Hemisphere; aa_mode = AAMode::FXAA; ao_mode = AOMode::SSAO; ssr = false;
            break;
        case GraphicsPreset::Medium:
            shadows = true; shadow_resolution = 1024; soft_shadows = false; shadow_distance = 30; shadow_cascades = 2;
            ambient_occlusion = false; bloom = true; antialiasing = true; pbr = true;
            local_shadows = false; bloom_mode = BloomMode::Simple;
            gi_mode = GiMode::Hemisphere; aa_mode = AAMode::FXAA; ao_mode = AOMode::SSAO; ssr = false;
            break;
        case GraphicsPreset::High:
            shadows = true; shadow_resolution = 2048; soft_shadows = true; shadow_distance = 40; shadow_cascades = 3;
            ambient_occlusion = true; ao_radius = 0.5f; ao_strength = 1.0f; bloom = true; antialiasing = true; pbr = true;
            local_shadows = true; local_shadow_lights = 2; local_shadow_resolution = 1024;
            gi_mode = GiMode::Probes; gi_density = GiDensity::Normal; gi_update = GiUpdate::Normal;
            aa_mode = AAMode::SMAA; ao_mode = AOMode::SSAO; ssr = false;
            break;
        case GraphicsPreset::Ultra:
            shadows = true; shadow_resolution = 4096; soft_shadows = true; shadow_distance = 80; shadow_cascades = 3;
            ambient_occlusion = true; ao_radius = 0.7f; ao_strength = 1.2f; bloom = true; antialiasing = true; pbr = true;
            local_shadows = true; local_shadow_lights = 4; local_shadow_resolution = 2048;
            gi_mode = GiMode::Probes; gi_density = GiDensity::Normal; gi_update = GiUpdate::Fast;
            aa_mode = AAMode::TAA; ao_mode = AOMode::GTAO; ssr = true;
            break;
        case GraphicsPreset::Custom:
            break;
    }
}

}  // namespace fe::gfx
