// fe-editor-bench: the editor's benchmark phases (bench.h) on the host,
// headless, for comparing with Minimal-OS (host only).
//
//   fe-editor-bench [--bench N] [--farcadegl] [--adaptive] [--scales] [--size WxH]
//
// Default: --bench 30, 1280x800.
#include "bench.h"
#include "editor.h"
#include "platform/headless.h"

#include "imgui/imgui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace fe;

int main(int argc, char** argv) {
    Bench bench;
    bench.frames_per_phase = 30;
    bool farcadegl = false, adaptive = false;
    int w = 1280, h = 800;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc) bench.frames_per_phase = atol(argv[++i]);
        else if (strcmp(argv[i], "--farcadegl") == 0) farcadegl = true;
        else if (strcmp(argv[i], "--adaptive") == 0) adaptive = true;
        else if (strcmp(argv[i], "--scales") == 0) bench.scales = true;
        else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
    }
    static Editor ed;
    if (!ed.init(w, h)) { printf("FE EDITOR FAIL: init\n"); return 1; }
    ImGui::GetIO().MouseDrawCursor = true;
    ImGui::GetIO().AddMousePosEvent(w * 0.5f, h * 0.5f);
    demo_scene(ed);
    ed.adaptive_view = adaptive;
    if (farcadegl) ed.view_renderer = Editor::ViewRenderer::FarcadeGL;
    long frames = 0;
    double t0 = platform::time_seconds();
    while (ed.frame()) {
        frames++;
        if (frames == 1) {
            printf("FE EDITOR READY (%dx%d, first frame %.1f ms, viewport render %.1f ms at 1/%d size)\n", w, h,
                   (platform::time_seconds() - t0) * 1000.0, (double)ed.view_ms, ed.view_div_shown);
            ed.perf = Editor::Perf();
        } else if (!bench.after_frame(ed)) {
            break;
        }
    }
    ed.shutdown();
    return 0;
}
