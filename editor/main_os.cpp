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
// prints "FE BENCH <phase> ..." lines, then "FE BENCH DONE".
#include "editor.h"
#include "platform/platform.h"

#include "imgui/imgui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace fe;

static void demo_scene(Editor& ed) {
    ObjectId ground = ed.create_object("Ground", MeshKind::Plane, Vec3(0, 0, 0));
    ed.scene.get(ground)->scale = Vec3(5, 1, 5);
    ed.scene.get(ground)->color = Vec3(0.30f, 0.32f, 0.36f);
    ObjectId cube = ed.create_object("Crate", MeshKind::Cube, Vec3(-1.6f, 0.5f, 0.4f));
    ed.scene.get(cube)->color = Vec3(0.86f, 0.58f, 0.32f);
    ed.scene.get(cube)->rotation = Quat::euler_deg(Vec3(0, 20, 0));
    ObjectId sphere = ed.create_object("Ball", MeshKind::Sphere, Vec3(0.9f, 0.6f, -0.3f));
    ed.scene.get(sphere)->scale = Vec3(1.2f, 1.2f, 1.2f);
    ed.scene.get(sphere)->color = Vec3(0.35f, 0.62f, 0.95f);
    ObjectId pillar = ed.create_object("Pillar", MeshKind::Cylinder, Vec3(2.6f, 1.0f, 1.6f));
    ed.scene.get(pillar)->scale = Vec3(0.6f, 2.0f, 0.6f);
    ed.scene.get(pillar)->color = Vec3(0.75f, 0.76f, 0.80f);
    // A sun, a sky, a camera: what FarcadeGL lights and draws.
    ed.create_light(LightType::Directional, Vec3(-1.5f, 4.0f, -2.5f));
    ed.create_kind("Sky", ObjectKind::Sky, Vec3(-3.5f, 2.2f, -3.0f));
    ed.create_kind("Sky Light", ObjectKind::SkyLight, Vec3(-2.7f, 2.2f, -3.0f));
    ObjectId cam = ed.create_kind("Main Camera", ObjectKind::Camera, Vec3(-3.5f, 2.2f, 4.5f));
    ed.scene.get(cam)->rotation = Quat::euler_deg(Vec3(-18, -35, 0));
    // Physics: the ball falls onto the ground when Play starts.
    ed.scene.get(sphere)->position.y = 2.5f;
    ed.scene.add_module(sphere, ModuleKind::RigidBody3D);
    ed.scene.add_module(sphere, ModuleKind::Collider3D);
    ed.scene.add_module(ground, ModuleKind::Collider3D);
    ed.scene.select_only(cube);
    ed.scene.toggle(sphere);
    ed.tool = Tool::Move;
    ed.log(LogLevel::Info, "Farcade editor on Minimal-OS");
}

static void print_profile(const char* tag, Editor& ed, long frames, double total_ms) {
    Editor::Perf& p = ed.perf;
    double n = frames > 0 ? (double)frames : 1.0;
    printf("FE PROF %s %ld frames: %.1f ms/frame | build %.1f raster %.1f present %.1f wait %.1f | viewport %.1f ms x%d renders\n", tag,
           frames, total_ms / n, p.build / n, p.raster / n, p.present / n, p.wait / n, p.view_renders ? p.view / p.view_renders : 0.0,
           p.view_renders);
    printf("FE PROF %s   UI raster last frame: %u cmds, %u rects, %u triangles, %llu pixels\n", tag, ed.raster_stats.commands,
           ed.raster_stats.fast_rects, ed.raster_stats.triangles, (unsigned long long)ed.raster_stats.pixels);
    if (ed.view_renderer == Editor::ViewRenderer::FarcadeGL && p.view_renders) {
        const gfx::RendererStats& g = ed.fgl_stats;
        printf("FE PROF %s   FarcadeGL last render: %.1f ms = shader %.1f upload %.1f gpu %.1f readback %.1f swizzle %.1f (%d draws, %d tris, %d uploads, %d shaders)\n",
               tag, g.ms, g.shader_ms, g.upload_ms, g.submit_ms, g.read_ms, g.swizzle_ms, g.draws, g.triangles, g.uploads,
               g.shaders_compiled);
    }
    p = Editor::Perf();
}

int main(int argc, char** argv) {
    long max_frames = 0, bench = 0;
    bool farcadegl = false, play = false, profile = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) max_frames = atol(argv[++i]);
        else if (strcmp(argv[i], "--farcadegl") == 0) farcadegl = true;
        else if (strcmp(argv[i], "--play") == 0) play = true;
        else if (strcmp(argv[i], "--profile") == 0) profile = true;
        else if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc) bench = atol(argv[++i]);
    }

    static Editor ed;
    if (!ed.init(0, 0)) {
        printf("FE EDITOR FAIL: init (screen busy?)\n");
        return 1;
    }
    ImGui::GetIO().MouseDrawCursor = true;
    // The platform layer starts its mouse at the centre of the screen; show
    // the cursor there before the first mouse event arrives.
    ImGui::GetIO().AddMousePosEvent(platform::width() * 0.5f, platform::height() * 0.5f);
    demo_scene(ed);
    if (farcadegl) ed.view_renderer = Editor::ViewRenderer::FarcadeGL;

    long frames = 0;
    double window_start = platform::time_seconds();
    double t0 = window_start;
    // Benchmark phases: 0 idle, 1 mouse over the UI, 2 camera orbit.
    int phase = 0;
    long phase_frames = 0;
    double phase_start = 0;
    static const char* const kPhase[3] = { "idle", "mouse", "orbit" };
    bool bench_started = false;
    while (ed.frame()) {
        frames++;
        if (frames == 1) {
            printf("FE EDITOR READY (%dx%d, first frame %.1f ms)\n", platform::width(), platform::height(),
                   (platform::time_seconds() - t0) * 1000.0);
            if (farcadegl)
                printf("FE PROF first frame: FarcadeGL init %.1f ms (shaders %.1f), viewport render %.1f ms = shader %.1f upload %.1f gpu %.1f readback %.1f swizzle %.1f (%d shaders, %d uploads)\n",
                       ed.fgl_init_ms, ed.fgl_init_shader_ms, (double)ed.view_ms, ed.fgl_stats.shader_ms, ed.fgl_stats.upload_ms,
                       ed.fgl_stats.submit_ms, ed.fgl_stats.read_ms, ed.fgl_stats.swizzle_ms, ed.fgl_stats.shaders_compiled,
                       ed.fgl_stats.uploads);
            window_start = platform::time_seconds();
            ed.perf = Editor::Perf();
            if (play) ed.play();
            if (bench) { bench_started = true; phase_start = window_start; }
        } else if (!bench && (frames - 1) % 60 == 0) {
            double now = platform::time_seconds();
            printf("FE EDITOR frames %ld: %.1f ms/frame (last 60), renderer %s, viewport %.1f ms\n", frames, (now - window_start) * 1000.0 / 60.0,
                   ed.view_renderer == Editor::ViewRenderer::FarcadeGL ? "FarcadeGL" : "CPU", ed.view_ms);
            if (profile) print_profile("", ed, 60, (now - window_start) * 1000.0);
            if (ed.playing()) {
                Object* ball = nullptr;
                for (int i = 0; i < ed.scene.count(); i++) if (!strcmp(ed.scene.at(i).name, "Ball")) ball = &ed.scene.at(i);
                printf("FE EDITOR playing %.1f s, ball y %.2f\n", ed.play_time, ball ? ball->position.y : -1.0f);
            }
            window_start = now;
        }
        if (bench_started && frames > 1) {
            if (++phase_frames >= bench) {
                double now = platform::time_seconds();
                double total = (now - phase_start) * 1000.0;
                printf("FE BENCH %s: %.1f ms/frame over %ld frames, renderer %s\n", kPhase[phase], total / (double)phase_frames, phase_frames,
                       ed.view_renderer == Editor::ViewRenderer::FarcadeGL ? "FarcadeGL" : "CPU");
                print_profile(kPhase[phase], ed, phase_frames, total);
                phase++;
                phase_frames = 0;
                phase_start = now;
                ed.bench_interacting = phase == 2;
                if (phase == 3) { printf("FE BENCH DONE\n"); break; }
            }
            // Drive the next frame of the phase.
            if (phase == 1) {
                int w = platform::width(), h = platform::height();
                float x = w * 0.05f + (float)((phase_frames * 53) % (long)(w * 0.9f));
                float y = h * 0.10f + (float)((phase_frames * 31) % (long)(h * 0.8f));
                ImGui::GetIO().AddMousePosEvent(x, y);
            } else if (phase == 2) {
                ed.camera.yaw += 3.0f;
                ed.camera.pitch = 25.0f + (float)((phase_frames % 20) - 10);
            }
        }
        if (max_frames && frames >= max_frames) break;
        if (!bench) {
            double w0 = platform::time_seconds();
            platform::wait_event(16);
            ed.perf.wait += (platform::time_seconds() - w0) * 1000.0;
        }
    }
    ed.shutdown();
    printf("FE EDITOR DONE (%ld frames)\n", frames);
    return 0;
}
