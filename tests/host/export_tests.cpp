// Tests of the game package (.fepak), the VFS, project settings and export:
// exports a demo project (a Lua script, a model, a texture, a sound) to a temp
// folder, then runs the headless player on the result.
//
// Run from the repository root (make test does): the test starts
// build/host/bin/feplayer and build/host/bin/fe-export.
#include "core/pak.h"
#include "core/vfs.h"
#include "export.h"
#include "graphics_text.h"
#include "platform/platform.h"
#include "project.h"
#include "scene.h"
#include "scene_io.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace fe;

static int g_checks = 0, g_failures = 0;
#define CHECK(cond, ...) \
    do { g_checks++; if (!(cond)) { g_failures++; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static char g_tmp[256];

static void put(const char* rel, const void* data, size_t n) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_tmp, rel);
    // parents
    for (char* p = path + strlen(g_tmp) + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(path, 0755); *p = '/'; }
    CHECK(platform::write_file(path, data, n));
}

static void put_str(const char* rel, const char* text) { put(rel, text, strlen(text)); }

// Runs a command and returns what it printed (malloc'd).
static char* capture(const char* cmd) {
    FILE* f = popen(cmd, "r");
    if (!f) return nullptr;
    size_t cap = 1 << 16, n = 0;
    char* out = (char*)malloc(cap);
    size_t r;
    while ((r = fread(out + n, 1, cap - n - 1, f)) > 0) {
        n += r;
        if (n + 1 >= cap) { cap *= 2; out = (char*)realloc(out, cap); }
    }
    out[n] = 0;
    pclose(f);
    return out;
}

static unsigned find_checksum(const char* out) {
    const char* p = strstr(out, "checksum=");
    return p ? (unsigned)strtoul(p + 9, nullptr, 16) : 0;
}

// ---------------------------------------------------------------- pak

static void test_pak() {
    PakWriter w;
    CHECK(w.add("b/two.txt", "second", 6));
    CHECK(w.add("a/one.txt", "first file", 10));
    CHECK(w.add("empty", "", 0));
    size_t big_n = 200 * 1024;
    uint8_t* big = (uint8_t*)malloc(big_n);
    for (size_t i = 0; i < big_n; i++) big[i] = (uint8_t)(i * 7);
    CHECK(w.add("models/big.bin", big, big_n));
    CHECK(!w.add("/absolute", "x", 1));
    CHECK(w.add("a/one.txt", "first file, replaced", 20));
    CHECK(w.count() == 4);
    size_t size = 0;
    uint8_t* image = w.build(&size);
    CHECK(image && size > big_n);
    CHECK(memcmp(image, "FEPAK", 5) == 0);

    PakReader r;
    CHECK(r.open(pak_memory_source(image, size)));
    CHECK(r.count() == 4);
    size_t n = 0;
    char* t = (char*)r.read("a/one.txt", &n);
    CHECK(t && n == 20 && strcmp(t, "first file, replaced") == 0);
    free(t);
    CHECK(r.read("missing", &n) == nullptr);
    PakEntryInfo e;
    CHECK(r.find("models/big.bin", &e));
    CHECK(e.offset % kPakChunkAlign == 0);          // big files start on a 64 KiB boundary
    CHECK(e.size == big_n);
    uint8_t* b = (uint8_t*)r.read(e, &n);
    CHECK(b && n == big_n && memcmp(b, big, big_n) == 0);
    free(b);
    CHECK(r.find("empty", &e) && e.size == 0);
    // the index is sorted: entries come in path order
    PakEntryInfo prev, cur;
    CHECK(r.entry(0, &prev));
    for (uint32_t i = 1; i < r.count(); i++) {
        CHECK(r.entry(i, &cur));
        CHECK(memcmp(prev.path, cur.path, prev.path_len < cur.path_len ? prev.path_len : cur.path_len) <= 0);
        prev = cur;
    }
    r.close();

    // damage: a flipped data byte is found by the entry checksum, a flipped
    // index byte by the index checksum, a cut file by the size.
    PakEntryInfo one;
    PakReader r2;
    CHECK(r2.open(pak_memory_source(image, size)));
    CHECK(r2.find("a/one.txt", &one));
    uint64_t at = one.offset + 3;
    r2.close();
    image[at] ^= 0x40;
    CHECK(r2.open(pak_memory_source(image, size)));
    CHECK(r2.read("a/one.txt", &n) == nullptr);
    CHECK(r2.read("b/two.txt", &n) != nullptr);     // others are fine (leaks the block: freed below)
    r2.close();
    image[at] ^= 0x40;
    uint64_t index_off = 0;
    memcpy(&index_off, image + 16, 8);
    image[index_off + 5] ^= 1;
    CHECK(!r2.open(pak_memory_source(image, size)));
    image[index_off + 5] ^= 1;
    CHECK(!r2.open(pak_memory_source(image, size - 1)));
    image[0] = 'X';
    CHECK(!r2.open(pak_memory_source(image, size)));
    free(big);
    free(image);
}

static void test_vfs() {
    PakWriter w;
    w.add("scripts/a.lua", "return 1", 8);
    size_t size = 0;
    uint8_t* image = w.build(&size);
    vfs::Disk d;
    d.read_file = platform::read_file;
    d.file_exists = platform::file_exists;
    vfs::set_disk(d);
    vfs::unmount_all();
    vfs::set_root(g_tmp);
    vfs::set_disk_fallback(true);
    put_str("loose.txt", "from disk");
    CHECK(vfs::mount_pak_memory(image, size));
    size_t n = 0;
    char* a = (char*)vfs::read("scripts/a.lua", &n);
    CHECK(a && n == 8 && strcmp(a, "return 1") == 0);
    free(a);
    char full[400];
    snprintf(full, sizeof full, "%s/scripts\\a.lua", g_tmp);            // a full path with a backslash
    a = (char*)vfs::read(full, &n);
    CHECK(a && n == 8);
    free(a);
    char* l = (char*)vfs::read("./loose.txt", &n);
    CHECK(l && n == 9 && strcmp(l, "from disk") == 0);
    free(l);
    CHECK(vfs::exists("loose.txt") && vfs::exists("scripts/a.lua") && !vfs::exists("nope.txt"));
    vfs::set_disk_fallback(false);
    CHECK(vfs::read("loose.txt", &n) == nullptr);                       // a game only sees its package
    CHECK(!vfs::exists("loose.txt"));
    vfs::unmount_all();
    vfs::set_disk_fallback(true);
    free(image);
}

static void test_graphics_text() {
    gfx::GraphicsSettings g;
    g.apply_preset(gfx::GraphicsPreset::Ultra);
    g.render_scale = 0.5f;
    size_t n = 0;
    char* t = project_graphics_to_text(g, &n);
    gfx::GraphicsSettings h;
    CHECK(t && project_graphics_from_text(t, n, &h));
    CHECK(h.preset == gfx::GraphicsPreset::Ultra && h.render_scale == 0.5f && h.shadow_resolution == 4096 && h.local_shadow_lights == 4);
    free(t);
    // Unknown names and bad values are skipped; the rest still loads.
    const char* odd = "future_field 3\nbloom off\nshadow_resolution 99999\n";
    gfx::GraphicsSettings k;
    CHECK(project_graphics_from_text(odd, strlen(odd), &k) && !k.bloom && k.shadow_resolution == gfx::GraphicsSettings().shadow_resolution);
    CHECK(!project_graphics_from_text("nothing known", 13, &k));
}

static void test_project() {
    ProjectSettings p;
    snprintf(p.name, sizeof p.name, "My \"Great\" Game");
    snprintf(p.entry_scene, sizeof p.entry_scene, "scenes/start.fscene");
    p.window_width = 800;
    p.window_height = 600;
    snprintf(p.icon, sizeof p.icon, "art/icon.bmp");
    size_t n = 0;
    char* text = project_text(p, &n);
    CHECK(text && strncmp(text, "farcade-project 1", 17) == 0);
    ProjectSettings q;
    CHECK(project_parse(text, n, &q));
    CHECK(strcmp(q.name, p.name) == 0 && strcmp(q.entry_scene, p.entry_scene) == 0 && q.window_width == 800 && q.window_height == 600);
    CHECK(strcmp(q.icon, "art/icon.bmp") == 0);
    free(text);
    const char* newer = "farcade-project 7\nfuture_key 1\nname Plain Name\nwindow_width 99999\n";
    ProjectSettings r;
    CHECK(project_parse(newer, strlen(newer), &r));
    CHECK(strcmp(r.name, "Plain Name") == 0 && r.window_width == 1280);   // out of range keeps the default
    CHECK(!project_parse("hello", 5, &r));
    char f[64];
    project_file_name("My Game: 2!", f, sizeof f);
    CHECK(strcmp(f, "My_Game__2_") == 0);
}

// ---------------------------------------------------------------- export

static const char* kSpinLua =
    "-- @prop speed number = 90 range 0 360\n"
    "function on_start(self)\n"
    "  print('FE GAME start ' .. self.object.name)\n"
    "  self.n = 0\n"
    "  self.angle = 0\n"
    "end\n"
    "function on_update(self, dt)\n"
    "  self.n = self.n + 1\n"
    "  self.angle = self.angle + self.speed * dt\n"
    "  self.object.rotation = fe.vec3(20, self.angle, 0)\n"
    "  if self.n == 15 then print('FE GAME spin ' .. self.n) end\n"
    "end\n";

static void build_demo_project() {
    put_str("scripts/spin.lua", kSpinLua);
    put_str("scripts/other.lua", "function on_start(self) print('FE GAME other') end\n");
    // a model (the repository's sample), a texture (any bytes: the player only loads it if used), a sound
    size_t n = 0;
    void* mof = platform::read_file("assets/samples/cube_textured.mof", &n);
    CHECK(mof != nullptr);
    if (mof) { put("models/cube.mof", mof, n); free(mof); }
    {
        // a 4x4 24-bit BMP
        unsigned char bmp[54 + 4 * 4 * 3] = { 'B', 'M', 54 + 48, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 1, 0, 24, 0 };
        bmp[34] = 48;
        for (int i = 0; i < 16; i++) { bmp[54 + i * 3] = 90; bmp[54 + i * 3 + 1] = (unsigned char)(255 - i * 8); bmp[54 + i * 3 + 2] = (unsigned char)(i * 16); }
        put("textures/stone.bmp", bmp, sizeof bmp);
    }
    put_str("audio/beep.wav", "not really a wav");
    put_str("icon.bmp", "BM-not-a-bitmap");
    put_str("graphics.cfg", "farcade-graphics 1\npreset low\nshadows off\nbloom off\n");
    put_str("unused/readme.txt", "never referenced");

    Scene s;
    ObjectId cube = s.create("Spinner", MeshKind::Cube);
    s.get(cube)->color = Vec3(0.9f, 0.4f, 0.2f);
    s.add_script(cube, "scripts/spin.lua");
    ObjectId ground = s.create("Ground", MeshKind::Plane);
    s.get(ground)->scale = Vec3(6, 1, 6);
    s.get(ground)->position = Vec3(0, -1, 0);
    snprintf(s.get(ground)->texture_path, 256, "textures/stone.bmp");
    ObjectId statue = s.create("Statue", MeshKind::Model);
    snprintf(s.get(statue)->model_path, 256, "models/cube.mof");
    s.get(statue)->position = Vec3(2.5f, 0, 0);
    s.add_module(statue, ModuleKind::AudioSampler);
    snprintf(s.get(statue)->modules.audio_sampler.path, kPathMax, "audio/beep.wav");
    ObjectId cam = s.create("Camera", MeshKind::Cube);
    s.set_kind(cam, ObjectKind::Camera);
    s.get(cam)->position = Vec3(0, 1.5f, 6);
    ObjectId sun = s.create("Sun", MeshKind::Cube);
    s.set_kind(sun, ObjectKind::Empty);
    s.add_module(sun, ModuleKind::Light);
    s.get(sun)->rotation = Quat::euler_deg(Vec3(-50, 30, 0));
    ObjectId sky = s.create("Sky", MeshKind::Cube);
    s.set_kind(sky, ObjectKind::Sky);
    size_t sn = 0;
    char* text = save_scene_text(s, g_tmp, &sn);
    put("scenes/main.fscene", text, sn);
    free(text);
    // a second scene with another script
    Scene s2;
    ObjectId o2 = s2.create("Other", MeshKind::Sphere);
    s2.add_script(o2, "scripts/other.lua");
    text = save_scene_text(s2, g_tmp, &sn);
    put("scenes/second.fscene", text, sn);
    free(text);
    ProjectSettings p;
    snprintf(p.name, sizeof p.name, "Demo Game");
    snprintf(p.icon, sizeof p.icon, "icon.bmp");
    p.window_width = 320;
    p.window_height = 200;
    text = project_text(p, &sn);
    put("project.fe", text, sn);
    free(text);
}

static void test_export_and_run() {
    build_demo_project();
    char out[400];
    snprintf(out, sizeof out, "%s/out", g_tmp);

    // ---- the API
    ExportOptions opt;
    opt.project_dir = g_tmp;
    opt.out_dir = out;
    opt.player_path = "build/host/bin/feplayer";
    ExportReport rep;
    bool ok = export_game(opt, &rep);
    CHECK(ok);
    if (!ok) fprintf(stderr, "export error: %s\n", rep.error);
    CHECK(rep.scenes == 2 && rep.missing == 0);
    CHECK(strstr(rep.pak_path, "Demo_Game.fepak") != nullptr);
    CHECK(platform::file_exists(rep.run_path));

    // what is in the package: referenced files yes, the unused one no
    {
        PakReader r;
        uint64_t size = 0;
        void* h = platform::open_read(rep.pak_path, &size);
        CHECK(h != nullptr);
        PakSource src;
        src.user = h;
        src.size = size;
        src.read_at = platform::read_at;
        src.close = platform::close_file;
        CHECK(r.open(src));
        const char* want[] = { "project.fe", "scenes/main.fscene", "scenes/second.fscene", "scripts/spin.lua", "scripts/other.lua",
                               "models/cube.mof", "textures/stone.bmp", "audio/beep.wav", "icon.bmp", "graphics.cfg" };
        for (const char* w : want) {
            PakEntryInfo e;
            bool found = r.find(w, &e);
            CHECK(found);
            if (!found) fprintf(stderr, "missing from the package: %s\n", w);
        }
        CHECK(!r.find("unused/readme.txt", nullptr));
        size_t n = 0;
        char* spin = (char*)r.read("scripts/spin.lua", &n);
        CHECK(spin && n == strlen(kSpinLua) && memcmp(spin, kSpinLua, n) == 0);
        free(spin);
        size_t mn = 0;
        void* m = r.read("models/cube.mof", &mn);
        size_t on = 0;
        void* orig = platform::read_file("assets/samples/cube_textured.mof", &on);
        CHECK(m && orig && mn == on && memcmp(m, orig, on) == 0);
        free(m);
        free(orig);
    }

    // ---- a missing file is reported, not fatal
    put_str("scenes/broken.fscene",
            "farcade-scene 1\nnext_id 2\nobject 1\n  name \"X\"\n  kind mesh\n  mesh cube\n  script \"scripts/gone.lua\"\n  end\nend\n");
    ExportReport rep2;
    snprintf(out, sizeof out, "%s/out2", g_tmp);
    opt.out_dir = out;
    CHECK(export_game(opt, &rep2));
    CHECK(rep2.missing == 1);
    char path[600];
    snprintf(path, sizeof path, "%s/scenes/broken.fscene", g_tmp);
    unlink(path);

    // ---- the command line, then the headless player on the result
    char cmd[2048];
    snprintf(out, sizeof out, "%s/cli", g_tmp);
    snprintf(cmd, sizeof cmd, "build/host/bin/fe-export %s %s --player build/host/bin/feplayer 2>&1", g_tmp, out);
    char* cli = capture(cmd);
    CHECK(cli && strstr(cli, "FE EXPORT OK") != nullptr);
    if (cli && !strstr(cli, "FE EXPORT OK")) fprintf(stderr, "%s\n", cli);
    free(cli);
    // also from a scene file
    snprintf(cmd, sizeof cmd, "build/host/bin/fe-export %s/scenes/second.fscene %s/cli2 --name Second --player build/host/bin/feplayer 2>&1", g_tmp, g_tmp);
    cli = capture(cmd);
    CHECK(cli && strstr(cli, "FE EXPORT OK") != nullptr);
    free(cli);
    snprintf(cmd, sizeof cmd, "build/host/bin/fe-export %s/nothing-here %s/cli3 --player build/host/bin/feplayer 2>&1", g_tmp, g_tmp);
    cli = capture(cmd);
    CHECK(cli && strstr(cli, "FE EXPORT FAIL") != nullptr);
    free(cli);

    // The exported program finds Demo_Game.fepak next to itself.
    snprintf(cmd, sizeof cmd, "%s/cli/Demo_Game --frames 20 --png %s/frame20.png 2>&1", g_tmp, g_tmp);
    char* run20 = capture(cmd);
    CHECK(run20 != nullptr);
    if (run20) {
        CHECK(strstr(run20, "FE GAME start Spinner") != nullptr);              // on_start ran
        CHECK(strstr(run20, "FE GAME spin 15") != nullptr);                    // on_update ran 15 times
        CHECK(strstr(run20, "FE PLAYER DONE frames=20") != nullptr);
        CHECK(strstr(run20, "scripts=1") != nullptr);
        CHECK(strstr(run20, "graphics settings from graphics.cfg (preset 0)") != nullptr);   // applied at start
        CHECK(strstr(run20, "FE GAME other") == nullptr);                      // the second scene is not the entry scene
        if (!strstr(run20, "FE PLAYER DONE")) fprintf(stderr, "%s\n", run20);
    }
    snprintf(cmd, sizeof cmd, "%s/cli/Demo_Game --frames 1 2>&1", g_tmp);
    char* run1 = capture(cmd);
    CHECK(run1 && strstr(run1, "FE PLAYER DONE frames=1") != nullptr);
    unsigned c20 = run20 ? find_checksum(run20) : 0, c1 = run1 ? find_checksum(run1) : 0;
    CHECK(c20 != 0 && c1 != 0 && c20 != c1);        // the script turned the cube: the picture changed
    // Deterministic: the same frames again give the same picture.
    snprintf(cmd, sizeof cmd, "%s/cli/Demo_Game --frames 20 2>&1", g_tmp);
    char* again = capture(cmd);
    CHECK(again && find_checksum(again) == c20);
    free(run20);
    free(run1);
    free(again);
    // a PNG was written (signature)
    snprintf(path, sizeof path, "%s/frame20.png", g_tmp);
    size_t pn = 0;
    unsigned char* png = (unsigned char*)platform::read_file(path, &pn);
    CHECK(png && pn > 200 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
    free(png);
    // the window size of project.fe is the frame size in the headless build
    snprintf(cmd, sizeof cmd, "%s/cli/Demo_Game --frames 1 2>&1", g_tmp);
    char* sized = capture(cmd);
    CHECK(sized && strstr(sized, "(320x200") != nullptr);
    free(sized);

    // The package runs from another folder, passed on the command line; a
    // project folder runs from loose files.
    snprintf(cmd, sizeof cmd, "build/host/bin/feplayer %s/cli/Demo_Game.fepak --frames 3 2>&1", g_tmp);
    char* viapak = capture(cmd);
    CHECK(viapak && strstr(viapak, "FE PLAYER DONE frames=3") != nullptr);
    free(viapak);
    snprintf(cmd, sizeof cmd, "build/host/bin/feplayer %s --frames 3 2>&1", g_tmp);
    char* loose = capture(cmd);
    CHECK(loose && strstr(loose, "FE GAME start Spinner") != nullptr);
    free(loose);
    // second scene as the entry scene
    snprintf(cmd, sizeof cmd, "build/host/bin/feplayer %s/cli/Demo_Game.fepak --scene scenes/second.fscene --frames 3 2>&1", g_tmp);
    char* second = capture(cmd);
    CHECK(second && strstr(second, "FE GAME other") != nullptr);
    free(second);
    // errors
    char* bad = capture("build/host/bin/feplayer /nonexistent/game.fepak 2>&1");
    CHECK(bad && strstr(bad, "FE PLAYER FAIL") != nullptr);
    free(bad);
}

// A scene with effects modules: their files are packed, and the player runs them.
static void test_fx_export() {
    put_str("anims/box.fanim", "fanim 1\nclip slide 1\n track . position\n  key 0 0 0 0\n  key 1 2 0 0\n");
    put_str("scenes/fx.fscene",
            "farcade-scene 1\nnext_id 5\n"
            "object 1\n  name \"Cam\"\n  kind camera\n  position 0 1 6\n  module camera\n    current true\n  end\nend\n"
            "object 2\n  name \"Sparks\"\n  kind empty\n  position 0 0 0\n  module particles\n    emission_rate 80\n    texture \"textures/stone.bmp\"\n    size_start 0.4\n  end\nend\n"
            "object 3\n  name \"Icon\"\n  kind empty\n  position 1 1 0\n  module billboard\n    texture \"textures/stone.bmp\"\n  end\nend\n"
            "object 4\n  name \"Label\"\n  kind empty\n  position -1 1 0\n  module text3d\n    text \"Hi\"\n  end\n"
            "  module animator\n    source \"anims/box.fanim\"\n    clip \"slide\"\n  end\nend\n");
    char out[400], cmd[2048];
    snprintf(out, sizeof out, "%s/fxout", g_tmp);
    snprintf(cmd, sizeof cmd, "build/host/bin/fe-export %s/scenes/fx.fscene %s --name FxGame --player build/host/bin/feplayer 2>&1", g_tmp, out);
    char* cli = capture(cmd);
    CHECK(cli && strstr(cli, "FE EXPORT OK") != nullptr);
    if (cli && !strstr(cli, "FE EXPORT OK")) fprintf(stderr, "%s\n", cli);
    free(cli);
    char pak[500];
    snprintf(pak, sizeof pak, "%s/FxGame.fepak", out);
    uint64_t size = 0;
    void* h = platform::open_read(pak, &size);
    CHECK(h != nullptr);
    if (h) {
        PakReader r;
        PakSource src;
        src.user = h; src.size = size; src.read_at = platform::read_at; src.close = platform::close_file;
        CHECK(r.open(src));
        CHECK(r.find("textures/stone.bmp", nullptr));      // the particle and billboard texture
        CHECK(r.find("anims/box.fanim", nullptr));         // the animator's file
    }
    // The player runs it: particles, a billboard and text change the picture over time, animation runs.
    snprintf(cmd, sizeof cmd, "build/host/bin/feplayer %s --frames 1 2>&1", pak);
    char* a = capture(cmd);
    snprintf(cmd, sizeof cmd, "build/host/bin/feplayer %s --frames 40 2>&1", pak);
    char* b = capture(cmd);
    CHECK(a && b && strstr(b, "FE PLAYER DONE frames=40") != nullptr);
    CHECK(a && b && find_checksum(a) != 0 && find_checksum(a) != find_checksum(b));
    free(a);
    free(b);
}

int main() {
    snprintf(g_tmp, sizeof g_tmp, "/tmp/fe-export-test-XXXXXX");
    if (!mkdtemp(g_tmp)) { perror("mkdtemp"); return 1; }
    printf("export tests in %s\n", g_tmp);
    test_pak();
    test_vfs();
    test_project();
    test_graphics_text();
    test_export_and_run();
    test_fx_export();
    printf("export_tests: %d checks, %d failures\n", g_checks, g_failures);
    if (!g_failures && !getenv("FE_KEEP_TEST_DIR")) {
        char cmd[400];
        snprintf(cmd, sizeof cmd, "rm -rf %s", g_tmp);
        if (system(cmd) != 0) {}
    }
    return g_failures ? 1 : 0;
}
