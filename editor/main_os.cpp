// farcade-editor: the editor on Minimal-OS (full screen).
//
//   run 0:/programs/farcade-editor.run [--frames N]
//
// Prints "FE EDITOR READY" once the first frame is on the screen, then every
// 60 frames the average frame time. With --frames N it quits after N frames
// and prints "FE EDITOR DONE".
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
    ed.scene.select_only(cube);
    ed.scene.toggle(sphere);
    ed.tool = Tool::Move;
    ed.log(LogLevel::Info, "Farcade editor on Minimal-OS");
}

int main(int argc, char** argv) {
    long max_frames = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) max_frames = atol(argv[++i]);

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

    long frames = 0;
    double window_start = platform::time_seconds();
    double t0 = window_start;
    while (ed.frame()) {
        frames++;
        if (frames == 1) {
            printf("FE EDITOR READY (%dx%d, first frame %.1f ms)\n", platform::width(), platform::height(),
                   (platform::time_seconds() - t0) * 1000.0);
            window_start = platform::time_seconds();
        } else if ((frames - 1) % 60 == 0) {
            double now = platform::time_seconds();
            printf("FE EDITOR frames %ld: %.1f ms/frame (last 60)\n", frames, (now - window_start) * 1000.0 / 60.0);
            window_start = now;
        }
        if (max_frames && frames >= max_frames) break;
        platform::wait_event(16);
    }
    ed.shutdown();
    printf("FE EDITOR DONE (%ld frames)\n", frames);
    return 0;
}
