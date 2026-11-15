// ECS checks shared by the host tests (tests/host/ecs_tests.cpp) and the
// Minimal-OS smoke program (tests/os/ecs_smoke). Public API only (no flecs).
// The includer defines ECS_CHECK(cond) before including this header.
#pragma once

#include "ecs/ecs.h"
#include <math.h>
#include <string.h>

namespace fe_ecs_checks {

using namespace fe;

inline bool near_(float a, float b) { return fabsf(a - b) < 1e-4f; }

inline Vec3 translation(const World& w, Entity e) {
    const WorldTransform* t = w.world_transform(e);
    return t ? Vec3(t->matrix.m[12], t->matrix.m[13], t->matrix.m[14]) : Vec3(-999, -999, -999);
}
#define ECS_CHECK_VEC(v, X, Y, Z) \
    ECS_CHECK(::fe_ecs_checks::near_((v).x, X) && ::fe_ecs_checks::near_((v).y, Y) && ::fe_ecs_checks::near_((v).z, Z))

// ---- entities, names, defaults
inline void check_entities(World& w) {
    Entity a = w.create("alpha");
    ECS_CHECK(a != 0 && w.alive(a));
    ECS_CHECK(w.lookup("alpha") == a);
    ECS_CHECK(w.name(a) && strcmp(w.name(a), "alpha") == 0);
    ECS_CHECK(w.create("alpha") == 0);                  // names are unique among siblings
    ECS_CHECK(w.lookup("nope") == 0);

    Entity anon = w.create();
    ECS_CHECK(anon != 0 && w.name(anon) == nullptr);
    ECS_CHECK(w.set_name(anon, "Cube.001"));            // dots are plain text
    ECS_CHECK(w.lookup("Cube.001") == anon);
    ECS_CHECK(!w.set_name(anon, "alpha"));
    ECS_CHECK(w.set_name(a, "beta"));
    ECS_CHECK(w.lookup("alpha") == 0 && w.lookup("beta") == a);
    ECS_CHECK(w.create("Camera") != 0 && w.create("Light") != 0);     // do not collide with component names
    ECS_CHECK(w.create("Transform") != 0 && w.create("Sky") != 0);
    ECS_CHECK(w.create("fe") == 0 && w.create("flecs") == 0);          // the two reserved root names
    ECS_CHECK(w.create("fe", w.lookup("Camera")) != 0);                // fine below a parent

    // defaults
    const Transform* t = w.transform(a);
    ECS_CHECK(t != nullptr);
    ECS_CHECK_VEC(t->position, 0, 0, 0);
    ECS_CHECK_VEC(t->scale, 1, 1, 1);
    ECS_CHECK(t->rotation.w == 1 && t->rotation.x == 0);
    const WorldTransform* wt = w.world_transform(a);
    ECS_CHECK(wt && wt->matrix.m[0] == 1 && wt->matrix.m[5] == 1 && wt->matrix.m[15] == 1);

    // renderable
    ECS_CHECK(w.renderable(a) == nullptr);
    Renderable* r = w.add_renderable(a);
    ECS_CHECK(r && r->visible && r->mesh == 0 && r->model_path[0] == 0);
    ECS_CHECK_VEC(r->color, 1, 1, 1);
    r->mesh = 3;
    ECS_CHECK(w.add_renderable(a)->mesh == 3);          // kept
    w.remove_renderable(a);
    ECS_CHECK(w.renderable(a) == nullptr);

    w.destroy(a);
    ECS_CHECK(!w.alive(a) && w.lookup("beta") == 0);
    ECS_CHECK(w.transform(a) == nullptr && w.get(a, ModuleKind::Light) == nullptr);
    w.destroy(a);                                       // twice is fine
    w.destroy(0);
    ECS_CHECK(w.create("beta") != 0);                   // the name is free again
}

// ---- hierarchy and the transform system
inline void check_hierarchy(World& w) {
    // three levels, created child first
    Entity g = w.create("g"), c = w.create("c"), r = w.create("r");
    ECS_CHECK(w.set_parent(g, c) && w.set_parent(c, r));
    ECS_CHECK(w.parent(g) == c && w.parent(c) == r && w.parent(r) == 0);
    w.transform(r)->position = Vec3(1, 0, 0);
    w.transform(c)->position = Vec3(0, 2, 0);
    w.transform(g)->position = Vec3(0, 0, 3);
    w.progress(0.016f);
    Vec3 p = translation(w, g);
    ECS_CHECK_VEC(p, 1, 2, 3);
    ECS_CHECK_VEC(translation(w, c), 1, 2, 0);
    ECS_CHECK(w.lookup_child(r, "c") == c && w.lookup("c") == 0);   // c is not a root any more

    // rotation and scale compose
    w.transform(r)->position = Vec3(10, 0, 0);
    w.transform(r)->rotation = Quat::axis_angle(Vec3(0, 0, 1), radians(90));
    w.transform(r)->scale = Vec3(2, 2, 2);
    w.transform(c)->position = Vec3(1, 0, 0);
    w.transform(g)->position = Vec3(0, 0, 0);
    w.progress(0.016f);
    ECS_CHECK_VEC(translation(w, c), 10, 2, 0);
    ECS_CHECK_VEC(translation(w, g), 10, 2, 0);

    // reparent g to the root
    w.transform(r)->rotation = Quat();
    w.transform(r)->scale = Vec3(1, 1, 1);
    w.transform(r)->position = Vec3(1, 0, 0);
    w.transform(c)->position = Vec3(0, 2, 0);
    w.transform(g)->position = Vec3(0, 0, 3);
    ECS_CHECK(w.set_parent(g, r));
    w.progress(0.016f);
    ECS_CHECK(w.parent(g) == r);
    ECS_CHECK_VEC(translation(w, g), 1, 0, 3);
    ECS_CHECK(w.child_count(r) == 2 && w.child_count(c) == 0);
    Entity kids[4];
    ECS_CHECK(w.children(r, kids, 4) == 2);
    ECS_CHECK((kids[0] == g && kids[1] == c) || (kids[0] == c && kids[1] == g));
    ECS_CHECK(w.set_parent(g, 0));                      // to a root
    w.progress(0.016f);
    ECS_CHECK(w.parent(g) == 0);
    ECS_CHECK_VEC(translation(w, g), 0, 0, 3);

    // no cycles, no self parent, no name clash
    ECS_CHECK(!w.set_parent(r, c) && !w.set_parent(r, r) && !w.set_parent(r, g + 1000000));
    Entity r2 = w.create("dup"), r3 = w.create("rr");
    Entity k1 = w.create("same", r2), k2 = w.create("same", r3);
    ECS_CHECK(k1 && k2 && !w.set_parent(k2, r2) && w.parent(k2) == r3);
    ECS_CHECK(w.create("same", r2) == 0);

    // destroying a parent removes its children
    Entity gc = w.create("gc", c);
    ECS_CHECK(w.alive(gc));
    w.destroy(r);
    ECS_CHECK(!w.alive(r) && !w.alive(c) && !w.alive(gc) && w.alive(g));
}

// ---- modules
inline void check_modules(World& w) {
    static const ModuleSet defaults;     // static: zeroed padding, so memcmp is exact
    Entity e = w.create("m");
    for (int k = 0; k < kModuleKindCount; k++) {
        ModuleKind kind = (ModuleKind)k;
        const ModuleInfo& mi = module_info(kind);
        ECS_CHECK(!w.has(e, kind) && w.get(e, kind) == nullptr && w.get_mut(e, kind) == nullptr);
        ECS_CHECK(w.add(e, kind) && w.has(e, kind));
        const void* d = w.get(e, kind);
        ECS_CHECK(d != nullptr && memcmp(d, defaults.data(kind), mi.size) == 0);   // defaults match the struct
        ECS_CHECK(w.get_mut(e, kind) == d);

        // set copies the data in
        ModuleSet s;
        memcpy(s.data(kind), defaults.data(kind), mi.size);
        w.remove(e, kind);
        ECS_CHECK(!w.has(e, kind));
        if (mi.field_count > 0 && mi.fields[0].type == FieldType::Float) *(float*)((char*)s.data(kind) + mi.fields[0].offset) = 0.75f;
        ECS_CHECK(w.set(e, kind, s.data(kind)) && w.has(e, kind));
        if (mi.field_count > 0 && mi.fields[0].type == FieldType::Float) {
            FieldValue v;
            ECS_CHECK(w.get_field(e, kind, mi.fields[0].name, &v) && near_(v.f, 0.75f));
        }
        w.remove(e, kind);
        ECS_CHECK(!w.has(e, kind));
    }
    ECS_CHECK(!w.set(e, ModuleKind::Light, nullptr));

    // load / store
    ModuleSet s;
    s.add(ModuleKind::Light);
    s.light.intensity = 3.5f;
    s.light.type = LightType::Spot;
    s.add(ModuleKind::Collider3D);
    s.collider3d.radius = 2.0f;
    s.add(ModuleKind::AudioStream);
    strcpy(s.audio_stream.path, "music/theme.ogg");
    Entity e2 = w.create("loaded");
    w.load(e2, s);
    ECS_CHECK(w.has(e2, ModuleKind::Light) && w.has(e2, ModuleKind::Collider3D) && w.has(e2, ModuleKind::AudioStream));
    ECS_CHECK(!w.has(e2, ModuleKind::Camera) && !w.has(e2, ModuleKind::RigidBody3D));
    ModuleSet back;
    w.store(e2, &back);
    ECS_CHECK(back.present == s.present);
    ECS_CHECK(near_(back.light.intensity, 3.5f) && back.light.type == LightType::Spot);
    ECS_CHECK(near_(back.collider3d.radius, 2.0f) && strcmp(back.audio_stream.path, "music/theme.ogg") == 0);

    // a module added next to others keeps them
    w.add(e2, ModuleKind::RigidBody3D);
    ECS_CHECK(w.has(e2, ModuleKind::Light) && near_(((const Light*)w.get(e2, ModuleKind::Light))->intensity, 3.5f));
    ECS_CHECK(w.add(e2, ModuleKind::Light) && near_(((const Light*)w.get(e2, ModuleKind::Light))->intensity, 3.5f));
}

// ---- fields by name
inline void check_fields(World& w) {
    Entity e = w.create("f");
    FieldValue v;
    ECS_CHECK(!w.set_field(e, ModuleKind::Light, "intensity", FieldValue::from_float(1)));   // no Light yet
    ECS_CHECK(!w.get_field(e, ModuleKind::Light, "intensity", &v));
    w.add(e, ModuleKind::Light);
    w.add(e, ModuleKind::Collider3D);
    w.add(e, ModuleKind::AudioStream);
    w.add(e, ModuleKind::AudioSampler);

    // float
    ECS_CHECK(w.set_field(e, ModuleKind::Light, "intensity", FieldValue::from_float(2.5f)));
    ECS_CHECK(w.get_field(e, ModuleKind::Light, "intensity", &v) && v.type == FieldType::Float && near_(v.f, 2.5f));
    ECS_CHECK(near_(((const Light*)w.get(e, ModuleKind::Light))->intensity, 2.5f));
    ECS_CHECK(w.set_field(e, ModuleKind::Light, "intensity", FieldValue::from_int(4)));      // int converts
    ECS_CHECK(w.get_field(e, ModuleKind::Light, "intensity", &v) && near_(v.f, 4.0f));
    // clamp to the FieldInfo range
    const FieldInfo* fr = find_field(module_info(ModuleKind::Collider3D), "friction");
    ECS_CHECK(fr && fr->min != fr->max);
    ECS_CHECK(w.set_field(e, ModuleKind::Collider3D, "friction", FieldValue::from_float(5)));
    ECS_CHECK(w.get_field(e, ModuleKind::Collider3D, "friction", &v) && near_(v.f, fr->max));
    ECS_CHECK(w.set_field(e, ModuleKind::Collider3D, "friction", FieldValue::from_float(-5)));
    ECS_CHECK(w.get_field(e, ModuleKind::Collider3D, "friction", &v) && near_(v.f, fr->min));
    ECS_CHECK(!w.set_field(e, ModuleKind::Collider3D, "friction", FieldValue::from_float(NAN)));
    ECS_CHECK(near_(v.f, fr->min));
    // int, clamped
    ECS_CHECK(w.set_field(e, ModuleKind::AudioSampler, "voices", FieldValue::from_int(1000)));
    ECS_CHECK(w.get_field(e, ModuleKind::AudioSampler, "voices", &v) && v.type == FieldType::Int && v.i == 32);
    ECS_CHECK(w.set_field(e, ModuleKind::AudioSampler, "voices", FieldValue::from_float(2.6f)));
    ECS_CHECK(w.get_field(e, ModuleKind::AudioSampler, "voices", &v) && v.i == 3);
    // bool
    ECS_CHECK(w.set_field(e, ModuleKind::Light, "shadows", FieldValue::from_bool(false)));
    ECS_CHECK(w.get_field(e, ModuleKind::Light, "shadows", &v) && v.type == FieldType::Bool && !v.b);
    ECS_CHECK(!w.set_field(e, ModuleKind::Light, "shadows", FieldValue::from_float(1)));     // wrong kind
    ECS_CHECK(!w.set_field(e, ModuleKind::Light, "intensity", FieldValue::from_bool(true)));
    // enum: index or name, bounds checked
    ECS_CHECK(w.set_field(e, ModuleKind::Light, "type", FieldValue::from_enum(2)));
    ECS_CHECK(w.get_field(e, ModuleKind::Light, "type", &v) && v.type == FieldType::Enum && v.i == 2 && strcmp(v.s, "Spot") == 0);
    ECS_CHECK(((const Light*)w.get(e, ModuleKind::Light))->type == LightType::Spot);
    ECS_CHECK(!w.set_field(e, ModuleKind::Light, "type", FieldValue::from_enum(3)));
    ECS_CHECK(!w.set_field(e, ModuleKind::Light, "type", FieldValue::from_enum(-1)));
    ECS_CHECK(w.get_field(e, ModuleKind::Light, "type", &v) && v.i == 2);
    ECS_CHECK(w.set_field(e, ModuleKind::Light, "type", FieldValue::from_string("Directional")));
    ECS_CHECK(w.get_field(e, ModuleKind::Light, "type", &v) && v.i == 0);
    ECS_CHECK(!w.set_field(e, ModuleKind::Light, "type", FieldValue::from_string("Bogus")));
    // vec3 / color
    ECS_CHECK(w.set_field(e, ModuleKind::Light, "color", FieldValue::from_color(Vec3(0.1f, 0.2f, 0.3f))));
    ECS_CHECK(w.get_field(e, ModuleKind::Light, "color", &v) && v.type == FieldType::Color);
    ECS_CHECK_VEC(v.v, 0.1f, 0.2f, 0.3f);
    ECS_CHECK(w.set_field(e, ModuleKind::Collider3D, "center", FieldValue::from_vec3(Vec3(1, 2, 3))));
    ECS_CHECK(w.get_field(e, ModuleKind::Collider3D, "center", &v) && v.type == FieldType::Vec3);
    ECS_CHECK_VEC(v.v, 1, 2, 3);
    // bits
    ECS_CHECK(w.set_field(e, ModuleKind::Collider3D, "layer", FieldValue::from_bits(0x80000005u)));
    ECS_CHECK(w.get_field(e, ModuleKind::Collider3D, "layer", &v) && v.type == FieldType::Bits && (uint32_t)v.i == 0x80000005u);
    ECS_CHECK(((const Collider3D*)w.get(e, ModuleKind::Collider3D))->layer == 0x80000005u);
    // path
    ECS_CHECK(w.set_field(e, ModuleKind::AudioStream, "path", FieldValue::from_string("a/b.ogg")));
    ECS_CHECK(w.get_field(e, ModuleKind::AudioStream, "path", &v) && v.type == FieldType::Path && strcmp(v.s, "a/b.ogg") == 0);
    char big[400];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    ECS_CHECK(w.set_field(e, ModuleKind::AudioStream, "path", FieldValue::from_string(big)));
    ECS_CHECK(w.get_field(e, ModuleKind::AudioStream, "path", &v) && strlen(v.s) == (size_t)kPathMax - 1);
    ECS_CHECK(!w.set_field(e, ModuleKind::AudioStream, "path", FieldValue::from_float(1)));
    // bad names
    ECS_CHECK(!w.set_field(e, ModuleKind::Light, "nope", FieldValue::from_float(1)));
    ECS_CHECK(!w.set_field(e, ModuleKind::Light, "", FieldValue::from_float(1)));
    ECS_CHECK(!w.set_field(e, ModuleKind::Light, nullptr, FieldValue::from_float(1)));
    ECS_CHECK(!w.get_field(e, ModuleKind::Light, "Intensity", &v));      // names are the Lua names
    ECS_CHECK(!w.get_field(e, ModuleKind::Light, "intensity", nullptr));
}

// ---- the current rule
inline bool flag(World& w, Entity e, ModuleKind k) {
    const void* d = w.get(e, k);
    return d && get_current(module_info(k), d);
}

inline void check_current(World& w) {
    const ModuleKind K = ModuleKind::Camera;
    ECS_CHECK(w.current(K) == 0);
    Entity a = w.create("camA"), b = w.create("camB"), c = w.create("camC");
    w.add(a, K); w.add(b, K); w.add(c, K);
    ECS_CHECK(w.current(K) == 0 && !flag(w, a, K));
    ECS_CHECK(w.set_current(a, K));
    ECS_CHECK(w.current(K) == a && flag(w, a, K) && !flag(w, b, K));
    ECS_CHECK(w.set_current(b, K));
    ECS_CHECK(w.current(K) == b && flag(w, b, K) && !flag(w, a, K));

    // through set_field
    FieldValue v;
    ECS_CHECK(w.set_field(a, K, "current", FieldValue::from_bool(true)));
    ECS_CHECK(w.current(K) == a && !flag(w, b, K) && w.get_field(a, K, "current", &v) && v.b);
    // through get_mut + modified
    ((Camera*)w.get_mut(b, K))->current = true;
    w.modified(b, K);
    ECS_CHECK(w.current(K) == b && !flag(w, a, K) && flag(w, b, K));
    // through set() with the whole struct
    Camera cam;
    cam.current = true;
    cam.fov = 90;
    ECS_CHECK(w.set(c, K, &cam));
    ECS_CHECK(w.current(K) == c && !flag(w, b, K) && near_(((const Camera*)w.get(c, K))->fov, 90));
    // set() with the flag off on the current one: no current
    cam.current = false;
    ECS_CHECK(w.set(c, K, &cam) && w.current(K) == 0);
    // clear
    w.set_current(a, K);
    w.clear_current(b, K);
    ECS_CHECK(w.current(K) == a);
    w.clear_current(a, K);
    ECS_CHECK(w.current(K) == 0 && !flag(w, a, K));
    // destroy the current one
    w.set_current(b, K);
    ECS_CHECK(w.current(K) == b);
    w.destroy(b);
    ECS_CHECK(w.current(K) == 0);
    // removing the module also
    w.set_current(a, K);
    w.remove(a, K);
    ECS_CHECK(w.current(K) == 0);
    ECS_CHECK(!w.set_current(a, K));                    // no Camera on it any more
    // load(): two flagged cameras, the last one wins
    ModuleSet s1, s2;
    s1.add(K); s1.camera.current = true;
    s2.add(K); s2.camera.current = true; s2.camera.fov = 33;
    Entity d1 = w.create(), d2 = w.create();
    w.load(d1, s1);
    w.load(d2, s2);
    ECS_CHECK(w.current(K) == d2 && !flag(w, d1, K));
    // a new camera added with defaults (flag off) does not steal it
    Entity d3 = w.create();
    w.add(d3, K);
    ECS_CHECK(w.current(K) == d2);

    // other kinds with a current flag are independent
    const ModuleKind kinds[] = { ModuleKind::SkyLight, ModuleKind::Sky, ModuleKind::PostProcess, ModuleKind::AudioListener };
    for (ModuleKind k : kinds) {
        ECS_CHECK(module_info(k).has_current && w.current(k) == 0);
        w.add(a, k); w.add(d1, k);
        ECS_CHECK(w.set_current(a, k) && w.current(k) == a);
        ECS_CHECK(w.set_field(d1, k, "current", FieldValue::from_bool(true)) && w.current(k) == d1 && !flag(w, a, k));
        ECS_CHECK(w.current(K) == d2);
    }

    // kinds without the flag reject it
    const ModuleKind plain[] = { ModuleKind::RigidBody3D, ModuleKind::Collider3D, ModuleKind::AudioStream,
                                 ModuleKind::AudioSampler, ModuleKind::Light };
    for (ModuleKind k : plain) {
        w.add(a, k);
        ECS_CHECK(!module_info(k).has_current);
        ECS_CHECK(!w.set_current(a, k) && w.current(k) == 0);
        ECS_CHECK(!w.set_field(a, k, "current", FieldValue::from_bool(true)));
        w.clear_current(a, k);
    }
}

// ---- frame: callbacks in phases, around the transform system
struct PhaseLog {
    int n = 0;
    int order[8] = {};
    float last_dt = 0;
    Vec3 seen_world;      // translation of `watch` as the Store callback saw it
    Entity watch = 0;
    Vec3 seen_pre;        // ... as the PreUpdate callback saw it
};
inline void log_pre(World& w, float dt, void* u) {
    PhaseLog* l = (PhaseLog*)u;
    l->order[l->n++] = 1;
    l->last_dt = dt;
    l->seen_pre = translation(w, l->watch);
    w.transform(l->watch)->position = Vec3(7, 8, 9);        // "scripts/physics" write Transform
}
inline void log_update(World& w, float dt, void* u) { PhaseLog* l = (PhaseLog*)u; l->order[l->n++] = 2; }
inline void log_post(World& w, float dt, void* u) { PhaseLog* l = (PhaseLog*)u; l->order[l->n++] = 3; }
inline void log_store(World& w, float dt, void* u) {
    PhaseLog* l = (PhaseLog*)u;
    l->order[l->n++] = 4;
    l->seen_world = translation(w, l->watch);
}

inline void check_frame(World& w) {
    PhaseLog log;
    log.watch = w.create("watched");
    Entity s1 = w.add_system(Phase::PreUpdate, log_pre, &log);
    Entity s2 = w.add_system(Phase::Update, log_update, &log);
    Entity s3 = w.add_system(Phase::PostUpdate, log_post, &log);
    Entity s4 = w.add_system(Phase::Store, log_store, &log);
    ECS_CHECK(s1 && s2 && s3 && s4);
    w.progress(0.25f);
    ECS_CHECK(log.n == 4 && log.order[0] == 1 && log.order[1] == 2 && log.order[2] == 3 && log.order[3] == 4);
    ECS_CHECK(near_(log.last_dt, 0.25f));
    ECS_CHECK_VEC(log.seen_world, 7, 8, 9);                 // Transform written in PreUpdate is in WorldTransform by Store
    ECS_CHECK_VEC(log.seen_pre, 0, 0, 0);                   // ... and was not yet when PreUpdate ran
    // systems can be removed again
    w.remove_system(s1);
    w.remove_system(s3);
    log.n = 0;
    w.progress(0.1f);
    ECS_CHECK(log.n == 2 && log.order[0] == 2 && log.order[1] == 4);
    w.remove_system(s2);
    w.remove_system(s4);
    log.n = 0;
    w.progress(0.1f);
    ECS_CHECK(log.n == 0);
}

}  // namespace fe_ecs_checks
