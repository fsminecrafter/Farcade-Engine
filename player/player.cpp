#include "player.h"

#include "audio/audio.h"
#include "gfx/renderer.h"
#include "gfx/particles.h"
#include "script/runtime.h"
#include "core/vfs.h"
#include "platform/platform.h"
#include "graphics_text.h"
#include "model_cache.h"
#include "play_session.h"
#include "fx/fx.h"
#include "project.h"
#include "render_bridge.h"
#include "scene.h"
#include "scene_io.h"
#include "swraster.h"
#if FE_HOST
#include "platform/headless.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe::player {

namespace {

struct State {
    Options opt;
    Result* res;
};

void log_line(PlayLog level, const char* text, void* user) {
    State* st = (State*)user;
    st->res->script_lines++;
    if (level == PlayLog::Info) printf("%s\n", text);
    else printf("%s: %s\n", level == PlayLog::Error ? "error" : "warning", text);
}

void* read_file(const char* path, size_t* size, void*) { return vfs::read(path, size); }

void set_error(Result* r, const char* fmt, const char* a = "", const char* b = "") {
    snprintf(r->error, sizeof r->error, fmt, a, b);
}

void setup_vfs(const Options& opt) {
    vfs::Disk disk;
    disk.read_file = platform::read_file;
    disk.file_exists = platform::file_exists;
    vfs::set_disk(disk);
    vfs::FileAccess fa;
    fa.open = platform::open_read;
    fa.read_at = platform::read_at;
    fa.close = platform::close_file;
    vfs::set_file_access(fa);
}

uint32_t checksum_of(const Framebuffer& fb) {
    uint32_t h = 2166136261u;
    for (int y = 0; y < fb.height; y++)
        for (int x = 0; x < fb.width; x++) {
            uint32_t p = fb.pixels[(size_t)y * fb.pitch + x] & 0xFFFFFF;
            h = (h ^ p) * 16777619u;
        }
    return h;
}

// The RGBA image (byte order R, G, B, A) into the framebuffer (0x00RRGGBB),
// stretched by `div` when the image is smaller than the screen.
void blit(const uint32_t* rgba, int rw, int rh, Framebuffer& fb) {
    for (int y = 0; y < fb.height; y++) {
        int sy = y * rh / fb.height;
        const uint32_t* src = rgba + (size_t)sy * rw;
        uint32_t* dst = fb.pixels + (size_t)y * fb.pitch;
        if (rw == fb.width) {
            for (int x = 0; x < fb.width; x++) {
                uint32_t p = src[x];
                dst[x] = (p & 0xFF) << 16 | (p & 0xFF00) | ((p >> 16) & 0xFF);
            }
        } else {
            for (int x = 0; x < fb.width; x++) {
                uint32_t p = src[x * rw / fb.width];
                dst[x] = (p & 0xFF) << 16 | (p & 0xFF00) | ((p >> 16) & 0xFF);
            }
        }
    }
}

}  // namespace

int run(const Options& opt, Result* result) {
    Result local;
    Result* res = result ? result : &local;
    *res = Result();
    State st;
    st.opt = opt;
    st.res = res;

    setup_vfs(opt);
    if (opt.project_dir && *opt.project_dir) {
        vfs::set_root(opt.project_dir);
        vfs::set_disk_fallback(true);
    } else {
        vfs::set_disk_fallback(false);          // a game only sees its package
    }
    if (opt.pak_path && *opt.pak_path) {
        const char* why = "";
        if (!vfs::mount_pak_file(opt.pak_path, &why)) {
            set_error(res, "cannot open the game package %s: %s", opt.pak_path, why);
            return 1;
        }
    }

    // project.fe (optional) and the entry scene
    {
        size_t n = 0;
        char* text = (char*)vfs::read("project.fe", &n);
        if (text) {
            if (!project_parse(text, n, &res->project)) set_error(res, "project.fe is not a project file%s%s");
            free(text);
            if (res->error[0]) return 1;
        }
    }
    const char* scene_path = opt.scene && *opt.scene ? opt.scene : res->project.entry_scene;
    static Scene scene;
    {
        size_t n = 0;
        char* text = (char*)vfs::read(scene_path, &n);
        if (!text) { set_error(res, "cannot read the scene %s%s", scene_path); return 1; }
        SceneLoadReport rep;
        bool ok = load_scene_text(scene, text, n, &rep);
        free(text);
        if (!ok) { set_error(res, "cannot load the scene %s: %s", scene_path, rep.error); return 1; }
        res->objects = rep.objects;
        // Script inputs follow the script files, like the editor does when Play starts
        // (a file may have gained an @prop since the scene was saved).
        for (int i = 0; i < scene.count(); i++) {
            Object& o = scene.at(i);
            for (int k = 0; k < o.script_count; k++) {
                size_t sn = 0;
                char* src = (char*)vfs::read(o.scripts[k].path, &sn);
                if (!src) continue;
                ScriptProp declared[kMaxProps];
                int bad = 0;
                int nd = parse_script_props(src, (uint32_t)sn, declared, kMaxProps, &bad);
                merge_script_props(&o.scripts[k], declared, nd);
                free(src);
            }
        }
    }

    platform::set_window_options(res->project.name, res->project.window_width, res->project.window_height);
    if (!platform::init(0, 0)) { set_error(res, "cannot open the screen (in use?)%s%s"); return 1; }
    Framebuffer& fb = platform::framebuffer();
    for (int y = 0; y < fb.height; y++) memset(fb.pixels + (size_t)y * fb.pitch, 0, (size_t)fb.width * 4);

    AudioDevice* device = nullptr;
    if (opt.audio) {
        device = new AudioDevice();
        if (!device->init()) printf("warning: audio: no device\n");
    }
    gfx::GraphicsSettings graphics;
    // Graphics settings exported with the game (graphics.cfg), applied before the first frame;
    // scripts may change them (fe.graphics), the loop applies them every frame.
    {
        size_t gn = 0;
        char* g = (char*)vfs::read("graphics.cfg", &gn);
        if (g) {
            if (project_graphics_from_text(g, gn, &graphics) && !opt.quiet) printf("FE PLAYER graphics settings from graphics.cfg (preset %d)\n", (int)graphics.preset);
            free(g);
        }
    }
    PlayHost host;
    host.user = &st;
    host.log = log_line;
    host.read_file = read_file;
    host.graphics = &graphics;
    static PlaySession session;
    if (!session.start(scene, host, device)) {
        set_error(res, "the scene could not start%s%s");
        platform::shutdown();
        return 1;
    }
    res->scripts = session.script_count();
    for (int i = 0; i < scene.count(); i++)
        if (Scene::model_for(scene.at(i))) res->models++;

    static gfx::Renderer fgl;
    static RenderBridge bridge;
    int div = opt.render_div < 1 ? 1 : opt.render_div;
    bool fixed = opt.frames > 0;
    double last = platform::time_seconds();
    bool quit = false;
    bool render_ok = true;
    bool compiling = false;                 // FarcadeGL is compiling its shaders a few per frame
    bool have_image = false;                // a frame was drawn (else the loading screen is on black)
    double start = platform::time_seconds();

    while (!quit) {
        platform::Event e;
        while (platform::poll(e)) {
            if (e.type == platform::EventType::Quit) quit = true;
            else if (e.type == platform::EventType::Key && e.down && e.key == platform::KEY_ESCAPE) quit = true;
            else session.forward_event(e);
        }
        if (quit) break;
        double now = platform::time_seconds();
        float dt = fixed ? 1.0f / 60.0f : (float)(now - last);
        if (dt > 0.1f) dt = 0.1f;
        if (dt < 0) dt = 0;
        last = now;
        session.set_quality(fx_quality_of(graphics));       // fe.graphics may change the effects options
        session.step(dt);

        Framebuffer& f = platform::framebuffer();
        int rw = f.width / div, rh = f.height / div;
        if (rw < 16) rw = 16;
        if (rh < 16) rh = 16;
        if (render_ok) {
            bool first = !fgl.ready();
            bool ok = fgl.ready() ? ((fgl.width() == rw && fgl.height() == rh) || fgl.resize(rw, rh)) : fgl.init(rw, rh);
            // Shaders are compiled for ~30 ms a frame, with a loading screen,
            // instead of one long stop before the first picture. A fixed-step
            // run (tests, --frames) compiles at once, so frame N is frame N.
            if (first && ok && !fixed) fgl.set_compile_budget(30.0);
            if (ok) {
                fgl.set_settings(graphics);
                Mat4 view, proj;
                Vec3 eye;
                float zn = 0.1f, zf = 500.0f;
                float aspect = (float)rw / (float)rh;
                ObjectId cam = scene.current(ModuleKind::Camera);
                if (!(cam && RenderBridge::camera_view(scene, cam, aspect, &view, &proj, &eye, &zn, &zf))) {
                    // No current camera: a view from above and behind the origin.
                    eye = Vec3(6, 5, 8);
                    view = Mat4::look_at(eye, Vec3(0, 0.5f, 0), Vec3(0, 1, 0));
                    proj = Mat4::perspective(radians(50.0f), aspect, 0.1f, 500.0f);
                }
                bridge.build(scene, view, proj, eye, zn, zf);
                static ModelAssetReader assets{ vfs::read_cb, nullptr };
                bridge.scene.read_asset = model_asset_reader;
                bridge.scene.asset_user = &assets;
                // Particles, billboards and text over the image (engine/fx).
                if (FxWorld* fx = session.fx()) {
                    FxWorld::Frame fr = fx->collect(eye);
                    bridge.scene.particles = fr.batches;
                    bridge.scene.particle_batch_count = fr.batch_count;
                    bridge.scene.labels = fr.labels;
                    bridge.scene.label_count = fr.label_count;
                    bridge.scene.particles_depth_test = fx->quality.depth_test;
                }
                ok = fgl.render(bridge.scene);
            }
            if (ok) {
                blit(fgl.pixels(), rw, rh, f);
                have_image = true;
                if (compiling && !opt.quiet) printf("FE PLAYER shaders ready (%d compiled)\n", fgl.shaders_compiled());
                compiling = false;
            } else if (fgl.compiling()) {
                compiling = true;               // not an error: the next frame goes on
            } else {
                printf("error: FarcadeGL: %s\n", fgl.error());
                render_ok = false;
            }
        }
        // The loading screen: FarcadeGL's shaders, and what the game says
        // about itself (fe.status).
        {
            const GameStatus* gs = session.scripts() ? &session.scripts()->status() : nullptr;
            bool game = gs && gs->shown;
            if (compiling || game) {
                if (!have_image)
                    for (int y = 0; y < f.height; y++) memset(f.pixels + (size_t)y * f.pitch, 0, (size_t)f.width * 4);
                char line[160], sub[96];
                float progress = -1.0f;
                if (game) {
                    snprintf(line, sizeof line, "%s", gs->text);
                    progress = gs->progress;
                    if (compiling) snprintf(sub, sizeof sub, "Compiling shaders (%d)", fgl.shaders_compiled());
                    else sub[0] = 0;
                } else {
                    snprintf(line, sizeof line, "Compiling shaders (%d)", fgl.shaders_compiled());
                    snprintf(sub, sizeof sub, "%s", "Only the first time: they are kept in the shader cache");
                }
                gfx::draw_status_box(f.pixels, f.width, f.height, f.pitch, line, sub, progress,
                                     platform::time_seconds() - start);
            }
        }
        platform::present();
        res->frames++;
        if (res->frames == 1 && !opt.quiet) printf("FE PLAYER READY %s (%dx%d, %d objects, %d scripts)\n", res->project.name, f.width, f.height, res->objects, res->scripts);
        if (opt.frames > 0 && res->frames >= opt.frames) break;
        if (!fixed) {
            // ~60 frames a second; the wait also lets the input arrive.
            double spent = platform::time_seconds() - now;
            if (spent < 0.016) platform::wait_event((int)((0.016 - spent) * 1000.0));
        }
    }

    Framebuffer& f = platform::framebuffer();
    res->checksum = checksum_of(f);
#if FE_HOST
    if (opt.png_path && *opt.png_path && !platform::headless_save_png(opt.png_path)) printf("warning: cannot write %s\n", opt.png_path);
#endif
    session.stop();
    fgl.shutdown();
    if (device) { device->shutdown(); delete device; }
    platform::shutdown();
    vfs::unmount_all();
    if (!opt.quiet) printf("FE PLAYER DONE frames=%ld checksum=%08x objects=%d scripts=%d models=%d\n", res->frames, (unsigned)res->checksum, res->objects, res->scripts, res->models);
    return 0;
}

}  // namespace fe::player
