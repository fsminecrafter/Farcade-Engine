// gfx_smoke: the FarcadeGL renderer on Minimal-OS.
//
// Renders the smoke scene (scene.h: a lit cube on a ground plane, sun,
// gradient sky, ACES) into an offscreen FarcadeGL context through the
// kernel's FGPU, checks the picture and prints "FE GFX PASS <checksum>" or
// "FE GFX FAIL <why>". The checksum is that of the RGBA pixels; the host
// test (build/host/bin/gfx_tests) prints the checksum of the same scene
// rendered on the host device, which should be the same.
//
// The screen is never touched: the renderer only submits and reads back.
// Run it with tools/ci/run_os.sh build/os/bin/gfx_smoke.run 'FE GFX PASS' 'FE GFX FAIL'.
#include "gfx/renderer.h"
#include "../../os/gfx_smoke/scene.h"

#include <stdio.h>

using namespace fe;
using namespace fe::gfx;

static int fail(const char* why) {
    printf("FE GFX FAIL %s\n", why);
    return 1;
}

static int luma(uint32_t p) {
    int r = (int)(p & 0xFF), g = (int)((p >> 8) & 0xFF), b = (int)((p >> 16) & 0xFF);
    return (r * 3 + g * 6 + b) / 10;
}

// The pixel a world point projects to.
static uint32_t pixel_at(const RenderScene& s, const Renderer& r, Vec3 p) {
    Vec4 c = s.camera.proj * (s.camera.view * Vec4{ p.x, p.y, p.z, 1 });
    int x = (int)((c.x / c.w * 0.5f + 0.5f) * (float)r.width());
    int y = (int)((1.0f - (c.y / c.w * 0.5f + 0.5f)) * (float)r.height());
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= r.width()) x = r.width() - 1;
    if (y >= r.height()) y = r.height() - 1;
    return r.pixels()[y * r.width() + x];
}

int main() {
    SmokeScene s;
    s.build();
    Renderer r;
    if (!r.init(kSmokeWidth, kSmokeHeight)) {
        char why[256];
        snprintf(why, sizeof why, "init: %s", r.error());
        return fail(why);
    }
    if (!r.render(s.scene)) {
        char why[256];
        snprintf(why, sizeof why, "render: %s", r.error());
        return fail(why);
    }
    if (r.stats().uploads != 2 || r.stats().draws != 2) return fail("first frame: wrong uploads or draws");
    uint32_t sum = checksum(r.pixels(), kSmokeWidth * kSmokeHeight);

    // The sky fills the empty top corner and differs from the cube; the cube's
    // sun-facing top is brighter than the ground in its shadow-free ambient.
    uint32_t sky = r.pixels()[2 * kSmokeWidth + 2];
    uint32_t top = pixel_at(s.scene, r, Vec3(0, 0.5f, 0));
    if ((sky & 0xFFFFFF) == 0 || (top & 0xFFFFFF) == (sky & 0xFFFFFF)) return fail("sky and cube look the same");
    if (luma(top) < 40) return fail("the lit top of the cube is dark");
    int differing = 0;
    for (int i = 0; i < kSmokeWidth * kSmokeHeight; i++)
        if (r.pixels()[i] != sky) differing++;
    if (differing < kSmokeWidth * kSmokeHeight / 8) return fail("picture is almost flat");
    if ((top >> 24) != 0xFF) return fail("alpha is not 255");

    // A second frame reuses the cached meshes and draws the same picture.
    if (!r.render(s.scene)) return fail("second render failed");
    if (r.stats().uploads != 0) return fail("second frame uploaded meshes again");
    if (checksum(r.pixels(), kSmokeWidth * kSmokeHeight) != sum) return fail("second frame differs");

    // Resize keeps the meshes.
    if (!r.resize(kSmokeWidth / 2, kSmokeHeight / 2)) return fail("resize failed");
    s.scene.camera.proj = Mat4::perspective(radians(55.0f), (float)kSmokeWidth / (float)kSmokeHeight, 0.1f, 100.0f);
    if (!r.render(s.scene) || r.stats().uploads != 0) return fail("render after resize");

    printf("FE GFX PASS %08x\n", sum);
    return 0;
}
