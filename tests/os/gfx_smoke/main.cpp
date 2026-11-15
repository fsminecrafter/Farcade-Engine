// gfx_smoke: the FarcadeGL renderer on Minimal-OS.
//
// Renders the smoke scene (scene.h: a lit cube on a ground plane, sun,
// gradient sky, ACES) into an offscreen FarcadeGL context through the
// kernel's FGPU, checks the picture (smoke_check in scene.h, the same checks
// the host test runs) and prints "FE GFX PASS <checksum>" or "FE GFX FAIL
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
    printf("FE GFX PASS %08x\n", sum);
    return 0;
}
