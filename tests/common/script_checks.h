// Script runtime checks shared by the host tests (tests/host/script_tests.cpp)
// and the Minimal-OS smoke program (tests/os/script_smoke). Public API only.
// The includer defines SCRIPT_CHECK(cond) before including this header.
#pragma once

#include "ecs/ecs.h"
#include "script/runtime.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace fe_script_checks {

using namespace fe;

inline bool near_(double a, double b, double eps = 1e-3) { return fabs(a - b) < eps; }

// A world, a runtime and a copy of everything the scripts logged.
struct Env {
    World w;
    ScriptRuntime rt;
    char log[16384] = {};
    int infos = 0, warnings = 0, errors = 0;

    static void sink(ScriptLogLevel lv, const char* text, void* user) {
        Env* e = (Env*)user;
        if (lv == ScriptLogLevel::Info) e->infos++;
        else if (lv == ScriptLogLevel::Warning) e->warnings++;
        else e->errors++;
        size_t n = strlen(e->log);
        snprintf(e->log + n, sizeof e->log - n, "[%c] %s\n", lv == ScriptLogLevel::Info ? 'I' : lv == ScriptLogLevel::Warning ? 'W' : 'E', text);
    }
    bool init() {
        if (!w.init()) return false;
        ScriptHost h;
        h.log = sink;
        h.user = this;
        return rt.init(&w, h);
    }
    bool logged(const char* s) const { return strstr(log, s) != nullptr; }
    void clear_log() { log[0] = 0; infos = warnings = errors = 0; }

    int add(Entity e, const char* path, const char* src, const ScriptProp* props = nullptr, int np = 0, const Entity* objs = nullptr) {
        return rt.add_instance(e, path, src, (uint32_t)strlen(src), props, np, objs);
    }
    bool flag(int inst, const char* f) { bool b = false; return rt.get_bool(inst, f, &b) && b; }
    double num(int inst, const char* f, double dflt = -12345) { double d; return rt.get_number(inst, f, &d) ? d : dflt; }
    bool str_is(int inst, const char* f, const char* want) {
        char buf[256];
        return rt.get_string(inst, f, buf, sizeof buf) && strcmp(buf, want) == 0;
    }
};

inline Vec3 pos_of(const World& w, Entity e) { const Transform* t = w.transform(e); return t ? t->position : Vec3(-999, -999, -999); }

#define SCRIPT_ENV(name) \
    ::fe_script_checks::Env name; \
    SCRIPT_CHECK(name.init()); \
    if (!name.w.valid()) return;

// ---- load errors
inline void check_load_errors() {
    SCRIPT_ENV(E);
    Entity e = E.w.create("obj");
    SCRIPT_CHECK(E.add(e, "scripts/bad.lua", "local a = 1\nx = = 2\n") == 0);
    SCRIPT_CHECK(E.logged("scripts/bad.lua:2:") && E.errors == 1);
    E.clear_log();
    SCRIPT_CHECK(E.add(e, "scripts/boom.lua", "-- top level\nerror('nope at load')\n") == 0);
    SCRIPT_CHECK(E.logged("scripts/boom.lua:2: nope at load") && E.logged("stack traceback"));
    SCRIPT_CHECK(E.rt.stats().instances == 0 && E.rt.stats().failed == 0);
    SCRIPT_CHECK(E.add(e, "scripts/ok.lua", "x = 1") > 0);
    // text only: a precompiled chunk is refused
    SCRIPT_CHECK(E.add(e, "scripts/bin.lua", "\x1bLua") == 0);
    SCRIPT_CHECK(E.rt.stats().instances == 1);
}

// ---- inputs reach self
inline void check_inputs() {
    SCRIPT_ENV(E);
    Entity e = E.w.create("hero");
    Entity other = E.w.create("friend");
    ScriptProp p[5];
    strcpy(p[0].name, "speed"); p[0].type = PropType::Number; p[0].number = 5;
    strcpy(p[1].name, "label"); p[1].type = PropType::Text; strcpy(p[1].text, "Front door");
    strcpy(p[2].name, "active"); p[2].type = PropType::Toggle; p[2].toggle = true;
    strcpy(p[3].name, "mode"); p[3].type = PropType::List; p[3].choice_count = 3;
    strcpy(p[3].choices[0], "idle"); strcpy(p[3].choices[1], "patrol"); strcpy(p[3].choices[2], "chase"); p[3].choice = 1;
    strcpy(p[4].name, "target"); p[4].type = PropType::Object; p[4].object = 7;
    Entity objs[5] = { 0, 0, 0, 0, other };
    int id = E.add(e, "scripts/hero.lua",
        "function on_start(self)\n"
        "  self.t_speed = self.speed + 0.5\n"
        "  self.t_target = self.target.name\n"
        "  self.t_object = self.object.name\n"
        "  self.t_name = self.name\n"
        "  self.t_types = type(self.speed) .. type(self.label) .. type(self.active) .. type(self.mode) .. type(self.target)\n"
        "  self.t_same = (self.target == fe.find('friend'))\n"
        "end\n", p, 5, objs);
    SCRIPT_CHECK(id > 0);
    E.w.progress(0.016f);
    SCRIPT_CHECK(near_(E.num(id, "speed"), 5) && near_(E.num(id, "t_speed"), 5.5));
    SCRIPT_CHECK(E.str_is(id, "label", "Front door") && E.flag(id, "active") && E.str_is(id, "mode", "patrol"));
    SCRIPT_CHECK(E.str_is(id, "t_target", "friend") && E.str_is(id, "t_object", "hero") && E.str_is(id, "t_name", "hero"));
    SCRIPT_CHECK(E.str_is(id, "t_types", "numberstringbooleanstringuserdata") && E.flag(id, "t_same"));
    // an unlinked object input is nil
    Entity nobj[5] = { 0, 0, 0, 0, 0 };
    int id2 = E.add(e, "scripts/hero2.lua", "function on_start(self) self.nil_target = (self.target == nil) end\n", p, 5, nobj);
    E.w.progress(0.016f);
    SCRIPT_CHECK(E.flag(id2, "nil_target"));
}

// ---- on_start once, on_update with dt, fe.dt, later instances start next frame
inline void check_lifecycle() {
    SCRIPT_ENV(E);
    Entity e = E.w.create("a");
    int id = E.add(e, "scripts/life.lua",
        "function on_start(self) self.starts = (self.starts or 0) + 1; self.at_start = (self.updates or 0) end\n"
        "function on_update(self, dt)\n"
        "  self.updates = (self.updates or 0) + 1\n"
        "  self.sum = (self.sum or 0) + dt\n"
        "  self.start_seen = self.starts\n"
        "  self.fedt = fe.dt\n"
        "  self.t = fe.time()\n"
        "end\n");
    SCRIPT_CHECK(id > 0);
    SCRIPT_CHECK(E.num(id, "starts") == -12345);        // nothing runs before the first frame
    E.w.progress(0.5f);
    E.w.progress(0.5f);
    E.w.progress(0.25f);
    SCRIPT_CHECK(E.num(id, "starts") == 1 && E.num(id, "updates") == 3);
    SCRIPT_CHECK(E.num(id, "at_start") == 0 && E.num(id, "start_seen") == 1);
    SCRIPT_CHECK(near_(E.num(id, "sum"), 1.25) && near_(E.num(id, "fedt"), 0.25));
    SCRIPT_CHECK(E.num(id, "t") >= 0 && E.num(id, "t") <= E.rt.time());
    int late = E.add(e, "scripts/late.lua", "function on_update(self, dt) self.n = (self.n or 0) + 1 end\n");
    SCRIPT_CHECK(E.num(late, "n") == -12345);
    E.w.progress(0.1f);
    SCRIPT_CHECK(E.num(late, "n") == 1 && E.num(id, "updates") == 4 && E.num(id, "starts") == 1);
    SCRIPT_CHECK(E.rt.stats().instances == 2 && E.rt.stats().update_ms >= 0);
    // fe.dt is also set in on_start
    int s2 = E.add(e, "scripts/s2.lua", "function on_start(self) self.d = fe.dt end\n");
    E.w.progress(0.125f);
    SCRIPT_CHECK(near_(E.num(s2, "d"), 0.125));
}

// ---- vec3
inline void check_vec3() {
    SCRIPT_ENV(E);
    Entity e = E.w.create("v");
    int id = E.add(e, "scripts/vec.lua",
        "function on_start(self)\n"
        "  local a, b = fe.vec3(1, 2, 3), fe.vec3(4, 5, 6)\n"
        "  local s = a + b * 2 - fe.vec3(1, 1, 1)\n"
        "  self.sum = s.x == 8 and s.y == 11 and s.z == 14\n"
        "  self.neg = (-a).y == -2 and (2 * a).z == 6\n"
        "  self.eq = a == fe.vec3(1, 2, 3) and a ~= b\n"
        "  self.len = fe.vec3(3, 4, 0):length()\n"
        "  local n = fe.vec3(0, 0, 5):normalized()\n"
        "  self.norm = n.z == 1 and n.x == 0\n"
        "  self.dot = fe.dot(a, b)\n"
        "  local c = fe.cross(fe.vec3(1, 0, 0), fe.vec3(0, 1, 0))\n"
        "  self.cross = c.x == 0 and c.y == 0 and c.z == 1\n"
        "  self.str = tostring(fe.vec3(1, 2.5, 3))\n"
        "  self.mut = a; a.x = 9; self.mut = (self.mut.x == 9)\n"
        "  self.zero = fe.vec3().x == 0\n"
        "  self.o = self.object\n"
        "  self.o.position = {x = 1, y = 2, z = 3}\n"
        "  self.p1 = self.o.position.z == 3\n"
        "  self.o.position = {4, 5, 6}\n"
        "  self.p2 = self.o.position.y == 5\n"
        "  local ok, err = pcall(function() self.o.position = 5 end)\n"
        "  self.bad = not ok and err\n"
        "end\n");
    E.w.progress(0.1f);
    SCRIPT_CHECK(id > 0 && E.flag(id, "sum") && E.flag(id, "neg") && E.flag(id, "eq") && E.flag(id, "norm") && E.flag(id, "cross"));
    SCRIPT_CHECK(near_(E.num(id, "len"), 5) && near_(E.num(id, "dot"), 32));
    SCRIPT_CHECK(E.str_is(id, "str", "vec3(1, 2.5, 3)") && E.flag(id, "mut") && E.flag(id, "zero"));
    SCRIPT_CHECK(E.flag(id, "p1") && E.flag(id, "p2"));
    SCRIPT_CHECK(pos_of(E.w, e).x == 4 && pos_of(E.w, e).z == 6);
    char buf[256];
    SCRIPT_CHECK(E.rt.get_string(id, "bad", buf, sizeof buf) && strstr(buf, "expected a vec3"));
}

// ---- transforms, hierarchy
inline void check_transforms() {
    SCRIPT_ENV(E);
    Entity parent = E.w.create("parent");
    Entity child = E.w.create("child", parent);
    E.w.transform(parent)->position = Vec3(5, 0, 0);
    int id = E.add(child, "scripts/mover.lua",
        "function on_start(self)\n"
        "  self.object.rotation = fe.vec3(10, 20, 30)\n"
        "  local r = self.object.rotation\n"
        "  self.rx, self.ry, self.rz = r.x, r.y, r.z\n"
        "  self.object.scale = fe.vec3(2, 2, 2)\n"
        "  self.pname = self.object.parent.name\n"
        "  self.nkids = #self.object.parent:children()\n"
        "  self.kid_is_me = self.object.parent:children()[1] == self.object\n"
        "  self.noparent = self.object.parent.parent == nil\n"
        "end\n"
        "function on_update(self, dt)\n"
        "  self.object.position = self.object.position + fe.vec3(1, 0, 0) * dt\n"
        "  self.object:move(0, 1, 0)\n"
        "  self.wx = self.object.world_position.x\n"
        "end\n");
    SCRIPT_CHECK(id > 0);
    E.w.progress(1.0f);
    E.w.progress(1.0f);
    SCRIPT_CHECK(near_(E.num(id, "rx"), 10, 0.01) && near_(E.num(id, "ry"), 20, 0.01) && near_(E.num(id, "rz"), 30, 0.01));
    SCRIPT_CHECK(E.str_is(id, "pname", "parent") && E.num(id, "nkids") == 1 && E.flag(id, "kid_is_me") && E.flag(id, "noparent"));
    const Transform* t = E.w.transform(child);
    SCRIPT_CHECK(near_(t->position.x, 2) && near_(t->position.y, 2) && near_(t->position.z, 0));
    SCRIPT_CHECK(near_(t->scale.x, 2) && near_(t->scale.z, 2));
    Vec3 ang = t->rotation.to_euler_deg();
    SCRIPT_CHECK(near_(ang.x, 10, 0.01) && near_(ang.y, 20, 0.01) && near_(ang.z, 30, 0.01));
    // read in the second frame, from the first frame's world transform: parent 5 + local 1
    SCRIPT_CHECK(near_(E.num(id, "wx"), 5 + 1, 0.01));
    // names: rename and clash
    int nid = E.add(child, "scripts/rename.lua",
        "function on_start(self)\n"
        "  self.object.name = 'kid'\n"
        "  self.n = self.object.name\n"
        "  local ok, err = pcall(function() self.object.world_position = fe.vec3() end)\n"
        "  self.ro = not ok and err\n"
        "end\n");
    E.w.progress(0.1f);
    SCRIPT_CHECK(E.str_is(nid, "n", "kid") && E.w.lookup_child(parent, "kid") == child);
    char buf[256];
    SCRIPT_CHECK(E.rt.get_string(nid, "ro", buf, sizeof buf) && strstr(buf, "read only"));
}

// ---- modules by Lua name
inline void check_modules() {
    SCRIPT_ENV(E);
    Entity e = E.w.create("lamp");
    Entity plain = E.w.create("plain");
    Entity camA = E.w.create("camA"), camB = E.w.create("camB");
    E.w.add(e, ModuleKind::Light);
    E.w.add(camA, ModuleKind::Camera);
    E.w.add(camB, ModuleKind::Camera);
    E.w.set_current(camA, ModuleKind::Camera);
    ScriptProp p;
    strcpy(p.name, "cam"); p.type = PropType::Object;
    Entity objs[1] = { camB };
    int id = E.add(e, "scripts/mod.lua",
        "function on_start(self)\n"
        "  local o = self.object\n"
        "  local l = o.light\n"
        "  self.type0 = l.type\n"
        "  self.intensity0 = l.intensity\n"
        "  l.intensity = 5000\n"                          // clamped to the range (0..100)
        "  self.intensity1 = l.intensity\n"
        "  l.type = 'Spot'\n"
        "  self.type1 = l.type\n"
        "  l.color = fe.vec3(1, 0.5, 0.25)\n"
        "  self.cg = l.color.y\n"
        "  l.color = {0.25, 0.5, 0.75}\n"
        "  self.cb = l.color.z\n"
        "  l.shadows = false\n"
        "  self.shadows = l.shadows\n"
        "  self.nocam = (o.camera == nil)\n"
        "  self.noplain = (fe.find('plain').light == nil)\n"
        "  self.rb_nil = (o.rigidbody3d == nil)\n"
        "  local function try(f) local ok, err = pcall(f); return (not ok) and err or '' end\n"
        "  self.e1 = try(function() l.nope = 1 end)\n"
        "  self.e2 = try(function() l.intensity = 'x' end)\n"
        "  self.e3 = try(function() l.type = 'Bogus' end)\n"
        "  self.e4 = try(function() return l.nope end)\n"
        "  self.cur0 = fe.current('camera').name\n"
        "  self.cam.camera.current = true\n"
        "  self.cur1 = fe.current('camera').name\n"
        "  self.fov0 = self.cam.camera.fov\n"
        "  self.cam.camera.fov = 75\n"
        "  self.proj = self.cam.camera.projection\n"
        "  self.mt = tostring(l)\n"
        "end\n", &p, 1, objs);
    SCRIPT_CHECK(id > 0);
    E.w.progress(0.1f);
    SCRIPT_CHECK(E.str_is(id, "type0", "Point") && near_(E.num(id, "intensity0"), 1) && near_(E.num(id, "intensity1"), 100));
    SCRIPT_CHECK(E.str_is(id, "type1", "Spot") && near_(E.num(id, "cg"), 0.5) && near_(E.num(id, "cb"), 0.75));
    SCRIPT_CHECK(E.flag(id, "nocam") && E.flag(id, "noplain") && E.flag(id, "rb_nil"));
    bool sh = true;
    SCRIPT_CHECK(E.rt.get_bool(id, "shadows", &sh) && !sh);
    FieldValue v;
    SCRIPT_CHECK(E.w.get_field(e, ModuleKind::Light, "type", &v) && v.s && strcmp(v.s, "Spot") == 0);
    SCRIPT_CHECK(E.w.get_field(e, ModuleKind::Light, "color", &v) && near_(v.v.x, 0.25) && near_(v.v.z, 0.75));
    char buf[256];
    SCRIPT_CHECK(E.rt.get_string(id, "e1", buf, sizeof buf) && strstr(buf, "light") && strstr(buf, "no field 'nope'"));
    SCRIPT_CHECK(E.rt.get_string(id, "e2", buf, sizeof buf) && strstr(buf, "light.intensity expects a number"));
    SCRIPT_CHECK(E.rt.get_string(id, "e3", buf, sizeof buf) && strstr(buf, "light.type") && strstr(buf, "Directional"));
    SCRIPT_CHECK(E.rt.get_string(id, "e4", buf, sizeof buf) && strstr(buf, "no field 'nope'"));
    // Inside a frame the world applies the one-current rule at the end of the phase:
    // fe.current() may still say camA right after the assignment, the world says camB after progress().
    SCRIPT_CHECK(E.str_is(id, "cur0", "camA"));
    SCRIPT_CHECK(E.w.current(ModuleKind::Camera) == camB);
    FieldValue cv;
    SCRIPT_CHECK(E.w.get_field(camA, ModuleKind::Camera, "current", &cv) && !cv.b);
    SCRIPT_CHECK(near_(E.num(id, "fov0"), 60) && near_(((const Camera*)E.w.get(camB, ModuleKind::Camera))->fov, 75));
    SCRIPT_CHECK(E.str_is(id, "proj", "Perspective") && E.str_is(id, "mt", "module(light)"));
    (void)plain;
}

// ---- scripts on different objects
inline void check_cross_script() {
    SCRIPT_ENV(E);
    Entity door = E.w.create("door1");
    Entity sw = E.w.create("switch1");
    ScriptProp dp;
    strcpy(dp.name, "locked"); dp.type = PropType::Toggle; dp.toggle = true;
    ScriptProp dsp;
    strcpy(dsp.name, "speed"); dsp.type = PropType::Number; dsp.number = 3;
    int other = E.add(door, "scenes/helper.lua", "x = 1");
    ScriptProp props2[2] = { dp, dsp };
    int did = E.add(door, "scripts/door.lua",
        "function open(self, how) self.opened = true; self.how = how or 'plain'; return 42 end\n"
        "function on_used_by(self, who) self.used_by = who.name end\n", props2, 2);
    ScriptProp sp;
    strcpy(sp.name, "door"); sp.type = PropType::Object;
    Entity objs[1] = { door };
    int sid = E.add(sw, "scripts/switch.lua",
        "function on_start(self)\n"
        "  local d = self.door:script('door')\n"
        "  self.found = d ~= nil\n"
        "  self.found2 = self.door:script('door.lua') == d\n"
        "  self.missing = self.door:script('nothing') == nil\n"
        "  self.locked = d.locked\n"
        "  self.speed = d.speed\n"
        "  self.ret = d:open('wide')\n"
        "  self.n = #self.door:scripts()\n"
        "  self.door:send('used_by', self.object)\n"
        "  self.same = self.door:script('door') == d\n"
        "  self.other = self.door:scripts()[1].name\n"
        "  d.locked = false\n"
        "end\n", &sp, 1, objs);
    SCRIPT_CHECK(other > 0 && did > 0 && sid > 0);
    E.w.progress(0.1f);
    SCRIPT_CHECK(E.flag(sid, "found") && E.flag(sid, "found2") && E.flag(sid, "missing") && E.flag(sid, "same"));
    SCRIPT_CHECK(E.flag(sid, "locked") && near_(E.num(sid, "speed"), 3) && near_(E.num(sid, "ret"), 42) && near_(E.num(sid, "n"), 2));
    SCRIPT_CHECK(E.flag(did, "opened") && E.str_is(did, "how", "wide") && E.str_is(did, "used_by", "switch1"));
    SCRIPT_CHECK(E.str_is(sid, "other", "helper"));
    bool locked = true;
    SCRIPT_CHECK(E.rt.get_bool(did, "locked", &locked) && !locked);
    SCRIPT_CHECK(E.errors == 0);
}

// ---- send, on_<event>, on_event, broadcast, host events
inline void check_events() {
    SCRIPT_ENV(E);
    Entity target = E.w.create("target");
    Entity sender = E.w.create("sender");
    Entity third = E.w.create("third");
    int a = E.add(target, "scripts/a.lua",
        "function on_ping(self, x, y) self.got = x + y; self.order = (self.order or '') .. 'a' end\n"
        "function on_event(self, name, x, y) self.ev = name .. ':' .. tostring(x); self.order = (self.order or '') .. 'e' end\n"
        "function on_alarm(self, level) self.alarm = level end\n");
    int b = E.add(target, "scripts/b.lua", "function on_event(self, name, x) self.evb = name end\n");
    int t = E.add(third, "scripts/t.lua", "function on_alarm(self, level) self.alarm = level * 2 end\n");
    int s = E.add(sender, "scripts/s.lua",
        "function on_start(self)\n"
        "  local t = fe.find('target')\n"
        "  t:send('ping', 3, 4)\n"
        "  self.after = t:script('a').got\n"      // delivered synchronously
        "  fe.broadcast('alarm', 7)\n"
        "  t:send('nobody_listens')\n"
        "  t:send('event', 1)\n"
        "end\n"
        "function on_alarm(self, level) self.alarm = level end\n");
    SCRIPT_CHECK(a > 0 && b > 0 && t > 0 && s > 0);
    E.w.progress(0.1f);
    SCRIPT_CHECK(near_(E.num(a, "got"), 7) && near_(E.num(s, "after"), 7));
    SCRIPT_CHECK(E.str_is(a, "ev", "event:1") && E.str_is(b, "evb", "event"));      // on_event after on_<name>, once for "event"
    SCRIPT_CHECK(E.str_is(a, "order", "aeeee"));
    SCRIPT_CHECK(near_(E.num(a, "alarm"), 7) && near_(E.num(t, "alarm"), 14) && near_(E.num(s, "alarm"), 7));
    // from the host
    E.rt.send(target, "ping");               // on_ping(self) with nil args: an error stops that instance
    SCRIPT_CHECK(E.errors == 1 && E.logged("scripts/a.lua:1:"));
    E.rt.send(target, "alarm");
    SCRIPT_CHECK(E.errors == 1 && E.rt.stats().failed == 1);     // a is failed, b has no on_alarm
    SCRIPT_CHECK(near_(E.num(a, "alarm"), 7));
}

// ---- fe.find, fe.current
inline void check_find() {
    SCRIPT_ENV(E);
    Entity root = E.w.create("world");
    Entity mid = E.w.create("mid", root);
    Entity deep = E.w.create("deep", mid);
    Entity deep2 = E.w.create("deep", E.w.create("other_root"));
    Entity shallow = E.w.create("same", root);
    Entity far = E.w.create("same", deep);
    (void)deep2; (void)far;
    Entity cam = E.w.create("cam");
    E.w.add(cam, ModuleKind::Camera);
    E.w.set_current(cam, ModuleKind::Camera);
    int id = E.add(root, "scripts/find.lua",
        "function on_start(self)\n"
        "  local d = fe.find('deep')\n"
        "  self.deep = d ~= nil and d.name == 'deep'\n"
        "  self.deep_parent = d.parent.name\n"
        "  self.root = fe.find('world') == self.object\n"
        "  self.none = fe.find('nothing') == nil\n"
        "  self.cur = fe.current('camera').name\n"
        "  self.same_parent = fe.find('same').parent.name\n"
        "  self.nolight = fe.current('light')\n"
        "  self.nosky = fe.current('sky') == nil\n"
        "  local ok, err = pcall(fe.current, 'bogus')\n"
        "  self.badname = not ok and err\n"
        "  local t = {}\n"
        "  t[fe.find('world')] = 1\n"
        "  self.key = t[self.object] == 1\n"
        "end\n");
    E.w.progress(0.1f);
    SCRIPT_CHECK(id > 0 && E.flag(id, "deep") && E.flag(id, "root") && E.flag(id, "none") && E.flag(id, "key") && E.flag(id, "nosky"));
    SCRIPT_CHECK(E.str_is(id, "cur", "cam"));
    char buf[256];
    SCRIPT_CHECK(E.rt.get_string(id, "badname", buf, sizeof buf) && strstr(buf, "unknown module"));
    SCRIPT_CHECK(E.str_is(id, "deep_parent", "other_root"));     // the match nearest the roots
    SCRIPT_CHECK(E.str_is(id, "same_parent", "world"));
    (void)shallow;
}

// ---- triggers and collisions
inline void check_physics_events() {
    SCRIPT_ENV(E);
    Entity zone = E.w.create("zone");
    Entity ball = E.w.create("ball");
    Entity dead = E.w.create("dead");
    int z = E.add(zone, "scripts/zone.lua",
        "function on_trigger_enter(self, other) self.entered = other.name; self.enters = (self.enters or 0) + 1 end\n"
        "function on_trigger_exit(self, other) self.exited = other.name end\n"
        "function on_collision(self, other) self.hit = other.name end\n");
    int bl = E.add(ball, "scripts/ball.lua", "function on_collision(self, other) self.hit = other.name end\n");
    SCRIPT_CHECK(z > 0 && bl > 0);
    E.rt.trigger_enter(zone, ball);
    E.rt.trigger_enter(zone, ball);
    E.rt.trigger_exit(zone, ball);
    SCRIPT_CHECK(E.str_is(z, "entered", "ball") && E.str_is(z, "exited", "ball") && near_(E.num(z, "enters"), 2));
    E.rt.collision(zone, ball);
    SCRIPT_CHECK(E.str_is(z, "hit", "ball") && E.str_is(bl, "hit", "zone"));      // both sides
    E.rt.collision(ball, zone);
    SCRIPT_CHECK(E.str_is(z, "hit", "ball") && E.str_is(bl, "hit", "zone"));
    // a dead entity is skipped
    E.w.destroy(dead);
    E.rt.trigger_enter(zone, dead);
    SCRIPT_CHECK(near_(E.num(z, "enters"), 2) && E.errors == 0);
}

// ---- errors, print
inline void check_errors() {
    SCRIPT_ENV(E);
    Entity e = E.w.create("e");
    int bad = E.add(e, "scripts/bad.lua",
        "function on_update(self, dt)\n"
        "  self.ran = (self.ran or 0) + 1\n"
        "  local x = nil\n"
        "  return x.field\n"
        "end\n");
    int good = E.add(e, "scripts/good.lua",
        "function on_update(self, dt)\n"
        "  self.n = (self.n or 0) + 1\n"
        "  print('tick', self.n, true, nil)\n"
        "  if self.n == 2 then fe.warn('careful', 5) end\n"
        "  if self.n == 3 then fe.log('plain') end\n"
        "end\n");
    int objerr = E.add(e, "scripts/objerr.lua", "function on_update(self, dt) error({}) end\n");
    E.w.progress(0.1f);
    SCRIPT_CHECK(E.errors == 2 && E.logged("scripts/bad.lua:4:") && E.logged("attempt to index a nil value"));
    SCRIPT_CHECK(E.logged("stack traceback") && E.logged("error object is a table value"));
    E.w.progress(0.1f);
    E.w.progress(0.1f);
    SCRIPT_CHECK(near_(E.num(bad, "ran"), 1));           // stopped after the first error
    SCRIPT_CHECK(near_(E.num(good, "n"), 3) && E.errors == 2);
    SCRIPT_CHECK(E.rt.stats().failed == 2 && E.rt.stats().instances == 3);
    SCRIPT_CHECK(E.logged("[I] tick\t1\ttrue\tnil") && E.warnings == 1 && E.logged("[W] careful\t5") && E.logged("[I] plain"));
    // on_start errors stop the instance too
    int bs = E.add(e, "scripts/badstart.lua", "function on_start(self) error('start failed') end\nfunction on_update(self) self.u = 1 end\n");
    E.w.progress(0.1f);
    SCRIPT_CHECK(E.logged("scripts/badstart.lua:1: start failed") && E.num(bs, "u") == -12345 && E.rt.stats().failed == 3);
    // run(): the console
    E.clear_log();
    SCRIPT_CHECK(E.rt.run("counter = (counter or 0) + 1; print('console', counter)"));
    SCRIPT_CHECK(E.rt.run("counter = counter + 1; print('console', counter)") && E.logged("console\t2"));
    SCRIPT_CHECK(!E.rt.run("error('from console')") && E.logged("console:1: from console"));
    SCRIPT_CHECK(!E.rt.run("x = = 1"));
    (void)objerr;
}

// ---- sandbox
inline void check_sandbox() {
    SCRIPT_ENV(E);
    Entity e = E.w.create("e");
    int id = E.add(e, "scripts/sb.lua",
        "function on_start(self)\n"
        "  self.gone = tostring(io) .. tostring(os) .. tostring(dofile) .. tostring(load) .. tostring(loadfile)\n"
        "    .. tostring(require) .. tostring(debug) .. tostring(package) .. tostring(loadstring) .. tostring(warn)\n"
        "  self.libs = type(string.rep) .. type(table.concat) .. type(math.floor) .. type(utf8.char) .. type(coroutine.create)\n"
        "  self.method = ('abc'):upper()\n"
        "  self.math = math.floor(2.7) + math.max(1, 5) + #string.format('%5.2f', 3.14159)\n"
        "  self.g_is_env = (_G ~= nil) and (_G.print == print)\n"
        "  _G.mine = 5\n"
        "  self.mine = mine\n"
        "  local co = coroutine.wrap(function(a) local b = coroutine.yield(a + 1); return b * 2 end)\n"
        "  self.co = co(1) + co(10)\n"
        "  self.pc = select('#', pcall(error, 'x'))\n"
        "end\n");
    int other = E.add(e, "scripts/sb2.lua", "function on_start(self) self.saw_mine = (mine == nil) end\n");
    E.w.progress(0.1f);
    SCRIPT_CHECK(id > 0 && E.str_is(id, "gone", "nilnilnilnilnilnilnilnilnilnil"));
    SCRIPT_CHECK(E.str_is(id, "libs", "functionfunctionfunctionfunctionfunction") && E.str_is(id, "method", "ABC"));
    SCRIPT_CHECK(near_(E.num(id, "math"), 2 + 5 + 5) && E.flag(id, "g_is_env") && near_(E.num(id, "mine"), 5));
    SCRIPT_CHECK(near_(E.num(id, "co"), 22) && near_(E.num(id, "pc"), 2));
    SCRIPT_CHECK(E.flag(other, "saw_mine"));              // globals are per instance
    SCRIPT_CHECK(E.errors == 0);
}

// ---- destroyed entities
inline void check_destroy() {
    SCRIPT_ENV(E);
    Entity killer = E.w.create("killer");
    Entity victim = E.w.create("victim");
    Entity kid = E.w.create("kid", victim);
    Entity bystander = E.w.create("bystander");
    ScriptProp p;
    strcpy(p.name, "victim"); p.type = PropType::Object;
    Entity objs[1] = { victim };
    int vid = E.add(victim, "scripts/v.lua", "function on_update(self, dt) self.n = (self.n or 0) + 1 end\n");
    int kidid = E.add(kid, "scripts/kid.lua", "function on_update(self, dt) self.n = (self.n or 0) + 1 end\n");
    int bid = E.add(bystander, "scripts/b.lua", "function on_update(self, dt) self.n = (self.n or 0) + 1 end\n");
    int kid2 = E.add(killer, "scripts/killer.lua",
        "function on_update(self, dt)\n"
        "  self.frame = (self.frame or 0) + 1\n"
        "  if self.frame == 1 then\n"
        "    self.h = self.victim\n"
        "    self.victim:destroy()\n"
        "    self.victim:destroy()\n"          // twice is fine
        "    self.alive1 = self.h.alive\n"     // false at once: destroy() is a request that ends the frame
        "  else\n"
        "    self.alive2 = self.h.alive\n"
        "    local ok, err = pcall(function() return self.h.name end)\n"
        "    self.err = not ok and err\n"
        "    local ok2, err2 = pcall(function() self.h.position = fe.vec3() end)\n"
        "    self.err2 = not ok2 and err2\n"
        "    self.str = tostring(self.h)\n"
        "    local ok3, err3 = pcall(function() return self.h:children() end)\n"
        "    self.err3 = ok3\n"
        "    self.eq = (self.h == self.victim)\n"
        "  end\n"
        "end\n", &p, 1, objs);
    SCRIPT_CHECK(vid > 0 && kidid > 0 && bid > 0 && kid2 > 0);
    E.w.progress(0.1f);
    SCRIPT_CHECK(!E.w.alive(victim) && !E.w.alive(kid) && E.w.alive(bystander));
    SCRIPT_CHECK(E.num(vid, "n") == -12345 || E.num(vid, "n") <= 1);
    SCRIPT_CHECK(E.rt.stats().instances == 2);
    E.w.progress(0.1f);
    SCRIPT_CHECK(!E.flag(kid2, "alive1") && !E.flag(kid2, "alive2") && !E.flag(kid2, "err3") && E.flag(kid2, "eq"));
    char buf[256];
    SCRIPT_CHECK(E.rt.get_string(kid2, "err", buf, sizeof buf) && strstr(buf, "no longer exists"));
    SCRIPT_CHECK(E.rt.get_string(kid2, "err2", buf, sizeof buf) && strstr(buf, "no longer exists"));
    SCRIPT_CHECK(E.str_is(kid2, "str", "object(destroyed)"));
    SCRIPT_CHECK(near_(E.num(bid, "n"), 2) && E.errors == 0);
    SCRIPT_CHECK(E.rt.stats().failed == 0);

    // destroyed behind the runtime's back: no callbacks, no crash
    Entity e2 = E.w.create("e2");
    int id2 = E.add(e2, "scripts/e2.lua", "function on_update(self, dt) self.n = (self.n or 0) + 1 end\n");
    E.w.progress(0.1f);
    SCRIPT_CHECK(near_(E.num(id2, "n"), 1));
    E.w.destroy(e2);
    E.w.progress(0.1f);
    E.w.progress(0.1f);
    SCRIPT_CHECK(E.num(id2, "n") == -12345 || near_(E.num(id2, "n"), 1));
    SCRIPT_CHECK(E.rt.stats().instances == 2);
    // remove_instances
    E.rt.remove_instances(bystander);
    E.w.progress(0.1f);
    SCRIPT_CHECK(E.num(bid, "n") == -12345 && E.rt.stats().instances == 1);
    // adding to a dead entity fails
    SCRIPT_CHECK(E.add(e2, "scripts/x.lua", "x = 1") == 0);
    // a script destroying its own object, from an event outside a frame
    Entity self_kill = E.w.create("suicide");
    int sk = E.add(self_kill, "scripts/sk.lua",
        "function on_trigger_enter(self, other) self.object:destroy(); self.after = self.object.alive end\n");
    E.rt.trigger_enter(self_kill, bystander);
    SCRIPT_CHECK(!E.w.alive(self_kill) && sk > 0);
    SCRIPT_CHECK(E.num(sk, "after") == -12345);               // the instance is gone
}

// ---- timing: many instances
inline void check_many(double* ms_per_frame, int n) {
    SCRIPT_ENV(E);
    Entity root = E.w.create("root");
    int first = 0, last = 0;
    for (int i = 0; i < n; i++) {
        Entity e = E.w.create(nullptr, root);
        int id = E.add(e, "scripts/many.lua",
            "function on_update(self, dt)\n"
            "  self.n = (self.n or 0) + 1\n"
            "  self.object.position = self.object.position + fe.vec3(dt, 0, 0)\n"
            "end\n");
        if (!first) first = id;
        last = id;
    }
    E.w.progress(0.016f);           // starts
    const int frames = 10;
    double total = 0;
    for (int f = 0; f < frames; f++) {
        E.w.progress(0.016f);
        total += E.rt.stats().update_ms;
    }
    *ms_per_frame = total / frames;
    SCRIPT_CHECK(E.rt.stats().instances == n && E.rt.stats().failed == 0);
    SCRIPT_CHECK(near_(E.num(first, "n"), frames + 1) && near_(E.num(last, "n"), frames + 1));
    SCRIPT_CHECK(E.errors == 0);
}

}  // namespace fe_script_checks
