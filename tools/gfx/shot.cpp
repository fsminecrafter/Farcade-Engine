// fe-gfx-shot <out.png> [width height] [--quality low|high|ultra]: renders a
// demo scene (ground plane, cubes, metal and rough spheres, an emissive item,
// a low sun, a point and a spot light, a gradient sky, fog, ACES) with
// fe::gfx::Renderer on the host FGPU device and writes it as a PNG.
//
// --quality low    the base tier (GraphicsPreset::Low), the default
// --quality high   the GPU tier forced on the CPU backend (preset High)
// --quality ultra  the GPU tier forced on the CPU backend (preset Ultra)
//
// Post stack options (engine/gfx/post_stack.h):
// --tonemap none|reinhard|aces|agx   --exposure EV   --auto-exposure   --grade warm|cool|bleach
// --grain A   --ca A   --bloom-radius R   --scale S (internal resolution)   --frames N (frames to settle, default 2)
#include "gfx/renderer.h"
#include "gfx/meshgen.h"
#include "scene_demo.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    const char* out = nullptr;
    const char* quality = "low";
    int nums[2] = { 800, 530 }, nn = 0;
    const char *tonemap = nullptr, *grade = nullptr;
    float exposure = 0, grain = 0, ca = 0, bloom_radius = -1, scale = 1;
    bool auto_exposure = false;
    int frames = 2;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tonemap") && i + 1 < argc) tonemap = argv[++i];
        else if (!strcmp(argv[i], "--exposure") && i + 1 < argc) exposure = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--auto-exposure")) auto_exposure = true;
        else if (!strcmp(argv[i], "--grade") && i + 1 < argc) grade = argv[++i];
        else if (!strcmp(argv[i], "--grain") && i + 1 < argc) grain = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--ca") && i + 1 < argc) ca = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--bloom-radius") && i + 1 < argc) bloom_radius = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quality") && i + 1 < argc) quality = argv[++i];
        else if (!out) out = argv[i];
        else if (nn < 2) nums[nn++] = atoi(argv[i]);
    }
    if (!out || (nn != 0 && nn != 2)) { fprintf(stderr, "usage: fe-gfx-shot <out.png> [width height] [--quality low|high|ultra]\n"); return 2; }
    int w = nums[0], h = nums[1];
    fe::gfx::GraphicsSettings gs;
    if (!strcmp(quality, "low")) {
        gs.apply_preset(fe::gfx::GraphicsPreset::Low);
    } else if (!strcmp(quality, "high")) {
        gs.apply_preset(fe::gfx::GraphicsPreset::High);
        gs.allow_gpu_effects_on_cpu = true;
    } else if (!strcmp(quality, "ultra")) {
        gs.apply_preset(fe::gfx::GraphicsPreset::Ultra);
        gs.allow_gpu_effects_on_cpu = true;
    } else {
        fprintf(stderr, "unknown quality '%s' (low, high, ultra)\n", quality);
        return 2;
    }
    gs.render_scale = scale;
    fe::gfx::Renderer r;
    r.set_settings(gs);
    if (!r.init(w, h)) { fprintf(stderr, "init: %s\n", r.error()); return 1; }
    fe::gfx::DemoScene demo;
    demo.build((float)w / (float)h);
    // The first frame uploads the meshes and compiles the shaders; time the second.
    fe::PostProcess& post = demo.scene.post;
    if (tonemap) {
        if (!strcmp(tonemap, "none")) post.tonemap = fe::Tonemap::None;
        else if (!strcmp(tonemap, "reinhard")) post.tonemap = fe::Tonemap::Reinhard;
        else if (!strcmp(tonemap, "aces")) post.tonemap = fe::Tonemap::ACES;
        else if (!strcmp(tonemap, "agx")) post.tonemap = fe::Tonemap::AgX;
        else { fprintf(stderr, "unknown tonemap '%s'\n", tonemap); return 2; }
    }
    post.exposure += exposure;
    post.auto_exposure = auto_exposure;
    post.film_grain = grain;
    post.chromatic_aberration = ca;
    if (bloom_radius >= 0) post.bloom_radius = bloom_radius;
    if (grade) {
        if (!strcmp(grade, "warm")) { post.lift = fe::Vec3(0.02f, 0.0f, 0.04f); post.gain = fe::Vec3(1.12f, 1.0f, 0.82f); post.gamma = fe::Vec3(1.05f, 1.0f, 0.95f); }
        else if (!strcmp(grade, "cool")) { post.lift = fe::Vec3(0.0f, 0.02f, 0.06f); post.gain = fe::Vec3(0.88f, 1.0f, 1.15f); post.gamma = fe::Vec3(0.95f, 1.0f, 1.05f); }
        else if (!strcmp(grade, "bleach")) { post.saturation = 0.45f; post.contrast = 1.25f; post.gain = fe::Vec3(1.05f, 1.05f, 1.0f); }
        else { fprintf(stderr, "unknown grade '%s'\n", grade); return 2; }
    }
    for (int i = 0; i < frames; i++) {
        demo.scene.delta_time = 0.5f;
        if (!r.render(demo.scene)) { fprintf(stderr, "render: %s\n", r.error()); return 1; }
        printf("frame %d: %.1f ms, %d draws, %d triangles, %d uploads\n", i, r.stats().ms, r.stats().draws,
               r.stats().triangles, r.stats().uploads);
    }
    fe::gfx::GraphicsFeatures f = r.features();
    printf("quality %s on %s: gpu tier %s (shadows %d, ao %d, bloom %d, aa %d, pbr %d)\n", quality, r.backend(),
           f.gpu_tier ? "on" : "off", f.shadows, f.ambient_occlusion, f.bloom, f.antialiasing, f.pbr);
    const fe::gfx::GpuTierStats& t = r.gpu_tier_stats();
    if (t.passes)
        printf("stages (ms): shadow %.1f%s depth %.1f ssao %.1f blur %.1f lit %.1f bloom %.1f composite %.1f fxaa %.1f; %d passes\n",
               t.shadow_ms, t.shadow_reused ? " (reused)" : "", t.depth_ms, t.ssao_ms, t.blur_ms, t.lit_ms, t.bloom_ms, t.composite_ms,
               t.fxaa_ms, t.passes);
    if (post.auto_exposure) printf("auto exposure: applied %.2f EV (manual %.2f), scene log2 luminance %.2f\n", r.stats().exposure_ev, post.exposure, r.stats().meter_log2);
    if (!stbi_write_png(out, w, h, 4, r.pixels(), w * 4)) { fprintf(stderr, "cannot write %s\n", out); return 1; }
    printf("wrote %s (%dx%d)\n", out, w, h);
    return 0;
}
