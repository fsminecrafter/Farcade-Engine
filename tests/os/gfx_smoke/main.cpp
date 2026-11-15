// gfx_smoke: the FarcadeGL renderer on Minimal-OS.
//
// Renders the smoke scene (scene.h: a lit cube on a ground plane, sun,
// gradient sky, ACES) into an offscreen FarcadeGL context through the
// kernel's FGPU, checks the picture (smoke_check in scene.h, the same checks
// the host test runs) and prints "FE GFX PASS <base> <hq>" (the base tier's checksum, then that of a frame
// with the GPU tier forced on, then of that frame with a point and a spot
// light casting shadows from the atlas) or "FE GFX FAIL
// <why>". The checksum is that of the RGBA pixels; the host test
// (build/host/bin/gfx_tests) prints the checksum of the same scene rendered
// on the host device, which should be the same.
//
// The screen is never touched: the renderer only submits and reads back.
// Run it with: tools/ci/run_os.sh build/os/bin/gfx_smoke.run 'FE GFX PASS' 'FE GFX FAIL'
#include "gfx/renderer.h"
#include "scene.h"

#include <stdio.h>

using namespace fe::gfx;

int main() {
    SmokeScene s;
    s.build();
    Renderer r;
    uint32_t sum = 0;
    const char* why = smoke_check(r, s, &sum);
    if (why) {
        printf("FE GFX FAIL %s (%s)\n", why, r.error());
        return 1;
    }

    // The GPU tier forced on (shadows, AO, bloom, FXAA, PBR, fog): one frame, its checksum.
    GraphicsSettings g;
    g.apply_preset(GraphicsPreset::High);
    g.shadow_resolution = 512;
    g.allow_gpu_effects_on_cpu = true;
    s.scene.post.bloom = 0.4f;
    s.scene.post.bloom_threshold = 0.8f;
    s.scene.sky.fog = 0.2f;
    Renderer h;
    h.set_settings(g);
    if (!h.init(kSmokeWidth, kSmokeHeight) || !h.render(s.scene)) {
        printf("FE GFX FAIL gpu tier (%s)\n", h.error());
        return 1;
    }
    if (!h.features().gpu_tier || h.gpu_tier_stats().passes < 5) {
        printf("FE GFX FAIL gpu tier did not run\n");
        return 1;
    }
    uint32_t hq = checksum(h.pixels(), kSmokeWidth * kSmokeHeight);

    // Point and spot shadows (the atlas), same settings.
    SmokeLocalLights ll;
    ll.apply(s);
    if (!h.render(s.scene) || h.gpu_tier_stats().local_shadows != 2) {
        printf("FE GFX FAIL point/spot shadows (%s, %d lights)\n", h.error(), h.gpu_tier_stats().local_shadows);
        return 1;
    }
    uint32_t local = checksum(h.pixels(), kSmokeWidth * kSmokeHeight);
    printf("FE GFX PASS %08x %08x %08x\n", sum, hq, local);
    return 0;
}
