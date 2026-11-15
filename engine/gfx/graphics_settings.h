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

#include <stdint.h>

namespace fe::gfx {

enum class GraphicsPreset : uint8_t { Low, Medium, High, Ultra, Custom };

struct GraphicsSettings {
    GraphicsPreset preset = GraphicsPreset::High;

    // ---- base tier (CPU and GPU)
    float render_scale = 1.0f;              // internal resolution, 0.25..1 (upscaled when shown)

    // ---- GPU tier (ignored on the CPU backend unless forced)
    bool shadows = true;                    // directional sun shadows from a shadow map
    int shadow_resolution = 2048;           // 512..4096
    float shadow_distance = 40.0f;          // meters from the camera that get shadows
    bool soft_shadows = true;               // PCF filtering
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
    bool gpu = false;                       // FarcadeGL runs on a graphics card
    bool gpu_tier = false;                  // the GPU tier is on (gpu, or forced)
    bool shadows = false, ambient_occlusion = false, bloom = false, antialiasing = false, pbr = false;
};

inline void GraphicsSettings::apply_preset(GraphicsPreset p) {
    preset = p;
    switch (p) {
        case GraphicsPreset::Low:
            shadows = false; ambient_occlusion = false; bloom = false; antialiasing = false; pbr = false;
            break;
        case GraphicsPreset::Medium:
            shadows = true; shadow_resolution = 1024; soft_shadows = false; shadow_distance = 30;
            ambient_occlusion = false; bloom = true; antialiasing = true; pbr = true;
            break;
        case GraphicsPreset::High:
            shadows = true; shadow_resolution = 2048; soft_shadows = true; shadow_distance = 40;
            ambient_occlusion = true; ao_radius = 0.5f; ao_strength = 1.0f; bloom = true; antialiasing = true; pbr = true;
            break;
        case GraphicsPreset::Ultra:
            shadows = true; shadow_resolution = 4096; soft_shadows = true; shadow_distance = 80;
            ambient_occlusion = true; ao_radius = 0.7f; ao_strength = 1.2f; bloom = true; antialiasing = true; pbr = true;
            break;
        case GraphicsPreset::Custom:
            break;
    }
}

}  // namespace fe::gfx
