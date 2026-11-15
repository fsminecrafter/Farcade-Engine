// Shared by the editor programs (main_os.cpp on Minimal-OS, main_bench.cpp on
// the host): the demo scene and the benchmark/profile driver.
//
//   --profile  adds a per-phase breakdown to every 60-frame report
//   --bench N  runs idle, mouse (the pointer sweeps over the UI) and orbit (the
//              camera turns every frame, the viewport in its "dragging"
//              quality) phases of N measured frames (after 5 warm-up frames
//              each), then a "settle" phase (the camera stops; time until the
//              viewport is at its final quality), and prints "FE BENCH ..."
//              lines and "FE BENCH DONE".
//   --scales   adds orbit phases with FarcadeGL at a fixed 1/2 and 1/4 size
#pragma once

#include "editor.h"
#include "platform/platform.h"

#include "imgui/imgui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe {

inline void demo_scene(Editor& ed) {
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

inline void print_profile(const char* tag, Editor& ed, long frames, double total_ms) {
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


// Drives the benchmark phases: call after_frame() after every Editor::frame().
class Bench {
public:
    long frames_per_phase = 0;          // 0: off
    bool scales = false;

    bool active() const { return frames_per_phase > 0; }

    // Returns false when the benchmark is over.
    bool after_frame(Editor& ed) {
        const Phase& ph = kPhases[phase_];
        if (phase_ == kSettle) return settle_frame(ed);
        // Start from a viewport image that is final (the first one is a quick draft).
        if (phase_ == 0 && !ed.view_final && prelude_ < 400) { prelude_++; return true; }
        frame_++;
        if (frame_ == 0) { start_ = platform::time_seconds(); ed.perf = Editor::Perf(); }
        if (frame_ >= frames_per_phase) {
            double now = platform::time_seconds();
            double total = (now - start_) * 1000.0;
            printf("FE BENCH %s: %.1f ms/frame over %ld frames, renderer %s\n", ph.name, total / (double)frame_, frame_,
                   ed.view_renderer == Editor::ViewRenderer::FarcadeGL ? "FarcadeGL" : "CPU");
            print_profile(ph.name, ed, frame_, total);
            frame_ = -kWarmup;
            phase_++;
            if (phase_ == count_()) phase_ = kSettle;
            if (phase_ == kSettle) { settle_start_ = platform::time_seconds(); settle_frames_ = 0; settle_renders_ = ed.view_renders; ed.bench_interacting = false; return true; }
            ed.bench_interacting = kPhases[phase_].kind == 2;
            ed.view_scale_div = kPhases[phase_].div;
        }
        const Phase& cur = kPhases[phase_];
        long i = frame_ + kWarmup;
        if (cur.kind == 1) {
            int w = platform::width(), h = platform::height();
            float x = w * 0.05f + (float)((i * 53) % (long)(w * 0.9f));
            float y = h * 0.10f + (float)((i * 31) % (long)(h * 0.8f));
            ImGui::GetIO().AddMousePosEvent(x, y);
        } else if (cur.kind == 2) {
            ed.camera.yaw += 3.0f;
            ed.camera.pitch = 25.0f + (float)((i % 20) - 10);
        }
        return true;
    }

private:
    struct Phase { const char* name; int kind; int div; };
    static constexpr int kSettle = 100;
    static constexpr long kWarmup = 5;
    static constexpr Phase kPhases[5] = { { "idle", 0, 1 }, { "mouse", 1, 1 }, { "orbit", 2, 1 }, { "orbit/2", 2, 2 }, { "orbit/4", 2, 4 } };
    int count_() const { return scales ? 5 : 3; }

    // After the orbit the camera stops: how long until the image is final.
    bool settle_frame(Editor& ed) {
        settle_frames_++;
        bool final_q = ed.view_final;
        if (final_q || settle_frames_ >= 150) {
            double ms = (platform::time_seconds() - settle_start_) * 1000.0;
            printf("FE BENCH settle: %.1f ms and %ld frames (%u renders) until the viewport image was final (1/%d size%s)\n", ms, settle_frames_,
                   ed.view_renders - settle_renders_, ed.view_div_shown, final_q ? "" : ", NOT reached");
            printf("FE BENCH DONE\n");
            return false;
        }
        return true;
    }

    int phase_ = 0;
    int prelude_ = 0;
    long frame_ = -kWarmup;
    double start_ = 0;
    double settle_start_ = 0;
    long settle_frames_ = 0;
    uint32_t settle_renders_ = 0;
};

}  // namespace fe
