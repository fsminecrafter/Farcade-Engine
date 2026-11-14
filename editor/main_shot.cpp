// fe-editor-shot: renders the editor with a small demo scene into PNG files,
// so its look can be checked without a screen (host only).
//
//   fe-editor-shot <out_dir> [width height]
//
// Writes <out_dir>/editor-dark.png and <out_dir>/editor-light.png. The demo
// project (a few files to show in the Files panel) is created in
// <out_dir>/demo-project.
#include "editor.h"
#include "platform/headless.h"

#include "imgui/imgui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

using namespace fe;

static double real_seconds() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void touch(const char* dir, const char* name, const char* text) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    platform::write_file(path, text, strlen(text));
}

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
    ObjectId small = ed.create_object("Crate", MeshKind::Cube, Vec3(-2.4f, 0.3f, -1.6f));
    ed.scene.get(small)->scale = Vec3(0.6f, 0.6f, 0.6f);
    ed.scene.get(small)->color = Vec3(0.55f, 0.75f, 0.45f);
    // A small tree: the green crate sits on the big one, as its child, and
    // the lamp is a child of the pillar.
    ed.scene.set_parent(small, cube);
    ObjectId lamp = ed.create_child("Lamp", MeshKind::Sphere, pillar);
    ed.scene.get(lamp)->position = Vec3(0, 0.65f, 0);
    ed.scene.get(lamp)->scale = Vec3(0.6f, 0.15f, 0.6f);
    ed.scene.get(lamp)->color = Vec3(0.98f, 0.85f, 0.45f);
    snprintf(ed.scene.get(cube)->texture_path, 256, "%s", "assets/crate.png");
    snprintf(ed.scene.get(sphere)->script_path, 256, "%s", "scripts/ball.lua");
    // Crate selected, then shift-click on the ball: two selected, the ball active.
    ed.scene.select_only(cube);
    ed.scene.toggle(sphere);
    ed.tool = Tool::Move;
    ed.log(LogLevel::Info, "Opened project demo-project");
    ed.log(LogLevel::Info, "Imported assets/crate.png (512x512)");
    ed.log(LogLevel::Warning, "scripts/ball.lua: global 'speed' is never assigned");
    ed.log(LogLevel::Info, "Crate: texture crate.png");
}

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : ".";
    int w = argc > 3 ? atoi(argv[2]) : 1280;
    int h = argc > 3 ? atoi(argv[3]) : 800;

    char project[512];
    snprintf(project, sizeof(project), "%s/demo-project", out);
    mkdir(out, 0755);
    mkdir(project, 0755);
    char sub[600];
    snprintf(sub, sizeof(sub), "%s/assets", project); mkdir(sub, 0755);
    snprintf(sub, sizeof(sub), "%s/scripts", project); mkdir(sub, 0755);
    snprintf(sub, sizeof(sub), "%s/scenes", project); mkdir(sub, 0755);
    touch(project, "project.fe", "name = \"Demo\"\n");
    touch(project, "crate.mof", "MOF");
    touch(project, "ball.lua", "function on_update(dt) end\n");
    touch(project, "crate.png", "PNG");
    touch(project, "grass.jpg", "JPG");
    touch(project, "notes.txt", "todo\n");

    static Editor ed;
    if (!ed.init(w, h)) { fprintf(stderr, "init failed\n"); return 1; }
    snprintf(ed.project_dir, sizeof(ed.project_dir), "%s", project);
    snprintf(ed.files_dir, sizeof(ed.files_dir), "%s", project);
    ed.perf_clock = real_seconds;
    demo_scene(ed);

    platform::headless_set_time(0.0);
    // A few frames: the first builds the dock layout and font atlas.
    for (int i = 0; i < 4; i++) {
        platform::headless_set_time(i / 60.0);
        platform::Event mv;
        mv.type = platform::EventType::MouseMove;
        mv.x = -100; mv.y = -100;
        platform::headless_push(mv);
        ed.frame();
    }
    // Timing: a few more frames, the whole frame and the viewport alone.
    {
        const int n = 10;
        double t0 = real_seconds(), view = 0;
        for (int i = 0; i < n; i++) {
            platform::headless_set_time((4 + i) / 60.0);
            ed.frame();
            view += ed.view_ms;
        }
        double ms = (real_seconds() - t0) * 1000.0 / n;
        printf("frame %.2f ms when idle (a full viewport render: %.2f ms, %d triangles, %dx%d)\n", ms, view / n, ed.view_triangles, (int)ed.view_rect[2],
               (int)ed.view_rect[3]);
        // What a frame costs while dragging: the camera moves every frame and
        // the view renders without supersampling and shadows.
        bool ss = ed.settings.view_supersample, sh = ed.settings.view_shadows;
        ed.settings.view_supersample = false;
        ed.settings.view_shadows = false;
        double view_fast = 0;
        t0 = real_seconds();
        for (int i = 0; i < n; i++) {
            ed.camera.yaw += 0.5f;
            ed.frame();
            view_fast += ed.view_ms;
        }
        ms = (real_seconds() - t0) * 1000.0 / n;
        printf("frame %.2f ms while dragging (viewport %.2f ms)\n", ms, view_fast / n);
        ed.camera.yaw -= 0.5f * n;
        ed.settings.view_supersample = ss;
        ed.settings.view_shadows = sh;
        ed.frame();
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/editor-dark.png", out);
    if (!platform::headless_save_png(path)) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    printf("wrote %s\n", path);

    ed.settings.dark_theme = false;
    ed.apply_theme();
    for (int i = 14; i < 17; i++) { platform::headless_set_time(i / 60.0); ed.frame(); }
    snprintf(path, sizeof(path), "%s/editor-light.png", out);
    if (!platform::headless_save_png(path)) return 1;
    printf("wrote %s\n", path);

    // Play mode with the Console in front and nothing selected (the empty
    // inspector), in both themes; then the renderer menu open.
    ed.scene.clear_selection();
    ed.play();
    ImGui::SetWindowFocus("Console");
    for (int t = 0; t < 2; t++) {
        ed.settings.dark_theme = t == 0;
        ed.apply_theme();
        for (int i = 0; i < 40; i++) { platform::headless_set_time((20 + t * 40 + i) / 60.0); ed.frame(); }
        snprintf(path, sizeof(path), "%s/editor-play-%s.png", out, t == 0 ? "dark" : "light");
        if (!platform::headless_save_png(path)) return 1;
        printf("wrote %s\n", path);
    }
    ed.stop();
    ed.settings.dark_theme = true;
    ed.apply_theme();
    ed.frame();
    if (const Editor::UiRect* r = ed.find_rect("run:renderer")) {
        float x = r->x + r->w * 0.5f, y = r->y + r->h * 0.5f;
        platform::Event e;
        e.type = platform::EventType::MouseMove; e.x = x; e.y = y;
        platform::headless_push(e); ed.frame();
        e.type = platform::EventType::MouseButton; e.button = 0; e.down = true;
        platform::headless_push(e); ed.frame();
        e.down = false;
        platform::headless_push(e); ed.frame();
        e.type = platform::EventType::MouseMove; e.x = x - 200; e.y = y + 400;
        platform::headless_push(e); ed.frame(); ed.frame();
        snprintf(path, sizeof(path), "%s/editor-renderer-menu.png", out);
        if (!platform::headless_save_png(path)) return 1;
        printf("wrote %s\n", path);
    }
    // The settings window, on its Viewport tab.
    ed.show_settings = true;
    ed.frame();
    ed.frame();
    snprintf(path, sizeof(path), "%s/editor-settings.png", out);
    if (!platform::headless_save_png(path)) return 1;
    printf("wrote %s\n", path);
    ed.shutdown();
    return 0;
}
