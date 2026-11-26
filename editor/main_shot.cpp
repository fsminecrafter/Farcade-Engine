// fe-editor-shot: renders the editor with a small demo scene into PNG files,
// so its look can be checked without a screen (host only).
//
//   fe-editor-shot <out_dir> [width height]
//   fe-editor-shot --world <project> <out.png> <x> <y> <z> <yaw> <pitch> <distance> [frames] [low|high|ultra]
//
// --world opens a project (its entry scene) and renders the view of the orbit camera around (x, y, z) after
// `frames` frames (default 40, so the terrain streams in and the foliage is built), with the editor's panels.
// Prints the terrain and draw stats. Used to check the example world (docs/example-world.md).
//
// Writes <out_dir>/editor-dark.png and <out_dir>/editor-light.png. The demo
// project (a few files to show in the Files panel) is created in
// <out_dir>/demo-project.
#include "editor.h"
#include "terrain_bridge.h"
#include "model_import.h"
#include "platform/headless.h"

#include "imgui/imgui.h"
#define STB_IMAGE_WRITE_STATIC           // a private copy (the platform and the importers have their own)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
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

static int world_shot(int argc, char** argv) {
    if (argc < 10) { fprintf(stderr, "usage: fe-editor-shot --world <project> <out.png> x y z yaw pitch distance [frames] [low|high|ultra]\n"); return 2; }
    static Editor ed;
    if (!ed.init(1600, 900)) { fprintf(stderr, "init failed\n"); return 1; }
    ed.perf_clock = real_seconds;
    ed.open_project(argv[2]);
    const char* q = argc > 11 ? argv[11] : "ultra";
    gfx::GraphicsPreset preset = !strcmp(q, "low") ? gfx::GraphicsPreset::Low : !strcmp(q, "high") ? gfx::GraphicsPreset::High : gfx::GraphicsPreset::Ultra;
    ed.settings.graphics.apply_preset(preset);
    ed.settings.graphics.allow_gpu_effects_on_cpu = preset == gfx::GraphicsPreset::Ultra;
    ed.settings.graphics.feature_level = preset == gfx::GraphicsPreset::Ultra ? gfx::FeatureLevelMode::GpuTier : gfx::FeatureLevelMode::Auto;
    ed.view_renderer = Editor::ViewRenderer::FarcadeGL;
    ed.settings.show_grid = false;
    ed.scene.clear_selection();
    // y as "+N": N meters above the ground at (x, z).
    float cx = (float)atof(argv[4]), cz = (float)atof(argv[6]), cy = (float)atof(argv[5]);
    if (argv[5][0] == '+')
        for (int i = 0; i < ed.scene.count(); i++) {
            Object& o = ed.scene.at(i);
            TerrainData* td = o.kind == ObjectKind::Terrain ? terrain_data(o) : nullptr;
            if (!td) continue;
            Vec3 org = terrain_origin(ed.scene, o);
            bool inside = false;
            float h = td->height_at(cx - org.x, cz - org.z, &inside);
            if (inside) { cy = org.y + h + cy; printf("ground at %.1f\n", org.y + h); break; }
        }
    ed.camera.target = Vec3(cx, cy, cz);
    ed.camera.yaw = (float)atof(argv[7]);
    ed.camera.pitch = (float)atof(argv[8]);
    ed.camera.distance = (float)atof(argv[9]);
    int frames = argc > 10 ? atoi(argv[10]) : 40;
    terrain_build_budget_override(1e9);      // every frame builds all the foliage it wants (a still shot)
    double t0 = real_seconds();
    // `frames` frames once FarcadeGL has compiled its shaders (a few frames of compiling come first).
    int i = 0, after = 0;
    while (after < frames && i < frames + 600) {
        platform::headless_set_time(i++ / 30.0);
        ed.frame();
        if (!ed.fgl_compiling) after++;
    }
    const TerrainViewStats& st = ed.terrain_stats_;
    printf("world: %d frames (%d compiling shaders) in %.1f s; last view %.1f ms, %d triangles\n", i, i - after, real_seconds() - t0, ed.view_ms, ed.view_triangles);
    printf("terrain: %d chunks drawn of %d, %d foliage batches, %ld instances, %s, %.1f MB\n", st.chunks_drawn, st.chunks, st.foliage_batches,
           st.foliage_instances, st.pending ? "still building" : "built", st.memory / 1048576.0);
    printf("draws: %d culled by frustum, %d by occluders, %d meshes left out\n", ed.fgl_stats.culled_frustum, ed.fgl_stats.culled_occlusion,
           ed.fgl_stats.meshes_dropped);
    if (!platform::headless_save_png(argv[3])) return 1;
    printf("wrote %s\n", argv[3]);
    ed.shutdown();
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--world")) return world_shot(argc, argv);
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
    // A real crate texture (planks with a frame), so FarcadeGL draws it.
    {
        enum { T = 128 };
        static uint8_t px[T * T * 3];
        for (int y = 0; y < T; y++)
            for (int x = 0; x < T; x++) {
                bool frame = x < 10 || x >= T - 10 || y < 10 || y >= T - 10;
                bool brace = abs((x - y)) < 7 || abs((x + y) - (T - 1)) < 7;
                int plank = (y / 27) & 1;
                float grain = 0.88f + 0.12f * fsin((float)x * 0.21f + fsin((float)y * 0.07f) * 3.0f);
                float shade = frame || brace ? 0.78f : (plank ? 0.95f : 1.0f) * grain;
                if (!frame && !brace && y % 27 == 0) shade *= 0.55f;          // gaps between planks
                uint8_t* c = &px[(y * T + x) * 3];
                c[0] = (uint8_t)(214 * shade);
                c[1] = (uint8_t)(150 * shade);
                c[2] = (uint8_t)(84 * shade);
            }
        char path[512];
        snprintf(path, sizeof(path), "%s/assets", project);
        mkdir(path, 0755);
        snprintf(path, sizeof(path), "%s/assets/crate.png", project);
        stbi_write_png(path, T, T, 3, px, T * 3);
        snprintf(path, sizeof(path), "%s/crate.png", project);      // also next to the scenes, for the Files panel
        stbi_write_png(path, T, T, 3, px, T * 3);
    }
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

    // The viewport fullscreen (its button under the orientation gizmo).
    ed.set_view_fullscreen(true);
    for (int i = 0; i < 3; i++) { platform::headless_set_time((11 + i) / 60.0); ed.frame(); }
    snprintf(path, sizeof(path), "%s/editor-viewport-fullscreen.png", out);
    if (!platform::headless_save_png(path)) return 1;
    printf("wrote %s\n", path);
    ed.set_view_fullscreen(false);
    for (int i = 0; i < 2; i++) { platform::headless_set_time((12 + i) / 60.0); ed.frame(); }

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
    // Groups and a selection of two: the Inspector's Groups row, edits applying to both.
    {
        ObjectId ball = kNoObject, pil = kNoObject;
        for (int i = 0; i < ed.scene.count(); i++) {
            if (!strcmp(ed.scene.at(i).name, "Ball")) ball = ed.scene.at(i).id;
            if (!strcmp(ed.scene.at(i).name, "Pillar")) pil = ed.scene.at(i).id;
        }
        char saved_b[256] = {}, saved_p[256] = {};
        if (ball && pil) {
            memcpy(saved_b, ed.scene.get(ball)->groups, sizeof saved_b);
            memcpy(saved_p, ed.scene.get(pil)->groups, sizeof saved_p);
            snprintf(ed.scene.get(ball)->groups, 256, "%s", "props;round;pickup");
            snprintf(ed.scene.get(pil)->groups, 256, "%s", "props");
            ed.scene.select_only(pil);
            ed.scene.select_add(ball);
            ImGuiWindow* iw = ImGui::FindWindowByName("###Inspector");
            for (int i = 0; i < 3; i++) { if (iw) ImGui::SetScrollY(iw, 0); ed.frame(); }
            snprintf(path, sizeof(path), "%s/editor-groups.png", out);
            if (!platform::headless_save_png(path)) return 1;
            printf("wrote %s\n", path);
            memcpy(ed.scene.get(ball)->groups, saved_b, sizeof saved_b);
            memcpy(ed.scene.get(pil)->groups, saved_p, sizeof saved_p);
        }
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
        ed.settings.graphics.feature_level = gfx::FeatureLevelMode::GpuTier;   // Auto keeps the CPU at FL0
        for (int i = 0; i < 3; i++) ed.frame();
        printf("FarcadeGL viewport, GPU tier: %.1f ms\n", ed.view_ms);
        snprintf(path, sizeof(path), "%s/editor-farcadegl-ultra.png", out);
        if (!platform::headless_save_png(path)) return 1;
        printf("wrote %s\n", path);
        // The Profiler tab next to Console and Files: the frames and where the last one went.
        for (int i = 0; i < 14; i++) ed.frame();        // the settle frames (TAA / AO) too, for the graph
        ImGui::SetWindowFocus("###Profiler");
        for (int i = 0; i < 2; i++) ed.frame();
        snprintf(path, sizeof(path), "%s/editor-profiler.png", out);
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
        // A screen shader on the current camera (the CRT sample), seen in Play with the camera in the Inspector.
        {
            char shader_path[600];
            snprintf(shader_path, sizeof(shader_path), "%s/assets/crt.fshader", ed.project_dir);
            size_t sn = 0;
            void* src = platform::read_file("assets/samples/screen/crt.fshader", &sn);
            if (src) { platform::write_file(shader_path, src, sn); free(src); }
            for (int i = 0; i < ed.scene.count(); i++) {
                Object& o = ed.scene.at(i);
                if (!strcmp(o.name, "Main Camera") && o.modules.has(ModuleKind::Camera)) {
                    snprintf(o.modules.camera.shader1, kPathMax, "assets/crt.fshader");
                    o.modules.camera.shader_params = Vec3(1.0f, 0.0f, 0.0f);
                    ed.scene.select_only(o.id);
                }
            }
            ed.play();
            for (int i = 0; i < 4; i++) ed.frame();
            snprintf(path, sizeof(path), "%s/editor-screen-shader.png", out);
            if (!platform::headless_save_png(path)) return 1;
            printf("wrote %s\n", path);
            ed.stop();
            for (int i = 0; i < ed.scene.count(); i++)
                if (ed.scene.at(i).modules.has(ModuleKind::Camera)) ed.scene.at(i).modules.camera.shader1[0] = 0;
            ed.scene.select_only(kNoObject);
        }
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
    // FarcadeGL compiling its shaders (the bar over the viewport), when asked for.
    if (getenv("FE_SHOT_COMPILE")) {
        ed.show_settings = false;
        ed.view_renderer = Editor::ViewRenderer::FarcadeGL;
        ed.settings.graphics.allow_gpu_effects_on_cpu = true;
        ed.fgl_compile_budget = 1e-6;
        ed.fgl_restart = true;                              // a fresh renderer: every shader again
        ed.frame();                                         // compiles one shader and stops: the bar shows
        snprintf(path, sizeof(path), "%s/editor-compiling.png", out);
        if (!platform::headless_save_png(path)) return 1;
        printf("wrote %s (%d shaders so far)\n", path, ed.fgl_compiled);
        ed.fgl_compile_budget = 25.0;
    }
    // The file picker (Create > Import model...), when asked for.
    if (getenv("FE_SHOT_PICKER")) {
        ed.show_settings = false;
        ed.open_file_dialog(Editor::PickFor::ImportModel, Editor::PickRoot, "Import model", false, ed.project_dir,
                            ".gltf;.glb;.obj;.mof");
        for (int i = 0; i < 4; i++) ed.frame();
        snprintf(path, sizeof(path), "%s/editor-picker.png", out);
        if (!platform::headless_save_png(path)) return 1;
        printf("wrote %s\n", path);
    }

    // Terrain: hills, a hole, grass blades as foliage, a few objects; the CPU preview, FarcadeGL and the GPU tier.
    {
        ed.show_settings = false;
        ed.new_scene();
        ed.settings.graphics.apply_preset(gfx::GraphicsPreset::High);
        ed.settings.graphics.allow_gpu_effects_on_cpu = false;
        ed.settings.graphics.feature_level = gfx::FeatureLevelMode::Auto;
        // A tuft of three crossed blades as an .obj with a green material, converted to .mof.
        char objp[700], mofp[700], msg[256];
        snprintf(objp, sizeof objp, "%s/assets/grass.mtl", project);
        FILE* f = fopen(objp, "wb");
        if (f) { fputs("newmtl blade\nKd 0.32 0.55 0.18\n", f); fclose(f); }
        snprintf(objp, sizeof objp, "%s/assets/grass.obj", project);
        f = fopen(objp, "wb");
        if (f) {
            fputs("mtllib grass.mtl\nusemtl blade\n", f);
            int v = 0;
            for (int b = 0; b < 3; b++) {
                float a = (float)b * 1.047f, c = fcos(a) * 0.06f, s2 = fsin(a) * 0.06f;
                float tx = fcos(a + 0.4f) * 0.08f, tz = fsin(a + 0.4f) * 0.08f;
                fprintf(f, "v %f 0 %f\nv %f 0 %f\nv %f 0.55 %f\n", -c, -s2, c, s2, tx, tz);
                fprintf(f, "vn 0 0.3 1\n");
                fprintf(f, "f %d//%d %d//%d %d//%d\nf %d//%d %d//%d %d//%d\n", v + 1, b + 1, v + 2, b + 1, v + 3, b + 1, v + 2, b + 1, v + 1, b + 1, v + 3, b + 1);
                v += 3;
            }
            fclose(f);
        }
        bool grass = model_convert_file(objp, mofp, sizeof mofp, msg, sizeof msg);
        if (!grass) printf("grass model: %s\n", msg);
        ObjectId sun = ed.create_light(LightType::Directional, Vec3(0, 20, 0));
        if (Object* so = ed.scene.get(sun)) so->rotation = Quat::euler_deg(Vec3(-40, 35, 0));
        ed.create_kind("Sky", ObjectKind::Sky, Vec3(0, 30, 0));
        ed.create_kind("Sky Light", ObjectKind::SkyLight, Vec3(0, 30, 0));
        ObjectId t = ed.create_kind("Terrain", ObjectKind::Terrain, Vec3(0, 0, 0));
        Object* to = ed.scene.get(t);
        TerrainData* td = to ? terrain_data(*to) : nullptr;
        if (td) {
            td->begin_stroke();
            TerrainBrushOp op;
            uint32_t seed = 12345;
            auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / 16777216.0f; };
            for (int i = 0; i < 70; i++) {
                op.brush = i % 5 == 4 ? TerrainBrush::Lower : TerrainBrush::Raise;
                op.x = 20 + rnd() * 216; op.z = 20 + rnd() * 216;
                op.radius = 10 + rnd() * 30; op.hardness = 0.1f; op.amount = 1.5f + rnd() * 6.0f;
                td->apply(op);
            }
            op.brush = TerrainBrush::Smooth; op.rate = 1.0f; op.radius = 300; op.x = 128; op.z = 128;
            for (int i = 0; i < 3; i++) td->apply(op);
            op.brush = TerrainBrush::Delete; op.x = 150; op.z = 100; op.radius = 5;
            td->apply(op);
            if (grass) {
                snprintf(to->modules.terrain.foliage[0].model, kPathMax, "%s", mofp);
                to->modules.terrain.foliage[0].density = 12.0f;
                to->modules.terrain.foliage[0].distance = 45.0f;
                to->modules.terrain.foliage[0].scale_min = 0.6f;
                to->modules.terrain.foliage[0].scale_max = 1.3f;
                op.brush = TerrainBrush::Paint; op.layer = 0; op.target = 1.0f; op.rate = 1.0f; op.hardness = 0.6f;
                op.x = 128; op.z = 150; op.radius = 60;
                td->apply(op);
            }
            td->end_stroke();
            to->modules.terrain.revision = td->revision;
            terrain_mark_dirty(td);
        }
        // A few boxes on the ground (some hidden behind hills from the view).
        for (int i = 0; i < 6; i++) {
            float x = -30.0f + (float)i * 12.0f, z = 10.0f - (float)(i % 3) * 25.0f;
            float y = td ? td->height_at(x + 128, z + 128) : 0;
            ObjectId c = ed.create_object("Crate", MeshKind::Cube, Vec3(x, y + 0.5f, z));
            if (Object* co = ed.scene.get(c)) co->color = Vec3(0.75f, 0.55f, 0.35f);
        }
        ed.scene.select_only(t);
        ed.terrain_tool = Editor::TerrainTool::Raise;
        ed.camera.target = Vec3(0, td ? td->height_at(128, 150) : 0, 22);
        ed.camera.yaw = 20;
        ed.camera.pitch = 18;
        ed.camera.distance = 28;
        ed.view_renderer = Editor::ViewRenderer::CpuPreview;
        for (int i = 0; i < 4; i++) ed.frame();
        snprintf(path, sizeof(path), "%s/editor-terrain-cpu.png", out);
        if (!platform::headless_save_png(path)) return 1;
        printf("wrote %s\n", path);
        ed.view_renderer = Editor::ViewRenderer::FarcadeGL;
        for (int i = 0; i < 8; i++) ed.frame();
        printf("terrain: %d chunks drawn, %d batches, %ld foliage instances, %d culled by occluders\n", ed.terrain_stats_.chunks_drawn,
               ed.terrain_stats_.foliage_batches, ed.terrain_stats_.foliage_instances, ed.fgl_stats.culled_occlusion);
        snprintf(path, sizeof(path), "%s/editor-terrain.png", out);
        if (!platform::headless_save_png(path)) return 1;
        printf("wrote %s\n", path);
        ed.settings.graphics.apply_preset(gfx::GraphicsPreset::Ultra);
        ed.settings.graphics.allow_gpu_effects_on_cpu = true;
        ed.settings.graphics.feature_level = gfx::FeatureLevelMode::GpuTier;
        ed.terrain_tool = Editor::TerrainTool::None;
        for (int i = 0; i < 16; i++) ed.frame();
        snprintf(path, sizeof(path), "%s/editor-terrain-ultra.png", out);
        if (!platform::headless_save_png(path)) return 1;
        printf("wrote %s\n", path);
        // Clouds over the hills, looking up from the ground.
        ObjectId cl = ed.create_kind("Clouds", ObjectKind::Clouds, Vec3(0, 6, 0));
        if (Object* co = ed.scene.get(cl)) { co->modules.clouds.coverage = 0.6f; co->modules.clouds.quality = CloudQuality::Ultra; }
        ed.scene.select_only(cl);
        ed.camera.target = Vec3(0, (td ? td->height_at(128, 150) : 0) + 12.0f, 22);
        ed.camera.pitch = -14;
        ed.camera.distance = 10;
        for (int i = 0; i < 16; i++) ed.frame();
        printf("clouds: quality %d drawn\n", ed.fgl_stats.cloud_quality);
        snprintf(path, sizeof(path), "%s/editor-clouds.png", out);
        if (!platform::headless_save_png(path)) return 1;
        printf("wrote %s\n", path);
    }
    ed.shutdown();
    return 0;
}
