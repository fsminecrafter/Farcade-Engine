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
#include "imgui/imgui_internal.h"

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
    ed.scene.get(sphere)->roughness = 0.25f;      // glossy
    ObjectId pillar = ed.create_object("Pillar", MeshKind::Cylinder, Vec3(2.6f, 1.0f, 1.6f));
    ed.scene.get(pillar)->scale = Vec3(0.6f, 2.0f, 0.6f);
    ed.scene.get(pillar)->color = Vec3(0.75f, 0.76f, 0.80f);
    ed.scene.get(pillar)->metallic = 0.9f;        // brushed metal (GPU tier)
    ed.scene.get(pillar)->roughness = 0.3f;
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
    // Two scripts on the ball, with inputs set in the editor; the lamp's
    // script links to the ball (its "follow" input) to reach its scripts.
    ed.add_script(sphere, "scripts/ball.lua");
    ed.add_script(sphere, "scripts/hover.lua");
    if (ScriptProp* p = ed.scene.script_prop(sphere, "ball", "speed")) p->number = 7.5f;
    if (ScriptProp* p = ed.scene.script_prop(sphere, "ball", "mode")) p->choice = 1;
    ed.add_script(lamp, "scripts/lamp.lua");
    if (ScriptProp* p = ed.scene.script_prop(lamp, "lamp", "follow")) p->object = sphere;
    // Scene objects: a camera (current, with the listener), a sun, sky,
    // sky light and post processing; physics on the ball and the ground.
    ObjectId cam = ed.create_kind("Main Camera", ObjectKind::Camera, Vec3(-3.5f, 2.2f, 4.5f));
    ed.scene.get(cam)->rotation = Quat::euler_deg(Vec3(-18, -35, 0));
    ed.scene.add_module(cam, ModuleKind::AudioListener);
    ObjectId cam2 = ed.create_kind("Overview Camera", ObjectKind::Camera, Vec3(4.5f, 5.0f, -4.0f));
    ed.scene.get(cam2)->rotation = Quat::euler_deg(Vec3(-40, 135, 0));
    ed.create_light(LightType::Directional, Vec3(-1.5f, 4.0f, -2.5f));
    ObjectId pl = ed.create_light(LightType::Point, Vec3(0, 0.9f, 0), lamp);
    ed.scene.get(pl)->modules.light.color = Vec3(1.0f, 0.8f, 0.45f);
    ed.create_kind("Sky", ObjectKind::Sky, Vec3(-4.2f, 2.2f, -3.0f));
    ed.create_kind("Sky Light", ObjectKind::SkyLight, Vec3(-3.4f, 2.2f, -3.0f));
    ed.create_kind("Post Processing", ObjectKind::PostProcess, Vec3(-2.6f, 2.2f, -3.0f));
    ed.scene.add_module(sphere, ModuleKind::RigidBody3D);
    ed.scene.add_module(sphere, ModuleKind::Collider3D);
    ed.scene.add_module(sphere, ModuleKind::AudioSampler);
    snprintf(ed.scene.get(sphere)->modules.audio_sampler.path, kPathMax, "%s", "sounds/bounce.wav");
    ed.scene.add_module(ground, ModuleKind::Collider3D);
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
    touch(project, "scripts/ball.lua",
          "-- Rolls the ball around.\n"
          "-- @prop speed  number = 5  range 0 20\n"
          "-- @prop mode   list(roll, bounce, idle) = roll\n"
          "-- @prop label  text = \"Player ball\"\n"
          "-- @prop sticky toggle = false\n"
          "\n"
          "function on_update(self, dt)\n"
          "  if self.mode == \"idle\" then return end\n"
          "  self.object:move(self.speed * dt, 0, 0)\n"
          "end\n");
    touch(project, "scripts/hover.lua",
          "-- @prop height number = 0.4\n"
          "-- @prop enabled toggle = true\n"
          "function on_update(self, dt) end\n");
    touch(project, "scripts/lamp.lua",
          "-- Lights up when the ball rolls fast.\n"
          "-- @prop follow object\n"
          "-- @prop threshold number = 6\n"
          "function on_update(self, dt)\n"
          "  local ball = self.follow and self.follow:script(\"ball\")\n"
          "  self.on = ball ~= nil and ball.speed > self.threshold\n"
          "end\n");
    touch(project, "crate.png", "PNG");
    touch(project, "grass.jpg", "JPG");
    touch(project, "notes.txt", "todo\n");

    static Editor ed;
    if (!ed.init(w, h)) { fprintf(stderr, "init failed\n"); return 1; }
    snprintf(ed.project_dir, sizeof(ed.project_dir), "%s", project);
    snprintf(ed.files_dir, sizeof(ed.files_dir), "%s", project);
    ed.perf_clock = real_seconds;
    demo_scene(ed);
    ed.history_reset();         // the demo scene is where undo starts

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
    ImGui::SetWindowFocus("###Console");
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
    // The ball's modules (Inspector scrolled down), then the overview
    // camera selected: its settings and the Current switch.
    {
        auto by_name = [&](const char* n) { for (int i = 0; i < ed.scene.count(); i++) if (!strcmp(ed.scene.at(i).name, n)) return ed.scene.at(i).id; return kNoObject; };
        ObjectId sphere = by_name("Ball"), cam2 = by_name("Overview Camera");
        ed.scene.select_only(sphere);
        ed.frame();
        ImGuiWindow* iw = ImGui::FindWindowByName("###Inspector");
        for (int i = 0; i < 3; i++) { if (iw) ImGui::SetScrollY(iw, 330); ed.frame(); }
        snprintf(path, sizeof(path), "%s/editor-modules.png", out);
        if (!platform::headless_save_png(path)) return 1;
        printf("wrote %s\n", path);
        ed.scene.select_only(cam2);
        for (int i = 0; i < 3; i++) { if (iw) ImGui::SetScrollY(iw, 0); ed.frame(); }
        snprintf(path, sizeof(path), "%s/editor-camera.png", out);
        if (!platform::headless_save_png(path)) return 1;
        printf("wrote %s\n", path);
    }
    // Shaping a collider: the pillar's cylinder collider with its handles.
    {
        ObjectId pil = kNoObject;
        for (int i = 0; i < ed.scene.count(); i++) if (!strcmp(ed.scene.at(i).name, "Pillar")) pil = ed.scene.at(i).id;
        ed.scene.add_module(pil, ModuleKind::Collider3D);
        ed.scene.get(pil)->modules.collider3d.radius = 0.6f;
        ed.scene.select_only(pil);
        ed.edit_collider = true;
        ed.tool = Tool::Select;
        ImGuiWindow* iw = ImGui::FindWindowByName("###Inspector");
        for (int i = 0; i < 3; i++) { if (iw) ImGui::SetScrollY(iw, 300); ed.frame(); }
        snprintf(path, sizeof(path), "%s/editor-collider.png", out);
        if (!platform::headless_save_png(path)) return 1;
        printf("wrote %s\n", path);
        ed.edit_collider = false;
        ed.tool = Tool::Move;
        if (iw) ImGui::SetScrollY(iw, 0);
    }
    // The FarcadeGL renderer: the editor view, then Play through the current camera.
    {
        ed.view_renderer = Editor::ViewRenderer::FarcadeGL;
        ed.scene.select_only(kNoObject);
        for (int i = 0; i < 3; i++) ed.frame();
        printf("FarcadeGL viewport: %.1f ms\n", ed.view_ms);
        snprintf(path, sizeof(path), "%s/editor-farcadegl.png", out);
        if (!platform::headless_save_png(path)) return 1;
        printf("wrote %s\n", path);
        // The GPU tier (shadows, AO, bloom, AA, PBR), forced on the CPU backend.
        ed.settings.graphics.apply_preset(gfx::GraphicsPreset::Ultra);
        ed.settings.graphics.allow_gpu_effects_on_cpu = true;
        for (int i = 0; i < 3; i++) ed.frame();
        printf("FarcadeGL viewport, GPU tier: %.1f ms\n", ed.view_ms);
        snprintf(path, sizeof(path), "%s/editor-farcadegl-ultra.png", out);
        if (!platform::headless_save_png(path)) return 1;
        printf("wrote %s\n", path);
        // The pillar's material in the Inspector (Look: roughness, metal),
        // with the scene saved as scenes/main.fscene (its name in the title).
        for (int i = 0; i < ed.scene.count(); i++)
            if (!strcmp(ed.scene.at(i).name, "Pillar")) ed.scene.select_only(ed.scene.at(i).id);
        ed.save_scene();
        for (int i = 0; i < 2; i++) ed.frame();
        if (ImGuiWindow* iw2 = ImGui::FindWindowByName("###Inspector")) {
            const Editor::UiRect* r = ed.find_rect("inspector:roughness");
            if (r) ImGui::SetScrollY(iw2, iw2->Scroll.y + (r->y - iw2->Pos.y) - 160);
        }
        for (int i = 0; i < 3; i++) ed.frame();
        snprintf(path, sizeof(path), "%s/editor-material.png", out);
        if (!platform::headless_save_png(path)) return 1;
        printf("wrote %s\n", path);
        if (ImGuiWindow* iw2 = ImGui::FindWindowByName("###Inspector")) ImGui::SetScrollY(iw2, 0);
        // An unsaved change, then File > New scene: the prompt. The pillar
        // turns gold (one undo step), then Escape cancels the prompt and
        // Ctrl+Z puts the pillar back.
        for (int i = 0; i < ed.scene.count(); i++)
            if (!strcmp(ed.scene.at(i).name, "Pillar")) ed.scene.at(i).color = Vec3(0.95f, 0.75f, 0.35f);
        ed.history_capture();
        ed.log(LogLevel::Info, "Pillar: color changed (Ctrl+Z undoes it)");
        ed.request_new_scene();
        for (int i = 0; i < 3; i++) ed.frame();
        snprintf(path, sizeof(path), "%s/editor-unsaved.png", out);
        if (!platform::headless_save_png(path)) return 1;
        printf("wrote %s\n", path);
        {
            platform::Event k;
            k.type = platform::EventType::Key;
            k.key = platform::KEY_ESCAPE; k.down = true;
            platform::headless_push(k); ed.frame();
            k.down = false;
            platform::headless_push(k); ed.frame(); ed.frame();
        }
        ed.undo();
        ed.scene.select_only(kNoObject);
        ed.play();
        for (int i = 0; i < 3; i++) ed.frame();
        snprintf(path, sizeof(path), "%s/editor-farcadegl-play.png", out);
        if (!platform::headless_save_png(path)) return 1;
        printf("wrote %s\n", path);
        ed.stop();
        ed.settings.graphics.allow_gpu_effects_on_cpu = false;
        ed.settings.graphics.apply_preset(gfx::GraphicsPreset::High);
        ed.view_renderer = Editor::ViewRenderer::CpuPreview;
        ed.frame();
    }
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
    // The Create menu open (its items carry the vector icons), with the
    // Light submenu. Menus need time to pass between frames.
    {
        static double t = 1000.0;
        auto step = [&](int n) { for (int i = 0; i < n; i++) { t += 1.0 / 60.0; platform::headless_set_time(t); ed.frame(); } };
        auto move = [&](float x, float y) {
            platform::Event e;
            e.type = platform::EventType::MouseMove; e.x = x; e.y = y;
            platform::headless_push(e);
        };
        auto button = [&](float x, float y, bool down) {
            platform::Event e;
            e.type = platform::EventType::MouseButton; e.button = 0; e.down = down; e.x = x; e.y = y;
            platform::headless_push(e);
        };
        // The renderer menu from the shot above is still open: close it first.
        {
            platform::Event k;
            k.type = platform::EventType::Key; k.key = platform::KEY_ESCAPE; k.down = true;
            platform::headless_push(k); step(1);
            k.down = false;
            platform::headless_push(k); step(2);
        }
        if (const Editor::UiRect* r = ed.find_rect("menubar:Create")) {
            float x = r->x + r->w * 0.5f, y = r->y + r->h * 0.5f;
            move(x, y); step(1);
            button(x, y, true); step(1);
            button(x, y, false); step(2);
            if (const Editor::UiRect* c = ed.find_rect("menu:Camera")) {
                // "Light" is the row above "Camera".
                move(c->x + 30, c->y - c->h * 0.5f);
                step(30);
            }
            snprintf(path, sizeof(path), "%s/editor-menu.png", out);
            if (!platform::headless_save_png(path)) return 1;
            printf("wrote %s\n", path);
            platform::Event k;
            k.type = platform::EventType::Key; k.key = platform::KEY_ESCAPE; k.down = true;
            platform::headless_push(k); step(1);
            k.down = false;
            platform::headless_push(k); step(1);
            move(10, 790); step(2);
        }
    }
    // The settings window, on its Graphics page.
    ed.show_settings = true;
    ed.settings_page = 3;       // Graphics: the page with the most to show
    ed.frame();
    ed.frame();
    snprintf(path, sizeof(path), "%s/editor-settings.png", out);
    if (!platform::headless_save_png(path)) return 1;
    printf("wrote %s\n", path);
    ed.shutdown();
    return 0;
}
