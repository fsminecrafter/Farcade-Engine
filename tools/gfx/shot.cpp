// fe-gfx-shot <out.png> [width height] [--quality low|high|ultra]: renders a
// demo scene (ground plane, cubes, metal and rough spheres, an emissive item,
// a low sun, a point and a spot light, a gradient sky, fog, ACES) with
// fe::gfx::Renderer on the host FGPU device and writes it as a PNG.
//
// --quality low    the base tier (GraphicsPreset::Low), the default
// --quality high   the GPU tier forced on the CPU backend (preset High)
// --quality ultra  the GPU tier forced on the CPU backend (preset Ultra)
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
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--quality") && i + 1 < argc) quality = argv[++i];
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
    fe::gfx::Renderer r;
    r.set_settings(gs);
    if (!r.init(w, h)) { fprintf(stderr, "init: %s\n", r.error()); return 1; }
    fe::gfx::DemoScene demo;
    demo.build((float)w / (float)h);
    // The first frame uploads the meshes and compiles the shaders; time the second.
    for (int i = 0; i < 2; i++) {
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
    if (!stbi_write_png(out, w, h, 4, r.pixels(), w * 4)) { fprintf(stderr, "cannot write %s\n", out); return 1; }
    printf("wrote %s (%dx%d)\n", out, w, h);
    return 0;
}
