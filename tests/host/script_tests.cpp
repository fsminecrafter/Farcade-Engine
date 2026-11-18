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
