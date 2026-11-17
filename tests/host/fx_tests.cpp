// Tests for the fx modules (engine/fx): particles, force fields, joints'
// scene-side behavior is in physics_tests, the rest here.
#include "ecs/ecs.h"
#include "fx/particles.h"
#include "fx/forcefield.h"
#include "gfx/particles.h"
#include "fx/fx.h"

#include <stdio.h>
#include <string.h>

using namespace fe;

static int g_checks, g_failed;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static float absf(float x) { return x < 0 ? -x : x; }

static void run_sim(ParticleSim& s, const ParticleEmitter& e, const Mat4& m, float seconds, float dt = 1.0f / 60.0f, const FieldVolume* f = nullptr, int nf = 0) {
    for (float t = 0; t < seconds - 1e-6f; t += dt) s.update(e, m, dt, f, nf, t);
}

static void particle_tests() {
    Mat4 id = Mat4::identity();
    // ---- the same seed gives the same particles; another seed does not
    {
        ParticleEmitter e;
        e.seed = 7;
        ParticleSim a, b, c;
        a.start(e, id); b.start(e, id);
        e.seed = 8;
        c.start(e, id);
        e.seed = 7;
        run_sim(a, e, id, 1.0f);
        run_sim(b, e, id, 1.0f);
        ParticleEmitter e2 = e;
        e2.seed = 8;
        run_sim(c, e2, id, 1.0f);
        CHECK(a.count() > 10 && a.count() == b.count(), "same seed, same count (%d vs %d)", a.count(), b.count());
        bool same = true;
        for (int i = 0; i < a.count(); i++)
            if (a.particles()[i].pos.x != b.particles()[i].pos.x || a.particles()[i].vel.y != b.particles()[i].vel.y) same = false;
        CHECK(same, "same seed: identical particles");
        bool differs = c.count() != a.count();
        for (int i = 0; !differs && i < a.count() && i < c.count(); i++)
            if (a.particles()[i].pos.x != c.particles()[i].pos.x) differs = true;
        CHECK(differs, "another seed: other particles");
    }
    // ---- emission rate and the pool cap
    {
        ParticleEmitter e;
        e.emission_rate = 100; e.lifetime_min = e.lifetime_max = 10; e.max_particles = 150;
        ParticleSim s;
        s.start(e, id);
        run_sim(s, e, id, 1.0f);
        CHECK(s.count() >= 97 && s.count() <= 103, "100/s for 1 s makes about 100 (%d)", s.count());
        run_sim(s, e, id, 2.0f);
        CHECK(s.count() == 150, "the pool is full at max_particles (%d)", s.count());
    }
    // ---- bursts
    {
        ParticleEmitter e;
        e.emission_rate = 0; e.burst_count = 40; e.burst_interval = 0; e.lifetime_min = e.lifetime_max = 10;
        ParticleSim s;
        s.start(e, id);
        s.update(e, id, 1.0f / 60.0f, nullptr, 0, 0);
        CHECK(s.count() == 40, "one burst of 40 (%d)", s.count());
        run_sim(s, e, id, 2.0f);
        CHECK(s.count() == 40, "no second burst with interval 0 (%d)", s.count());
        e.burst_interval = 0.5f;
        ParticleSim r;
        r.start(e, id);
        run_sim(r, e, id, 2.01f);
        CHECK(r.count() == 40 * 5, "bursts every 0.5 s: 1 + 4 in 2 s (%d)", r.count());
        s.emit(e, id, 7);
        CHECK(s.count() == 47, "emit(7) adds 7 (%d)", s.count());
        s.clear();
        CHECK(s.count() == 0, "clear");
    }
    // ---- lifetime, stop, duration
    {
        ParticleEmitter e;
        e.emission_rate = 50; e.lifetime_min = e.lifetime_max = 0.5f;
        ParticleSim s;
        s.start(e, id);
        run_sim(s, e, id, 2.0f);
        CHECK(s.count() >= 22 && s.count() <= 27, "steady state is rate * lifetime (%d)", s.count());
        s.stop();
        run_sim(s, e, id, 0.6f);
        CHECK(s.count() == 0 && !s.playing(), "after stop() the rest die out (%d)", s.count());
        e.looping = false; e.duration = 0.5f;
        ParticleSim d;
        d.start(e, id);
        run_sim(d, e, id, 0.4f);
        CHECK(d.playing(), "still emitting inside the duration");
        run_sim(d, e, id, 0.3f);
        CHECK(!d.playing(), "a non-looping emitter stops after its duration");
    }
    // ---- prewarm
    {
        ParticleEmitter e;
        e.emission_rate = 40; e.lifetime_min = e.lifetime_max = 1.0f; e.prewarm = true;
        ParticleSim s;
        s.start(e, id);
        CHECK(s.count() >= 30, "prewarm starts in flow (%d)", s.count());
    }
    // ---- gravity, drag, shapes
    {
        ParticleEmitter e;
        e.emission_rate = 0; e.burst_count = 1; e.speed_min = e.speed_max = 0; e.lifetime_min = e.lifetime_max = 5; e.gravity = 1.0f;
        e.shape = EmitShape::Point;
        ParticleSim s;
        s.start(e, id);
        run_sim(s, e, id, 1.0f);
        CHECK(s.count() == 1 && absf(s.particles()[0].vel.y + 9.81f) < 0.2f, "gravity 1 falls at 9.81 m/s^2 (vy %.2f)", s.particles()[0].vel.y);
        e.gravity = 0; e.drag = 5; e.speed_min = e.speed_max = 10;
        ParticleSim d;
        d.start(e, id);
        run_sim(d, e, id, 1.0f);
        float sp = length(d.particles()[0].vel);
        CHECK(sp < 3.0f && sp > 0.0f, "drag slows the particle (%.2f)", sp);
        // Cone: along -Z within the angle; Box: along -Z inside the box.
        ParticleEmitter c;
        c.emission_rate = 0; c.burst_count = 300; c.max_particles = 400; c.shape = EmitShape::Cone; c.cone_angle = 20; c.speed_min = c.speed_max = 1; c.lifetime_min = c.lifetime_max = 5;
        ParticleSim cs;
        cs.start(c, id);
        cs.update(c, id, 0.0001f, nullptr, 0, 0);
        bool ok = cs.count() == 300;
        for (int i = 0; ok && i < cs.count(); i++) {
            Vec3 v = normalize(cs.particles()[i].vel);
            if (v.z > -0.93f) ok = false;      // cos 20 deg = 0.94
        }
        CHECK(ok, "cone particles fly along -Z inside the angle");
        ParticleEmitter bx;
        bx.emission_rate = 0; bx.burst_count = 200; bx.shape = EmitShape::Box; bx.box_size = Vec3(2, 4, 6); bx.speed_min = bx.speed_max = 0; bx.lifetime_min = bx.lifetime_max = 5;
        ParticleSim bs;
        bs.start(bx, id);
        bs.update(bx, id, 0.0001f, nullptr, 0, 0);
        bool inside = true;
        Vec3 lo(9, 9, 9), hi(-9, -9, -9);
        for (int i = 0; i < bs.count(); i++) {
            Vec3 p = bs.particles()[i].pos;
            if (absf(p.x) > 1.001f || absf(p.y) > 2.001f || absf(p.z) > 3.001f) inside = false;
            lo = vmin(lo, p); hi = vmax(hi, p);
        }
        CHECK(inside && hi.z - lo.z > 4.5f && hi.y - lo.y > 3.0f, "box particles fill the box");
    }
    // ---- world space particles stay, local space particles follow the object
    {
        ParticleEmitter e;
        e.emission_rate = 0; e.burst_count = 1; e.speed_min = e.speed_max = 0; e.lifetime_min = e.lifetime_max = 5; e.shape = EmitShape::Point;
        ParticleSim w, l;
        w.start(e, id);
        ParticleEmitter el = e;
        el.space = ParticleSpace::Local;
        l.start(el, id);
        Mat4 moved = Mat4::translation(Vec3(5, 0, 0));
        w.update(e, id, 0.016f, nullptr, 0, 0);
        l.update(el, id, 0.016f, nullptr, 0, 0);
        w.update(e, moved, 0.016f, nullptr, 0, 0);
        l.update(el, moved, 0.016f, nullptr, 0, 0);
        gfx::Particle qw, ql;
        w.collect(e, moved, &qw, 1);
        l.collect(el, moved, &ql, 1);
        CHECK(absf(qw.pos.x) < 0.1f, "world space particle stayed (x %.2f)", qw.pos.x);
        CHECK(absf(ql.pos.x - 5) < 0.1f, "local space particle moved with the object (x %.2f)", ql.pos.x);
    }
    // ---- color and size over life
    {
        ParticleEmitter e;
        e.emission_rate = 0; e.burst_count = 1; e.speed_min = e.speed_max = 0; e.lifetime_min = e.lifetime_max = 1; e.shape = EmitShape::Point;
        e.size_start = 1; e.size_end = 0; e.alpha_start = 1; e.alpha_end = 0; e.color_start = Vec3(1, 0, 0); e.color_end = Vec3(0, 0, 1);
        ParticleSim s;
        s.start(e, id);
        run_sim(s, e, id, 0.5f);
        gfx::Particle q;
        CHECK(s.collect(e, id, &q, 1) == 1, "one particle");
        CHECK(absf(q.size - 0.5f) < 0.05f && absf(q.a - 0.5f) < 0.05f && absf(q.r - 0.5f) < 0.05f && absf(q.b - 0.5f) < 0.05f, "halfway through life: size %.2f alpha %.2f", q.size, q.a);
    }
    // ---- a force field pushes particles
    {
        ParticleEmitter e;
        e.emission_rate = 0; e.burst_count = 1; e.speed_min = e.speed_max = 0; e.lifetime_min = e.lifetime_max = 5; e.shape = EmitShape::Point;
        ForceField ff;
        ff.shape = FieldShape::Box; ff.size = Vec3(20, 20, 20); ff.direction = Vec3(1, 0, 0); ff.strength = 4;
        FieldVolume fv;
        CHECK(field_volume(ff, id, &fv), "field volume");
        ParticleSim s;
        s.start(e, id);
        run_sim(s, e, id, 1.0f, 1.0f / 60.0f, &fv, 1);
        CHECK(absf(s.particles()[0].vel.x - 4.0f) < 0.1f, "4 N on a 1 kg particle: 4 m/s after 1 s (%.2f)", s.particles()[0].vel.x);
        ForceField off = ff;
        off.affect_particles = false;
        field_volume(off, id, &fv);
        ParticleSim t;
        t.start(e, id);
        run_sim(t, e, id, 1.0f, 1.0f / 60.0f, &fv, 1);
        CHECK(absf(t.particles()[0].vel.x) < 0.01f, "Pushes particles off: no effect");
    }
}

static void field_tests() {
    Mat4 id = Mat4::identity();
    ForceField f;
    f.shape = FieldShape::Sphere; f.radius = 4; f.mode = FieldMode::Radial; f.strength = 10; f.falloff = FieldFalloff::Linear;
    FieldVolume v;
    CHECK(field_volume(f, id, &v), "volume");
    Vec3 a = field_force(v, Vec3(2, 0, 0), 0);
    CHECK(a.x > 4.9f && a.x < 5.1f && absf(a.y) < 1e-4f, "radial, linear falloff: half way is half force (%.2f)", a.x);
    CHECK(length(field_force(v, Vec3(5, 0, 0), 0)) == 0, "zero outside");
    f.strength = -10;
    field_volume(f, id, &v);
    CHECK(field_force(v, Vec3(2, 0, 0), 0).x < -4.9f, "negative strength pulls in");
    f.mode = FieldMode::Vortex; f.strength = 10; f.falloff = FieldFalloff::None;
    field_volume(f, id, &v);
    Vec3 t = field_force(v, Vec3(2, 0, 0), 0);
    CHECK(absf(t.x) < 1e-3f && absf(t.z) > 9.9f && absf(t.y) < 1e-3f, "vortex: tangent around Y (%.2f %.2f %.2f)", t.x, t.y, t.z);
    // Directional follows the object's turn and scale sets the volume.
    ForceField d;
    d.shape = FieldShape::Box; d.size = Vec3(2, 2, 2); d.direction = Vec3(0, 0, -1); d.strength = 3;
    Mat4 m = Mat4::trs(Vec3(0, 0, 0), Quat::axis_angle(Vec3(0, 1, 0), 3.14159265f * 0.5f), Vec3(2, 2, 2));
    field_volume(d, m, &v);
    Vec3 w = field_force(v, Vec3(1.5f, 0, 0), 0);
    CHECK(absf(w.x + 3.0f) < 0.05f, "-Z turned 90 degrees about Y points to -X (%.2f %.2f %.2f)", w.x, w.y, w.z);
    CHECK(length(field_force(v, Vec3(2.5f, 0, 0), 0)) == 0 && length(field_force(v, Vec3(1.9f, 0, 0), 0)) > 0, "scale 2 makes the 2 m box 4 m wide");
    // Turbulence is deterministic and varies.
    d.turbulence = 1; d.turbulence_scale = 2;
    field_volume(d, id, &v);
    Vec3 t1 = field_force(v, Vec3(0.3f, 0.2f, 0.1f), 0), t2 = field_force(v, Vec3(0.3f, 0.2f, 0.1f), 0), t3 = field_force(v, Vec3(0.8f, 0.2f, 0.1f), 1.5f);
    CHECK(t1.x == t2.x && t1.z == t2.z, "turbulence: same point and time, same force");
    CHECK(absf(t1.x - t3.x) + absf(t1.y - t3.y) + absf(t1.z - t3.z) > 0.1f, "turbulence varies in space and time");
    d.enabled = false;
    CHECK(!field_volume(d, id, &v), "a disabled field has no volume");
}

// The renderer's particle pass.
static void raster_tests() {
    const int W = 64, H = 48;
    uint32_t px[W * H];
    for (int i = 0; i < W * H; i++) px[i] = 0xFF000000u;       // opaque black, RGBA with R low
    Mat4 view = Mat4::look_at(Vec3(0, 0, 5), Vec3(0, 0, 0), Vec3(0, 1, 0));
    Mat4 proj = Mat4::perspective(radians(60.0f), (float)W / H, 0.1f, 100.0f);
    gfx::Particle p;
    p.pos = Vec3(0, 0, 0); p.size = 2.0f; p.r = 1; p.g = 0.5f; p.b = 0.0f; p.a = 1.0f;
    gfx::ParticleBatch b;
    b.items = &p; b.count = 1; b.additive = true;
    gfx::ParticleRaster r;
    int n = r.draw(&b, 1, view, proj, nullptr, 0, px, W, H, 0, H, true, nullptr, nullptr);
    CHECK(n == 1, "one quad drawn");
    uint32_t c = px[(H / 2) * W + W / 2];
    CHECK((c & 0xFF) > 200 && ((c >> 8) & 0xFF) > 90 && ((c >> 16) & 0xFF) < 10, "additive orange at the center (%08x)", c);
    CHECK(px[0] == 0xFF000000u, "corners untouched");
    // Alpha blend over white at half alpha.
    for (int i = 0; i < W * H; i++) px[i] = 0xFFFFFFFFu;
    p.a = 0.5f; p.r = 0; p.g = 0; p.b = 0;
    b.additive = false;
    r.draw(&b, 1, view, proj, nullptr, 0, px, W, H, 0, H, true, nullptr, nullptr);
    c = px[(H / 2) * W + W / 2];
    CHECK((c & 0xFF) > 115 && (c & 0xFF) < 140, "alpha blend darkens white to about half (%08x)", c);
    // A wall in front hides it (depth test).
    for (int i = 0; i < W * H; i++) px[i] = 0xFF000000u;
    p.a = 1; p.r = 1; p.g = 1; p.b = 1;
    b.additive = true;
    static const Vec3 wall[6] = { Vec3(-3, -3, 0), Vec3(3, -3, 0), Vec3(3, 3, 0), Vec3(-3, -3, 0), Vec3(3, 3, 0), Vec3(-3, 3, 0) };
    gfx::Occluder oc;
    oc.positions = wall; oc.vertex_count = 6; oc.model = Mat4::translation(Vec3(0, 0, 2));       // between camera (z 5) and the particle (z 0)
    r.draw(&b, 1, view, proj, &oc, 1, px, W, H, 0, H, true, nullptr, nullptr);
    CHECK(px[(H / 2) * W + W / 2] == 0xFF000000u, "a wall in front hides the particle");
    oc.model = Mat4::translation(Vec3(0, 0, -2));                                                 // behind it
    r.draw(&b, 1, view, proj, &oc, 1, px, W, H, 0, H, true, nullptr, nullptr);
    CHECK((px[(H / 2) * W + W / 2] & 0xFF) > 200, "a wall behind does not");
    // Band rendering writes only its rows, in XRGB.
    uint32_t band[W * 8];
    for (int i = 0; i < W * 8; i++) band[i] = 0;
    p.size = 8;
    r.draw(&b, 1, view, proj, nullptr, 0, band, W, H, H / 2 - 4, 8, false, nullptr, nullptr);
    CHECK((band[4 * W + W / 2] & 0xFF) > 200 && (band[4 * W + W / 2] >> 16 & 0xFF) > 200, "band: the particle's rows are drawn (%08x)", band[4 * W + W / 2]);
}

// The ECS side.
static void system_tests() {
    World w;
    CHECK(w.init(), "world");
    ParticleSystem ps;
    CHECK(ps.init(&w), "particle system");
    Entity e = w.create("Fire");
    w.add(e, ModuleKind::ParticleEmitter);
    ParticleEmitter* m = (ParticleEmitter*)w.get_mut(e, ModuleKind::ParticleEmitter);
    m->emission_rate = 60; m->lifetime_min = m->lifetime_max = 100; m->speed_min = m->speed_max = 0;
    for (int i = 0; i < 60; i++) w.progress(1.0f / 60.0f);
    m = (ParticleEmitter*)w.get_mut(e, ModuleKind::ParticleEmitter);
    CHECK(m->playing && m->particle_count >= 55 && m->particle_count <= 62, "play_on_start emits (%d alive)", m->particle_count);
    CHECK(ps.stop(e), "stop");
    for (int i = 0; i < 30; i++) w.progress(1.0f / 60.0f);
    int after_stop = ((const ParticleEmitter*)w.get(e, ModuleKind::ParticleEmitter))->particle_count;
    CHECK(!((const ParticleEmitter*)w.get(e, ModuleKind::ParticleEmitter))->playing && after_stop == m->particle_count, "stopped: no new particles (%d)", after_stop);
    CHECK(ps.emit(e, 25), "emit");
    CHECK(((const ParticleEmitter*)w.get(e, ModuleKind::ParticleEmitter))->particle_count == after_stop + 25, "emit(25) adds 25");
    CHECK(ps.clear(e) && ((const ParticleEmitter*)w.get(e, ModuleKind::ParticleEmitter))->particle_count == 0, "clear");
    CHECK(ps.play(e), "play");
    for (int i = 0; i < 20; i++) w.progress(1.0f / 60.0f);
    CHECK(((const ParticleEmitter*)w.get(e, ModuleKind::ParticleEmitter))->particle_count > 10, "play() emits again");
    // set_field playing = false stops it.
    CHECK(w.set_field(e, ModuleKind::ParticleEmitter, "playing", FieldValue::from_bool(false)), "set playing");
    int c0 = 0;
    for (int i = 0; i < 10; i++) w.progress(1.0f / 60.0f);
    c0 = ((const ParticleEmitter*)w.get(e, ModuleKind::ParticleEmitter))->particle_count;
    for (int i = 0; i < 10; i++) w.progress(1.0f / 60.0f);
    CHECK(c0 == ((const ParticleEmitter*)w.get(e, ModuleKind::ParticleEmitter))->particle_count, "playing = false stops emission");
    // Frames of batches for the renderer.
    const gfx::ParticleBatch* batches = nullptr;
    int nb = ps.collect(Vec3(0, 0, 10), &batches);
    CHECK(nb == 1 && batches[0].count == c0 && batches[0].additive, "collect gives one additive batch (%d batches)", nb);
    // A destroyed emitter is dropped.
    w.destroy(e);
    w.progress(1.0f / 60.0f);
    CHECK(ps.collect(Vec3(0, 0, 10), &batches) == 0, "a destroyed emitter leaves no batch");
    // A force field on the world pushes the particles of an emitter.
    Entity f = w.create("Wind");
    w.add(f, ModuleKind::ForceField);
    ForceField* ff = (ForceField*)w.get_mut(f, ModuleKind::ForceField);
    ff->size = Vec3(100, 100, 100); ff->direction = Vec3(1, 0, 0); ff->strength = 5;
    Entity e2 = w.create("Smoke");
    w.add(e2, ModuleKind::ParticleEmitter);
    ParticleEmitter* m2 = (ParticleEmitter*)w.get_mut(e2, ModuleKind::ParticleEmitter);
    m2->emission_rate = 0; m2->burst_count = 20; m2->speed_min = m2->speed_max = 0; m2->lifetime_min = m2->lifetime_max = 50; m2->shape = EmitShape::Point;
    for (int i = 0; i < 60; i++) w.progress(1.0f / 60.0f);
    nb = ps.collect(Vec3(0, 0, 10), &batches);
    CHECK(nb == 1 && batches[0].count == 20 && batches[0].items[0].pos.x > 1.5f, "the wind pushed the particles (x %.2f)", nb ? batches[0].items[0].pos.x : -1.0f);
    ps.shutdown();
    w.shutdown();
}

static int g_spawns;
static Entity g_last[64];
static Entity mock_spawn(void* user, Entity src, const Mat4& m) {
    World* w = (World*)user;
    Entity e = w->create(nullptr);
    Vec3 t, sc;
    Quat r;
    decompose(m, &t, &r, &sc);
    w->transform(e)->position = t;
    w->add(e, ModuleKind::RigidBody3D);
    RigidBody3D* rb = (RigidBody3D*)w->get_mut(e, ModuleKind::RigidBody3D);
    const RigidBody3D* srb = (const RigidBody3D*)w->get(src, ModuleKind::RigidBody3D);
    if (srb) rb->velocity = srb->velocity;
    g_last[g_spawns % 64] = e;
    g_spawns++;
    return e;
}

static void spawner_tests() {
    World w;
    w.init();
    FxWorld fx;
    FxHost host;
    host.user = &w;
    host.spawn = mock_spawn;
    CHECK(fx.init(&w, host), "fx init");
    Entity tmpl = w.create("Template");
    w.add(tmpl, ModuleKind::RigidBody3D);
    w.bind_link(5, tmpl);
    Entity sp = w.create("Spawner");
    w.transform(sp)->position = Vec3(10, 0, 0);
    w.add(sp, ModuleKind::Spawner);
    auto M = [&]() { return (Spawner*)w.get_mut(sp, ModuleKind::Spawner); };
    auto run = [&](float secs) { for (float t = 0; t < secs - 1e-6f; t += 1.0f / 60.0f) { w.progress(1.0f / 60.0f); fx.post_frame(1.0f / 60.0f); } };
    M()->source = 5; M()->interval = 0.5f; M()->max_alive = 3;
    run(3.01f);
    CHECK(M()->spawned == 6 && M()->alive == 3, "every 0.5 s for 3 s: 6 made, the newest 3 alive (%d, %d)", M()->spawned, M()->alive);
    Entity first_left = 0;
    (void)first_left;
    // total
    fx.spawner.clear(sp);
    CHECK(M()->alive == 0, "clear() destroys the copies");
    M()->total = 8; M()->max_alive = 50;
    run(5.0f);
    CHECK(M()->spawned == 8, "total 8 stops the spawner (%d)", M()->spawned);
    // lifetime
    fx.spawner.clear(sp);
    M()->total = 0; M()->lifetime = 0.8f; M()->interval = 0.2f; M()->spawned = 0;
    run(3.0f);
    CHECK(M()->alive >= 3 && M()->alive <= 5, "lifetime 0.8 s at 5/s: about 4 alive (%d)", M()->alive);
    // position, offset, velocity, inherit
    fx.spawner.clear(sp);
    M()->auto_spawn = false;
    M()->offset = Vec3(2, 0, 0); M()->velocity = Vec3(0, 3, 0); M()->lifetime = 0;
    int before = g_spawns;
    for (int i = 0; i < 20; i++) fx.spawner.spawn(sp);
    CHECK(g_spawns == before + 20 && M()->alive == 20, "spawn() makes one now (%d)", M()->alive);
    // (positions: found through the children-less roots)
    float minx = 99, maxx = -99, vy = 0;
    int found = 0;
    Entity all[64];
    (void)all;
    // The copies are the entities with a rigidbody that are not the template.
    // Walk ids from the last created downwards.
    for (int i = 0; i < 20; i++) {
        Entity e = g_last[(g_spawns - 1 - i) % 64];
        if (!w.alive(e)) continue;
        Vec3 p = w.transform(e)->position;
        found++;
        if (p.x < minx) minx = p.x;
        if (p.x > maxx) maxx = p.x;
        vy = ((const RigidBody3D*)w.get(e, ModuleKind::RigidBody3D))->velocity.y;
    }
    CHECK(found == 20 && minx >= 7.99f && maxx <= 12.01f && maxx - minx > 1.5f, "copies appear at the spawner within the random offset (x %.2f..%.2f, %d)", minx, maxx, found);
    CHECK(absf(vy - 3.0f) < 0.01f, "with the added velocity (%.2f)", vy);
    // no template: nothing
    M()->source = 0;
    int c = M()->spawned;
    CHECK(!fx.spawner.spawn(sp) && M()->spawned == c, "no template, no copy");
    // determinism: the same seed gives the same offsets
    fx.shutdown();
    w.shutdown();
}

static void quality_tests() {
    Mat4 id = Mat4::identity();
    ParticleEmitter e;
    e.emission_rate = 100; e.lifetime_min = e.lifetime_max = 10; e.max_particles = 1000;
    const gfx::ParticleBatch* b = nullptr;
    ParticleBank full, half, off;
    half.quality.density = 0.5f;
    off.quality.particles = false;
    for (int i = 0; i < 60; i++) {
        ParticleBank* banks[3] = { &full, &half, &off };
        for (ParticleBank* k : banks) { k->begin(); k->update(1, e, id, 1.0f / 60.0f, nullptr, 0, 0); k->end(); }
    }
    int nf = full.collect(Vec3(0, 0, 5), &b), cf = nf ? b[0].count : 0;
    int nh = half.collect(Vec3(0, 0, 5), &b), ch = nh ? b[0].count : 0;
    CHECK(cf >= 97 && ch >= 47 && ch <= 53, "density 0.5 halves the particles (%d vs %d)", ch, cf);
    CHECK(off.collect(Vec3(0, 0, 5), &b) == 0 && off.sim_count() == 0, "particles off: nothing simulated or drawn");
}

int main() {
    particle_tests();
    field_tests();
    raster_tests();
    system_tests();
    quality_tests();
    spawner_tests();
    printf("fx_tests: %d checks, %d failures\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
