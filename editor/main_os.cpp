// farcade-editor: the editor on Minimal-OS (full screen).
//
//   run 0:/programs/farcade-editor.run [--frames N] [--farcadegl] [--play]
//
// Prints "FE EDITOR READY" once the first frame is on the screen, then every
// 60 frames the average frame time. With --frames N it quits after N frames
// and prints "FE EDITOR DONE". --farcadegl draws the viewport with FarcadeGL
// (the kernel's FGPU) instead of the CPU preview and reports, every 60
// frames, whether it still does ("renderer FarcadeGL" or "renderer CPU").
// --play presses Play after the first frame (the ball has physics) and adds
// the ball's height to the report.
// --profile adds a per-phase breakdown (ms per frame: UI build, viewport
// render, UI rasterizing, present, waiting; for FarcadeGL also shader compile,
// upload, GPU run, readback) to every 60-frame report.
// --bench N runs three phases of N frames each without waiting for input:
// idle (nothing happens), mouse (the pointer sweeps over the UI) and orbit (the
// camera turns every frame, with the viewport in its "dragging" quality), and
// prints "FE BENCH <phase> ..." lines (the first 5 frames of a phase are not
// measured), then "FE BENCH DONE". --scales adds orbit phases with FarcadeGL at
// 1/2 and 1/4 of the viewport size.
// --export <project-or-scene> <outdir> [--name N] [--scene S] [--player P] [--run [FRAMES]]
// exports a game without opening the editor (docs/export.md) and prints
// "FE EXPORT OK ..." or "FE EXPORT FAIL: ...".
// --scene-test saves the demo scene to <project>/scenes/os-test.fscene,
// opens it again and prints "FE SCENE PASS <objects> <bytes>" when saving
// the opened scene gives the same file (else "FE SCENE FAIL ...").
#include "bench.h"
#include "scene_io.h"
#include "editor.h"
#include "export.h"
#include "platform/platform.h"

#include "imgui/imgui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace fe;

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--export") == 0) return export_cli(argc, argv, i);
    long max_frames = 0;
    Bench bench;
    bool farcadegl = false, play = false, profile = false, adaptive = true, scene_test = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) max_frames = atol(argv[++i]);
        else if (strcmp(argv[i], "--farcadegl") == 0) farcadegl = true;
        else if (strcmp(argv[i], "--play") == 0) play = true;
        else if (strcmp(argv[i], "--profile") == 0) profile = true;
        else if (strcmp(argv[i], "--scales") == 0) bench.scales = true;
        else if (strcmp(argv[i], "--no-adaptive") == 0) adaptive = false;
        else if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc) bench.frames_per_phase = atol(argv[++i]);
        else if (strcmp(argv[i], "--scene-test") == 0) scene_test = true;
    }

    static Editor ed;
    if (!ed.init(0, 0)) {
        printf("FE EDITOR FAIL: init (screen busy?)\n");
        return 1;
    }
    // Full screen draws its own cursor; in a desktop window mosWM draws it.
    ImGui::GetIO().MouseDrawCursor = !platform::windowed();
    if (platform::windowed()) printf("FE EDITOR windowed (MOSdesk)\n");
    // The platform layer starts its mouse at the centre of the screen; show
    // the cursor there before the first mouse event arrives.
    ImGui::GetIO().AddMousePosEvent(platform::width() * 0.5f, platform::height() * 0.5f);
    demo_scene(ed);
    ed.history_reset();         // the demo scene is where undo starts
    if (scene_test) {
        size_t n1 = 0, n2 = 0;
        int count = ed.scene.count();
        // Twice: the second save replaces the file (with a longer scene in
        // between, so stale bytes would show).
        ObjectId extra = ed.create_object("Extra object with a long name", MeshKind::Cube, Vec3(0, 0, 0));
        bool saved = ed.save_scene("scenes/os-test.fscene");
        ed.scene.remove(extra);
        char* t1 = save_scene_text(ed.scene, ed.project_dir, &n1);     // next_id counts the removed object
        saved = ed.save_scene("scenes/os-test.fscene") && saved;
        ed.new_scene();
        bool opened = ed.open_scene("scenes/os-test.fscene");
        char* t2 = save_scene_text(ed.scene, ed.project_dir, &n2);
        bool same = t1 && t2 && n1 == n2 && memcmp(t1, t2, n1) == 0;
        if (saved && opened && same && ed.scene.count() == count) printf("FE SCENE PASS %d %u\n", count, (unsigned)n1);
        else printf("FE SCENE FAIL saved %d opened %d same %d objects %d/%d (%s)\n", saved, opened, same, ed.scene.count(), count, ed.scene_path);
        free(t1);
        free(t2);
    }
    ed.adaptive_view = adaptive;
    if (farcadegl) ed.view_renderer = Editor::ViewRenderer::FarcadeGL;

    long frames = 0;
    double window_start = platform::time_seconds();
    double t0 = window_start;
    bool bench_running = false;
    while (ed.frame()) {
        frames++;
        if (frames == 1) {
            printf("FE EDITOR READY (%dx%d, first frame %.1f ms)\n", platform::width(), platform::height(),
                   (platform::time_seconds() - t0) * 1000.0);
            printf("FE PROF first frame: viewport render %.1f ms at 1/%d size", (double)ed.view_ms, ed.view_div_shown);
            if (farcadegl)
                printf(", FarcadeGL init %.1f ms (shaders %.1f), render = shader %.1f upload %.1f gpu %.1f readback %.1f swizzle %.1f (%d shaders, %d uploads)",
                       ed.fgl_init_ms, ed.fgl_init_shader_ms, ed.fgl_stats.shader_ms, ed.fgl_stats.upload_ms, ed.fgl_stats.submit_ms,
                       ed.fgl_stats.read_ms, ed.fgl_stats.swizzle_ms, ed.fgl_stats.shaders_compiled, ed.fgl_stats.uploads);
            printf("\n");
            window_start = platform::time_seconds();
            ed.perf = Editor::Perf();
            if (play) ed.play();
            bench_running = bench.active();
        } else if (!bench.active() && (frames - 1) % 60 == 0) {
            double now = platform::time_seconds();
            printf("FE EDITOR frames %ld: %.1f ms/frame (last 60), renderer %s, viewport %.1f ms (1/%d size)\n", frames,
                   (now - window_start) * 1000.0 / 60.0, ed.view_renderer == Editor::ViewRenderer::FarcadeGL ? "FarcadeGL" : "CPU",
                   ed.view_ms, ed.view_div_shown);
            if (profile) print_profile("", ed, 60, (now - window_start) * 1000.0);
            if (ed.playing()) {
                Object* ball = nullptr;
                for (int i = 0; i < ed.scene.count(); i++) if (!strcmp(ed.scene.at(i).name, "Ball")) ball = &ed.scene.at(i);
                printf("FE EDITOR playing %.1f s, ball y %.2f\n", ed.play_time, ball ? ball->position.y : -1.0f);
            }
            window_start = now;
        }
        if (bench_running && frames > 1 && !bench.after_frame(ed)) break;
        if (max_frames && frames >= max_frames) break;
        if (!bench.active()) {
            double w0 = platform::time_seconds();
            platform::wait_event(ed.last_frame_idle ? 40 : 16);
            ed.perf.wait += (platform::time_seconds() - w0) * 1000.0;
        }
    }
    ed.shutdown();
    printf("FE EDITOR DONE (%ld frames)\n", frames);
    return 0;
}
