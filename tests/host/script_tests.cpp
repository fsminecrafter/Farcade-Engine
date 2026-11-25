// Tests of the Lua script runtime (engine/script): the checks shared with the
// Minimal-OS smoke program (tests/common/script_checks.h) and a performance run.
#include <stdio.h>
#include <string.h>

static int g_checks = 0, g_failures = 0;
#define SCRIPT_CHECK(cond) \
    do { g_checks++; if (!(cond)) { g_failures++; \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#include "../common/script_checks.h"

#include "gfx/graphics_settings.h"

using namespace fe_script_checks;

// fe.graphics (engine/script/lua_graphics.h): read, set, presets, list, errors, applied through the host's struct.
static void check_graphics() {
    SCRIPT_ENV(E);
    fe::gfx::GraphicsSettings gs;
    bool changed = false;
    // no settings given: an error, not a crash
    fe::Entity e0 = E.w.create("e0");
    int id0 = E.add(e0, "scripts/g0.lua", "function on_start(self) self.ok = pcall(fe.graphics.get, 'aa') end\n");
    E.w.progress(0.1f);
    SCRIPT_CHECK(id0 > 0 && !E.flag(id0, "ok"));

    E.rt.set_graphics_settings(&gs, &changed);
    gs.apply_preset(fe::gfx::GraphicsPreset::High);
    fe::Entity e = E.w.create("e");
    int id = E.add(e, "scripts/g.lua",
        "function on_start(self)\n"
        "  self.aa0 = fe.graphics.get('aa')\n"
        "  self.scale0 = fe.graphics.get('render_scale')\n"
        "  self.pbr0 = fe.graphics.get('pbr')\n"
        "  fe.graphics.set('aa', 'off')\n"
        "  fe.graphics.set('render_scale', 0.5)\n"
        "  fe.graphics.set('pbr', false)\n"
        "  fe.graphics.set('shadow_resolution', 1024)\n"
        "  self.aa1 = fe.graphics.get('aa')\n"
        "  self.res1 = fe.graphics.get('shadow_resolution')\n"
        "  self.bad_name = pcall(fe.graphics.set, 'nope', 1)\n"
        "  local ok, msg = pcall(fe.graphics.set, 'aa', 'msaa')\n"
        "  self.bad_value = ok\n"
        "  self.msg = msg\n"
        "  self.bad_get = pcall(fe.graphics.get, 'nope')\n"
        "  self.bad_preset = pcall(fe.graphics.preset, 'extreme')\n"
        "  self.bad_type = pcall(fe.graphics.set, 'aa', {})\n"
        "  local list = fe.graphics.list()\n"
        "  self.count = #list\n"
        "  for _, p in ipairs(list) do\n"
        "    if p.name == 'bloom' then self.bloom_values = table.concat(p.values, ',') self.bloom_type = p.type end\n"
        "    if p.name == 'render_scale' then self.scale_max = p.max end\n"
        "  end\n"
        "  self.text = fe.graphics.text()\n"
        "end\n"
        "function on_event(self, name)\n"
        "  if name == 'low' then fe.graphics.preset('low') end\n"
        "end\n");
    SCRIPT_CHECK(id > 0);
    E.w.progress(0.1f);
    SCRIPT_CHECK(E.str_is(id, "aa0", "smaa") && near_(E.num(id, "scale0"), 1.0) && E.flag(id, "pbr0"));
    SCRIPT_CHECK(E.str_is(id, "aa1", "off") && near_(E.num(id, "res1"), 1024));
    SCRIPT_CHECK(!gs.antialiasing && gs.render_scale == 0.5f && !gs.pbr && gs.shadow_resolution == 1024);
    SCRIPT_CHECK(changed);                                   // the host applies it before the next frame
    SCRIPT_CHECK(!E.flag(id, "bad_name") && !E.flag(id, "bad_value") && !E.flag(id, "bad_get") && !E.flag(id, "bad_preset") &&
                 !E.flag(id, "bad_type"));
    char msg[200] = {};
    SCRIPT_CHECK(E.rt.get_string(id, "msg", msg, sizeof msg) && strstr(msg, "invalid value 'msaa'") && strstr(msg, "off, fxaa, smaa, taa"));
    SCRIPT_CHECK(near_(E.num(id, "count"), fe::gfx::graphics_property_count()) && near_(E.num(id, "scale_max"), 1.0));
    SCRIPT_CHECK(E.str_is(id, "bloom_values", "off,simple,chain") && E.str_is(id, "bloom_type", "choice"));
    char text[2048] = {};
    SCRIPT_CHECK(E.rt.get_string(id, "text", text, sizeof text) == false || strstr(text, "farcade-graphics 1") != nullptr);
    // a preset from a later event
    changed = false;
    E.rt.send(e, "low");
    E.w.progress(0.1f);
    SCRIPT_CHECK(changed && gs.preset == fe::gfx::GraphicsPreset::Low && !gs.shadows && !gs.bloom);
    SCRIPT_CHECK(E.errors == 0);
}

template <class F>
static void run(const char* name, F f) {
    int before = g_failures;
    f();
    printf("  %-14s %s\n", name, g_failures == before ? "ok" : "FAILED");
}

// fe.status: what a game wants shown over itself (the player draws it).
static void check_status() {
    SCRIPT_ENV(E);
    SCRIPT_CHECK(!E.rt.status().shown);
    fe::Entity e = E.w.create("e");
    int id = E.add(e, "scripts/status.lua",
        "local n = 0\n"
        "function on_start(self) fe.status('Loading level 2', 0.25) end\n"
        "function on_update(self, dt) n = n + 1\n"
        "  if n == 1 then fe.status('Waiting for players') end\n"
        "  if n == 2 then fe.status('Too far', 7) end\n"
        "  if n == 3 then fe.status() end\n"
        "  if n == 4 then self.bad = pcall(fe.status, {}) end\n"
        "end\n");
    SCRIPT_CHECK(id > 0);
    E.w.progress(0.016f);
    const fe::GameStatus& st = E.rt.status();
    // on_start ran, then the first on_update in the same frame
    SCRIPT_CHECK(st.shown && !strcmp(st.text, "Waiting for players") && st.progress < 0);
    E.w.progress(0.016f);
    SCRIPT_CHECK(st.shown && !strcmp(st.text, "Too far") && st.progress == 1.0f);   // clamped to 0..1
    E.w.progress(0.016f);
    SCRIPT_CHECK(!st.shown && st.text[0] == 0);
    E.w.progress(0.016f);
    SCRIPT_CHECK(!E.flag(id, "bad"));                                                // a table is no text: an error
}

// ---- terrain and streaming through the host (fe.terrain_height, obj.loaded, obj.streaming)
static int g_mode = 2;
static bool g_loaded = false;
static void check_terrain_and_streaming() {
    fe::World w;
    SCRIPT_CHECK(w.init());
    fe::ScriptRuntime rt;
    fe::ScriptHost h;
    h.terrain_height = [](float x, float z, float* hgt, fe::Vec3* n, void*) {
        if (x < 0) return false;
        *hgt = 2.0f + x * 0.5f;
        *n = fe::Vec3(0, 1, 0);
        (void)z;
        return true;
    };
    h.stream_get = [](fe::Entity, int* mode, bool* loaded, void*) { *mode = g_mode; *loaded = g_loaded; return true; };
    h.stream_set = [](fe::Entity, int mode, int loaded, void*) { if (mode >= 0) g_mode = mode; if (loaded >= 0) g_loaded = loaded != 0; };
    SCRIPT_CHECK(rt.init(&w, h));
    fe::Entity e = w.create("obj");
    const char* src =
        "function on_start(self)\n"
        "  self.h = fe.terrain_height(4, 1)\n"
        "  self.none = fe.terrain_height(-1, 0) == nil\n"
        "  self.mode = self.object.streaming\n"
        "  self.before = self.object.loaded\n"
        "  self.object.loaded = true\n"
        "  self.after = self.object.loaded\n"
        "  self.object.streaming = 'always'\n"
        "  self.bad = not pcall(function() self.object.streaming = 'sometimes' end)\n"
        "end\n";
    int id = rt.add_instance(e, "scripts/t.lua", src, (uint32_t)strlen(src), nullptr, 0, nullptr);
    SCRIPT_CHECK(id > 0);
    w.progress(0.016f);
    double hv = 0;
    bool b = false;
    char buf[32];
    SCRIPT_CHECK(rt.get_number(id, "h", &hv) && hv > 3.99 && hv < 4.01);
    SCRIPT_CHECK(rt.get_bool(id, "none", &b) && b);
    SCRIPT_CHECK(rt.get_string(id, "mode", buf, sizeof buf) && !strcmp(buf, "unloaded"));
    SCRIPT_CHECK(rt.get_bool(id, "before", &b) && !b);
    SCRIPT_CHECK(rt.get_bool(id, "after", &b) && b && g_loaded);
    SCRIPT_CHECK(g_mode == 1);
    SCRIPT_CHECK(rt.get_bool(id, "bad", &b) && b);
}

// ---- groups through the host (fe.find_group, obj.groups, obj:in_group / add_group / remove_group)
struct GroupObj { fe::Entity e; char groups[256]; };
static GroupObj g_gobj[4];
static int g_gcount = 0;
static GroupObj* gobj(fe::Entity e) {
    for (int i = 0; i < g_gcount; i++) if (g_gobj[i].e == e) return &g_gobj[i];
    return nullptr;
}
static void check_groups() {
    fe::World w;
    SCRIPT_CHECK(w.init());
    fe::ScriptRuntime rt;
    fe::ScriptHost h;
    h.groups_get = [](fe::Entity e, char* out, int cap, void*) {
        GroupObj* o = gobj(e);
        if (!o) return false;
        snprintf(out, (size_t)cap, "%s", o->groups);
        return true;
    };
    h.groups_set = [](fe::Entity e, const char* g, void*) { if (GroupObj* o = gobj(e)) snprintf(o->groups, sizeof o->groups, "%s", g); };
    h.find_group = [](const char* name, fe::Entity* out, int max, void*) {
        int n = 0;
        for (int i = 0; i < g_gcount; i++)
            if (fe::name_list_has(g_gobj[i].groups, name)) { if (n < max) out[n] = g_gobj[i].e; n++; }
        return n;
    };
    SCRIPT_CHECK(rt.init(&w, h));
    const char* names[] = { "a", "b", "c", "d" };
    const char* groups[] = { "grass;Sway", "grass", "rock", "" };
    g_gcount = 4;
    for (int i = 0; i < 4; i++) {
        g_gobj[i].e = w.create(names[i]);
        snprintf(g_gobj[i].groups, sizeof g_gobj[i].groups, "%s", groups[i]);
    }
    const char* src =
        "function on_start(self)\n"
        "  local g = fe.find_group('GRASS')\n"
        "  self.n = #g\n"
        "  self.first = g[1].name .. g[2].name\n"
        "  self.none = #fe.find_group('nothing')\n"
        "  local a = fe.find('a')\n"
        "  self.in_sway = a:in_group('sway')\n"
        "  self.in_rock = a:in_group('rock')\n"
        "  self.count = #a.groups\n"
        "  local d = fe.find('d')\n"
        "  self.added = d:add_group('grass')\n"
        "  self.again = d:add_group('grass')\n"
        "  self.n2 = #fe.find_group('grass')\n"
        "  self.removed = fe.find('b'):remove_group('grass')\n"
        "  self.n3 = #fe.find_group('grass')\n"
        "  fe.find('c').groups = {'x', 'y'}\n"
        "  fe.find('b').groups = 'p; q'\n"
        "  self.bad = not pcall(function() d:add_group('a;b') end)\n"
        "  self.bad2 = not pcall(function() d.groups = 5 end)\n"
        "end\n";
    int id = rt.add_instance(g_gobj[0].e, "scripts/g.lua", src, (uint32_t)strlen(src), nullptr, 0, nullptr);
    SCRIPT_CHECK(id > 0);
    w.progress(0.016f);
    double v = 0;
    bool b = false;
    char buf[64];
    SCRIPT_CHECK(rt.get_number(id, "n", &v) && v == 2);
    SCRIPT_CHECK(rt.get_string(id, "first", buf, sizeof buf) && !strcmp(buf, "ab"));
    SCRIPT_CHECK(rt.get_number(id, "none", &v) && v == 0);
    SCRIPT_CHECK(rt.get_bool(id, "in_sway", &b) && b);
    SCRIPT_CHECK(rt.get_bool(id, "in_rock", &b) && !b);
    SCRIPT_CHECK(rt.get_number(id, "count", &v) && v == 2);
    SCRIPT_CHECK(rt.get_bool(id, "added", &b) && b);
    SCRIPT_CHECK(rt.get_bool(id, "again", &b) && !b);
    SCRIPT_CHECK(rt.get_number(id, "n2", &v) && v == 3);
    SCRIPT_CHECK(rt.get_bool(id, "removed", &b) && b);
    SCRIPT_CHECK(rt.get_number(id, "n3", &v) && v == 2);
    SCRIPT_CHECK(!strcmp(g_gobj[2].groups, "x;y"));
    SCRIPT_CHECK(!strcmp(g_gobj[1].groups, "p;q"));
    SCRIPT_CHECK(!strcmp(g_gobj[3].groups, "grass"));
    SCRIPT_CHECK(rt.get_bool(id, "bad", &b) && b);
    SCRIPT_CHECK(rt.get_bool(id, "bad2", &b) && b);
}

// ---- foliage layers (fe.find_foliage, fe.assign_shader_to_foliage)
static void check_foliage() {
    fe::World w;
    SCRIPT_CHECK(w.init());
    fe::ScriptRuntime rt;
    fe::ScriptHost h;
    SCRIPT_CHECK(rt.init(&w, h));
    fe::Entity t1 = w.create("t1"), t2 = w.create("t2");
    fe::Terrain td;
    snprintf(td.foliage[0].name, sizeof td.foliage[0].name, "Grass");
    snprintf(td.foliage[1].name, sizeof td.foliage[1].name, "flowers");
    snprintf(td.foliage[0].model, sizeof td.foliage[0].model, "a.mof;b.mof");
    SCRIPT_CHECK(w.set(t1, fe::ModuleKind::Terrain, &td));
    snprintf(td.foliage[2].name, sizeof td.foliage[2].name, "grass");
    SCRIPT_CHECK(w.set(t2, fe::ModuleKind::Terrain, &td));
    const char* src =
        "function on_start(self)\n"
        "  local g = fe.find_foliage('grass')\n"
        "  self.n = #g\n"
        "  self.models = g[1].models\n"
        "  self.layer = g[1].layer\n"
        "  self.obj = g[1].object.name\n"
        "  g[1].density = 7\n"
        "  g[1].shadows = true\n"
        "  self.d = g[1].density\n"
        "  self.changed = fe.assign_shader_to_foliage('grass', 'shaders/sway.fshader')\n"
        "  self.shader = g[3].shader\n"
        "  self.other = fe.find_foliage('flowers')[1].shader\n"
        "  self.bad = not pcall(function() return g[1].nope end)\n"
        "  self.bad2 = not pcall(function() g[1].density = 'lots' end)\n"
        "end\n";
    int id = rt.add_instance(t1, "scripts/f.lua", src, (uint32_t)strlen(src), nullptr, 0, nullptr);
    SCRIPT_CHECK(id > 0);
    w.progress(0.016f);
    double v = 0;
    bool b = false;
    char buf[128];
    SCRIPT_CHECK(rt.get_number(id, "n", &v) && v == 3);       // t1 layer 1, t2 layers 1 and 3
    SCRIPT_CHECK(rt.get_string(id, "models", buf, sizeof buf) && !strcmp(buf, "a.mof;b.mof"));
    SCRIPT_CHECK(rt.get_number(id, "layer", &v) && v == 1);
    SCRIPT_CHECK(rt.get_string(id, "obj", buf, sizeof buf) && !strcmp(buf, "t1"));
    SCRIPT_CHECK(rt.get_number(id, "d", &v) && v == 7);
    SCRIPT_CHECK(rt.get_number(id, "changed", &v) && v == 3);
    SCRIPT_CHECK(rt.get_string(id, "shader", buf, sizeof buf) && !strcmp(buf, "shaders/sway.fshader"));
    SCRIPT_CHECK(rt.get_string(id, "other", buf, sizeof buf) && buf[0] == 0);
    SCRIPT_CHECK(rt.get_bool(id, "bad", &b) && b);
    SCRIPT_CHECK(rt.get_bool(id, "bad2", &b) && b);
    const fe::Terrain* r1 = (const fe::Terrain*)w.get(t1, fe::ModuleKind::Terrain);
    const fe::Terrain* r2 = (const fe::Terrain*)w.get(t2, fe::ModuleKind::Terrain);
    SCRIPT_CHECK(r1 && r1->foliage[0].density == 7 && r1->foliage[0].cast_shadows);
    SCRIPT_CHECK(r1 && !strcmp(r1->foliage[0].shader, "shaders/sway.fshader") && !r1->foliage[1].shader[0]);
    SCRIPT_CHECK(r2 && !strcmp(r2->foliage[2].shader, "shaders/sway.fshader"));
}

int main() {
    run("load errors", check_load_errors);
    run("inputs", check_inputs);
    run("lifecycle", check_lifecycle);
    run("vec3", check_vec3);
    run("transforms", check_transforms);
    run("modules", check_modules);
    run("material", check_material);
    run("cross script", check_cross_script);
    run("events", check_events);
    run("coroutines", check_coroutines_and_limits);
    run("find", check_find);
    run("physics events", check_physics_events);
    run("terrain and streaming", check_terrain_and_streaming);
    run("groups", check_groups);
    run("foliage", check_foliage);
    run("errors", check_errors);
    run("sandbox", check_sandbox);
    run("destroy", check_destroy);
    run("graphics", check_graphics);
    run("status", check_status);
    double ms = 0;
    run("2000 instances", [&] { check_many(&ms, 2000); });
    printf("script: 2000 instances on_update: %.3f ms/frame\n", ms);
    printf("script: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
