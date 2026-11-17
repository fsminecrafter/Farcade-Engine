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
#include <stdint.h>

namespace fe::gfx {

enum class GraphicsPreset : uint8_t { Low, Medium, High, Ultra, Custom };

struct GraphicsSettings {
    GraphicsPreset preset = GraphicsPreset::High;
    // Which feature level to run (feature_level.h). Auto picks from the
    // device caps: the GPU tier on a graphics card, else FL1-ready when the
    // kernel has scissor + mipmaps + 16 texture units, else FL0.
    FeatureLevelMode feature_level = FeatureLevelMode::Auto;

    // ---- base tier (CPU and GPU)
    float render_scale = 1.0f;              // internal resolution, 0.25..1 (upscaled when shown)

    // ---- GPU tier (ignored on the CPU backend unless forced)
    bool shadows = true;                    // directional sun shadows from a shadow map
    int shadow_resolution = 2048;           // 512..4096
    float shadow_distance = 40.0f;          // meters from the camera that get shadows
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
    bool antialiasing = true;               // FXAA
    bool pbr = true;                        // GGX specular, Fresnel, energy-conserving (else Blinn-Phong)

    // Run the GPU tier even on the CPU backend (slow).
    bool allow_gpu_effects_on_cpu = false;

    // The values of a preset (render_scale and the forcing flag are kept).
    void apply_preset(GraphicsPreset p);
};

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
};

inline void GraphicsSettings::apply_preset(GraphicsPreset p) {
    preset = p;
    switch (p) {
        case GraphicsPreset::Low:
            shadows = false; ambient_occlusion = false; bloom = false; antialiasing = false; pbr = false;
            local_shadows = false;
            break;
        case GraphicsPreset::Medium:
            shadows = true; shadow_resolution = 1024; soft_shadows = false; shadow_distance = 30;
            ambient_occlusion = false; bloom = true; antialiasing = true; pbr = true;
            local_shadows = false;
            break;
        case GraphicsPreset::High:
            shadows = true; shadow_resolution = 2048; soft_shadows = true; shadow_distance = 40;
            ambient_occlusion = true; ao_radius = 0.5f; ao_strength = 1.0f; bloom = true; antialiasing = true; pbr = true;
            local_shadows = true; local_shadow_lights = 2; local_shadow_resolution = 1024;
            break;
        case GraphicsPreset::Ultra:
            shadows = true; shadow_resolution = 4096; soft_shadows = true; shadow_distance = 80;
            ambient_occlusion = true; ao_radius = 0.7f; ao_strength = 1.2f; bloom = true; antialiasing = true; pbr = true;
            local_shadows = true; local_shadow_lights = 4; local_shadow_resolution = 2048;
            break;
        case GraphicsPreset::Custom:
            break;
    }
}

}  // namespace fe::gfx
