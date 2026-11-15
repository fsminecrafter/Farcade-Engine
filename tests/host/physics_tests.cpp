// Tests for the built-in physics step (engine/physics).
#include "physics/physics.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

using namespace fe;

static int g_checks, g_failed;
static float absf_(float x) { return x < 0 ? -x : x; }
static float maxf_(float a, float b) { return a > b ? a : b; }
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Events {
    int enters = 0, exits = 0, collisions = 0;
    Entity last_trigger = 0, last_other = 0;
};

static void on_enter(Entity t, Entity o, void* u) { Events* e = (Events*)u; e->enters++; e->last_trigger = t; e->last_other = o; }
static void on_exit(Entity t, Entity o, void* u) { Events* e = (Events*)u; e->exits++; (void)t; (void)o; }
static void on_collision(Entity a, Entity b, void* u) { ((Events*)u)->collisions++; (void)a; (void)b; }

static Entity make(World& w, const char* name, Vec3 pos, Vec3 scale = Vec3(1, 1, 1)) {
    Entity e = w.create(name);
    Transform* t = w.transform(e);
    t->position = pos;
    t->scale = scale;
    return e;
}

static Entity ground(World& w, PhysicsWorld& p) {
    // A 20 x 0.2 x 20 box whose top is at y = 0.
    Entity g = make(w, "Ground", Vec3(0, -0.1f, 0), Vec3(20, 0.2f, 20));
    w.add(g, ModuleKind::Collider3D);
    p.add(g);
    return g;
}

static void run(World& w, float seconds) {
    for (float t = 0; t < seconds; t += 1.0f / 60.0f) w.progress(1.0f / 60.0f);
}

int main() {
    // ---- a dynamic sphere falls and comes to rest on a static box
    {
        World w;
        CHECK(w.init(), "world");
        PhysicsWorld p;
        Events ev;
        PhysicsEvents pe;
        pe.trigger_enter = on_enter; pe.trigger_exit = on_exit; pe.collision = on_collision; pe.user = &ev;
        CHECK(p.init(&w, pe), "physics init");
        ground(w, p);
        Entity ball = make(w, "Ball", Vec3(0, 3, 0));
        w.add(ball, ModuleKind::RigidBody3D);
        w.add(ball, ModuleKind::Collider3D);
        FieldValue sphere = FieldValue::from_string("Sphere");
        CHECK(w.set_field(ball, ModuleKind::Collider3D, "shape", sphere), "shape = Sphere");
        p.add(ball);
        run(w, 0.3f);
        float y1 = w.transform(ball)->position.y;
        CHECK(y1 < 3.0f && y1 > 2.0f, "it falls (y %.3f after 0.3 s, free fall ~2.56)", y1);
        const RigidBody3D* rb = (const RigidBody3D*)w.get(ball, ModuleKind::RigidBody3D);
        CHECK(rb->velocity.y < -2.0f, "its velocity is live (%.2f)", rb->velocity.y);
        run(w, 2.5f);
        float y = w.transform(ball)->position.y;
        CHECK(y > 0.45f && y < 0.56f, "it rests on the ground (y %.3f, radius 0.5)", y);
        CHECK(ev.collisions == 1, "one first contact reported (%d)", ev.collisions);
        CHECK(p.stats().bodies == 1 && p.stats().colliders == 2, "stats (%d bodies, %d colliders)", p.stats().bodies, p.stats().colliders);
        // The ground never moves.
        CHECK(w.transform(w.lookup("Ground"))->position.y == -0.1f, "a collider without a body stays put");
    }

    // ---- boxes stack; gravity off floats; static and kinematic bodies
    {
        World w;
        w.init();
        PhysicsWorld p;
        p.init(&w, PhysicsEvents());
        ground(w, p);
        Entity a = make(w, "A", Vec3(0, 0.6f, 0));
        Entity b = make(w, "B", Vec3(0.1f, 1.8f, 0));
        Entity f = make(w, "Floater", Vec3(5, 2, 0));
        Entity s = make(w, "Static", Vec3(-5, 2, 0));
        Entity k = make(w, "Kinematic", Vec3(-8, 2, 0));
        Entity all5[] = { a, b, f, s, k };
        for (Entity e : all5) { w.add(e, ModuleKind::RigidBody3D); w.add(e, ModuleKind::Collider3D); p.add(e); }
        w.set_field(f, ModuleKind::RigidBody3D, "use_gravity", FieldValue::from_bool(false));
        w.set_field(s, ModuleKind::RigidBody3D, "type", FieldValue::from_string("Static"));
        w.set_field(k, ModuleKind::RigidBody3D, "type", FieldValue::from_string("Kinematic"));
        w.set_field(k, ModuleKind::RigidBody3D, "velocity", FieldValue::from_vec3(Vec3(1, 0, 0)));
        run(w, 3.0f);
        float ya = w.transform(a)->position.y, yb = w.transform(b)->position.y;
        CHECK(ya > 0.45f && ya < 0.56f, "the bottom box rests on the ground (%.3f)", ya);
        CHECK(yb > 1.4f && yb < 1.56f, "the top box rests on it (%.3f)", yb);
        CHECK(w.transform(f)->position.y == 2.0f, "no gravity: it floats (%.3f)", w.transform(f)->position.y);
        CHECK(w.transform(s)->position.y == 2.0f, "a static body does not fall");
        float kx = w.transform(k)->position.x;
        CHECK(kx > -5.2f && kx < -4.8f && w.transform(k)->position.y == 2.0f, "a kinematic body moves by its velocity only (x %.2f)", kx);
    }

    // ---- bounciness and friction
    {
        World w;
        w.init();
        PhysicsWorld p;
        p.init(&w, PhysicsEvents());
        Entity g = ground(w, p);
        w.set_field(g, ModuleKind::Collider3D, "bounciness", FieldValue::from_float(0.8f));
        Entity ball = make(w, "Bouncy", Vec3(0, 4, 0));
        w.add(ball, ModuleKind::RigidBody3D);
        w.add(ball, ModuleKind::Collider3D);
        w.set_field(ball, ModuleKind::Collider3D, "shape", FieldValue::from_string("Sphere"));
        p.add(ball);
        float max_after = 0;
        bool hit = false;
        for (int i = 0; i < 180; i++) {
            w.progress(1.0f / 60.0f);
            float y = w.transform(ball)->position.y;
            if (y < 0.6f) hit = true;
            if (hit && y > max_after) max_after = y;
        }
        CHECK(hit && max_after > 2.0f, "a bouncy ground throws it back up (peak %.2f after the hit)", max_after);
        // A box sliding on a rough floor stops; on ice (friction 0) it keeps going.
        Entity rough = make(w, "Rough", Vec3(3, 0.5f, 0));
        Entity ice = make(w, "Ice", Vec3(-3, 0.5f, 3));
        Entity both[] = { rough, ice };
        for (Entity e : both) {
            w.add(e, ModuleKind::RigidBody3D); w.add(e, ModuleKind::Collider3D);
            w.set_field(e, ModuleKind::RigidBody3D, "linear_damping", FieldValue::from_float(0));
            w.set_field(e, ModuleKind::RigidBody3D, "velocity", FieldValue::from_vec3(Vec3(0, 0, -4)));
            p.add(e);
        }
        w.set_field(g, ModuleKind::Collider3D, "bounciness", FieldValue::from_float(0));
        w.set_field(ice, ModuleKind::Collider3D, "friction", FieldValue::from_float(0));
        run(w, 2.0f);
        const RigidBody3D* rr = (const RigidBody3D*)w.get(rough, ModuleKind::RigidBody3D);
        const RigidBody3D* ri = (const RigidBody3D*)w.get(ice, ModuleKind::RigidBody3D);
        CHECK(rr->velocity.z > -0.2f, "friction stops the rough box (vz %.2f)", rr->velocity.z);
        CHECK(ri->velocity.z < -3.5f, "no friction: the icy box keeps sliding (vz %.2f)", ri->velocity.z);
    }

    // ---- triggers report enter and exit, and do not push
    {
        World w;
        w.init();
        PhysicsWorld p;
        Events ev;
        PhysicsEvents pe;
        pe.trigger_enter = on_enter; pe.trigger_exit = on_exit; pe.collision = on_collision; pe.user = &ev;
        p.init(&w, pe);
        Entity zone = make(w, "Zone", Vec3(0, 2, 0), Vec3(2, 1, 2));
        w.add(zone, ModuleKind::Collider3D);
        w.set_field(zone, ModuleKind::Collider3D, "mode", FieldValue::from_string("Trigger"));
        p.add(zone);
        Entity ball = make(w, "Falling", Vec3(0, 4, 0));
        w.add(ball, ModuleKind::RigidBody3D);
        w.add(ball, ModuleKind::Collider3D);
        w.set_field(ball, ModuleKind::Collider3D, "shape", FieldValue::from_string("Sphere"));
        w.set_field(ball, ModuleKind::Collider3D, "radius", FieldValue::from_float(0.2f));
        p.add(ball);
        run(w, 0.6f);
        CHECK(ev.enters == 1 && ev.last_trigger == zone && ev.last_other == ball, "entering the trigger is reported once (%d)", ev.enters);
        run(w, 1.0f);
        CHECK(ev.exits == 1, "leaving it is reported (%d)", ev.exits);
        CHECK(w.transform(ball)->position.y < 0.0f, "a trigger does not stop the ball (y %.2f)", w.transform(ball)->position.y);
        CHECK(ev.collisions == 0, "a trigger is not a collision");
        // Layers: a collider on another layer is ignored.
        Entity g = make(w, "Ground2", Vec3(0, -20, 0), Vec3(20, 0.2f, 20));
        w.add(g, ModuleKind::Collider3D);
        w.set_field(g, ModuleKind::Collider3D, "layer", FieldValue::from_bits(2));
        p.add(g);
        run(w, 3.0f);
        CHECK(w.transform(ball)->position.y < -21.0f, "different layers don't collide (y %.2f)", w.transform(ball)->position.y);
        // Removing the body: it stops being simulated.
        p.remove(ball);
        float y = w.transform(ball)->position.y;
        run(w, 0.5f);
        CHECK(w.transform(ball)->position.y == y, "removed bodies don't move");
        w.destroy(zone);
        run(w, 0.1f);       // a dead entity drops out without trouble
        CHECK(true, "dead entities are skipped");
    }

    // ---- a rotated box: a sphere slides down a 30 degree ramp
    {
        World w;
        w.init();
        PhysicsWorld p;
        p.init(&w, PhysicsEvents());
        Entity ramp = make(w, "Ramp", Vec3(0, 0, 0), Vec3(10, 0.2f, 4));
        w.transform(ramp)->rotation = Quat::euler_deg(Vec3(0, 0, 30));
        w.add(ramp, ModuleKind::Collider3D);
        p.add(ramp);
        Entity box = make(w, "Slider", Vec3(1.5f, 1.6f, 0), Vec3(0.5f, 0.5f, 0.5f));
        w.transform(box)->rotation = Quat::euler_deg(Vec3(0, 0, 30));
        w.add(box, ModuleKind::RigidBody3D);
        w.add(box, ModuleKind::Collider3D);
        w.set_field(box, ModuleKind::Collider3D, "friction", FieldValue::from_float(0.0f));
        p.add(box);
        run(w, 1.0f);
        Vec3 pos = w.transform(box)->position;
        CHECK(pos.x < 0.5f, "it slides down the slope towards -x (x %.2f, y %.2f)", pos.x, pos.y);
    }

    // ---- a sphere on a 30 degree ramp rolls (friction) or slides (none)
    {
        for (int frictionless = 0; frictionless < 2; frictionless++) {
            World w;
            w.init();
            PhysicsWorld p;
            p.init(&w, PhysicsEvents());
            Entity ramp = make(w, "Ramp", Vec3(0, 0, 0), Vec3(10, 0.2f, 4));
            w.transform(ramp)->rotation = Quat::euler_deg(Vec3(0, 0, 30));
            w.add(ramp, ModuleKind::Collider3D);
            p.add(ramp);
            float c30 = fcos(radians(30)), s30 = fsin(radians(30));
            Vec3 n(-s30, c30, 0);
            Vec3 pos = Vec3(c30 * 1.5f, s30 * 1.5f, 0) + n * 0.6f;
            Entity ball = make(w, "Ball", pos);
            w.add(ball, ModuleKind::RigidBody3D);
            w.add(ball, ModuleKind::Collider3D);
            Collider3D* col = (Collider3D*)w.get_mut(ball, ModuleKind::Collider3D);
            col->shape = ColliderShape::Sphere;
            if (frictionless) col->friction = 0;
            RigidBody3D* rb = (RigidBody3D*)w.get_mut(ball, ModuleKind::RigidBody3D);
            rb->linear_damping = 0;
            rb->angular_damping = 0;
            p.add(ball);
            run(w, 1.0f);
            float speed = length(rb->velocity);
            float acc = speed / 1.0f;
            Vec3 om = rb->angular_velocity;
            if (!frictionless) {
                float theory = 5.0f / 7.0f * 9.81f * s30;
                printf("physics: rolling sphere acceleration %.3f m/s^2, theory 5/7 g sin = %.3f, omega %.2f rad/s\n", acc, theory, length(om));
                CHECK(acc > theory * 0.8f && acc < theory * 1.2f, "rolling acceleration %.3f (theory %.3f)", acc, theory);
                CHECK(om.z > 0.5f && absf_(om.x) < 0.3f && absf_(om.y) < 0.3f, "rolls around +z (%.2f %.2f %.2f)", om.x, om.y, om.z);
                CHECK(absf_(length(om) * 0.5f - speed) < 0.15f * speed, "no slip: v %.2f, omega*r %.2f", speed, length(om) * 0.5f);
                CHECK(rb->velocity.x < 0 && rb->velocity.y < 0, "it goes down the slope");
            } else {
                float theory = 9.81f * s30;
                CHECK(acc > theory * 0.85f && acc < theory * 1.15f, "sliding acceleration %.3f (theory %.3f)", acc, theory);
                CHECK(length(om) < 0.05f, "a frictionless sphere does not spin (%.3f)", length(om));
            }
        }
    }

    // ---- a box balanced on a tilted edge tips over and lands flat
    {
        World w;
        w.init();
        PhysicsWorld p;
        p.init(&w, PhysicsEvents());
        ground(w, p);
        Entity b = make(w, "Tipper", Vec3(0, 0.67f, 0));
        w.transform(b)->rotation = Quat::euler_deg(Vec3(0, 0, 25));
        w.add(b, ModuleKind::RigidBody3D);
        w.add(b, ModuleKind::Collider3D);
        p.add(b);
        run(w, 5.0f);
        const Transform* t = w.transform(b);
        const RigidBody3D* rb = (const RigidBody3D*)w.get(b, ModuleKind::RigidBody3D);
        Vec3 up = rotate(t->rotation, Vec3(0, 1, 0));
        float al = maxf_(absf_(up.x), maxf_(absf_(up.y), absf_(up.z)));
        CHECK(al > fcos(radians(4)), "it lies flat (up axis %.3f %.3f %.3f)", up.x, up.y, up.z);
        CHECK(t->position.y > 0.47f && t->position.y < 0.53f, "at rest on a face (y %.3f)", t->position.y);
        CHECK(length(rb->angular_velocity) < 0.1f && length(rb->velocity) < 0.1f, "and still");
    }

    // ---- a stack of 5 boxes stands for 5 s; its energy never grows
    {
        World w;
        w.init();
        PhysicsWorld p;
        p.init(&w, PhysicsEvents());
        ground(w, p);
        Entity bx[5];
        for (int i = 0; i < 5; i++) {
            bx[i] = make(w, "S", Vec3(0, 0.5f + i * 1.0f + 0.02f * (i + 1), 0));
            w.add(bx[i], ModuleKind::RigidBody3D);
            w.add(bx[i], ModuleKind::Collider3D);
            p.add(bx[i]);
            char nm[16];
            snprintf(nm, sizeof nm, "S%d", i);
            w.set_name(bx[i], nm);
        }
        auto energy = [&]() {
            float e = 0;
            for (int i = 0; i < 5; i++) {
                const RigidBody3D* rb = (const RigidBody3D*)w.get(bx[i], ModuleKind::RigidBody3D);
                e += 9.81f * w.transform(bx[i])->position.y + 0.5f * dot(rb->velocity, rb->velocity);
            }
            return e;
        };
        float e0 = energy(), emax = e0;
        for (int i = 0; i < 300; i++) {
            w.progress(1.0f / 60.0f);
            float e = energy();
            if (e > emax) emax = e;
        }
        Vec3 top = w.transform(bx[4])->position;
        CHECK(absf_(top.y - 4.5f) < 0.05f && absf_(top.x) < 0.05f && absf_(top.z) < 0.05f, "the top box stays (%.3f %.3f %.3f)", top.x, top.y, top.z);
        CHECK(emax <= e0 + 0.05f, "no energy gain (start %.3f, max %.3f)", e0, emax);
        float spin = 0;
        for (int i = 0; i < 5; i++) spin += length(((const RigidBody3D*)w.get(bx[i], ModuleKind::RigidBody3D))->angular_velocity);
        CHECK(spin < 0.2f, "nothing spins (%.3f)", spin);
    }

    // ---- capsules and cylinders: lying they roll, upright they stand (cylinder)
    {
        World w;
        w.init();
        PhysicsWorld p;
        p.init(&w, PhysicsEvents());
        ground(w, p);
        struct Spec { const char* name; ColliderShape shape; Vec3 pos; bool lying; float h, r; };
        Spec specs[4] = {
            { "CylLie", ColliderShape::Cylinder, Vec3(-4, 0.6f, 0), true, 1.0f, 0.5f },
            { "CapLie", ColliderShape::Capsule, Vec3(-2, 0.4f, 0), true, 1.6f, 0.3f },
            { "CylStand", ColliderShape::Cylinder, Vec3(2, 0.6f, 0), false, 1.0f, 0.5f },
            { "CapStand", ColliderShape::Capsule, Vec3(4, 1.0f, 0), false, 1.6f, 0.3f },
        };
        Entity es[4];
        for (int i = 0; i < 4; i++) {
            es[i] = make(w, specs[i].name, specs[i].pos);
            if (specs[i].lying) w.transform(es[i])->rotation = Quat::euler_deg(Vec3(0, 0, 90));
            w.add(es[i], ModuleKind::RigidBody3D);
            w.add(es[i], ModuleKind::Collider3D);
            Collider3D* c = (Collider3D*)w.get_mut(es[i], ModuleKind::Collider3D);
            c->shape = specs[i].shape;
            c->height = specs[i].h;
            c->radius = specs[i].r;
            p.add(es[i]);
        }
        run(w, 1.5f);
        float y0 = w.transform(es[0])->position.y, y1 = w.transform(es[1])->position.y;
        CHECK(absf_(y0 - 0.5f) < 0.02f, "a lying cylinder rests at its radius (%.3f)", y0);
        CHECK(absf_(y1 - 0.3f) < 0.02f, "a lying capsule rests at its radius (%.3f)", y1);
        for (int i = 0; i < 2; i++) {
            RigidBody3D* rb = (RigidBody3D*)w.get_mut(es[i], ModuleKind::RigidBody3D);
            rb->velocity = Vec3(0, 0, 2);
        }
        run(w, 0.5f);
        for (int i = 0; i < 2; i++) {
            const RigidBody3D* rb = (const RigidBody3D*)w.get(es[i], ModuleKind::RigidBody3D);
            float r = specs[i].r;
            float want = rb->velocity.z / r;
            CHECK(rb->angular_velocity.x > 0.6f * want && rb->angular_velocity.x < 1.4f * want && rb->velocity.z > 1.0f,
                  "%s rolls when pushed (omega.x %.2f, v/r %.2f)", specs[i].name, rb->angular_velocity.x, want);
        }
        float y2 = w.transform(es[2])->position.y;
        Vec3 up2 = rotate(w.transform(es[2])->rotation, Vec3(0, 1, 0));
        CHECK(absf_(y2 - 0.5f) < 0.02f && up2.y > 0.998f, "an upright cylinder rests on its flat end at height/2 (%.3f, up.y %.4f)", y2, up2.y);
        run(w, 4.0f);
        y2 = w.transform(es[2])->position.y;
        CHECK(absf_(y2 - 0.5f) < 0.02f, "and keeps standing (%.3f)", y2);
        run(w, 6.0f);                               // a perfectly upright capsule may take a while to topple
        float y3 = w.transform(es[3])->position.y;
        Vec3 up3 = rotate(w.transform(es[3])->rotation, Vec3(0, 1, 0));
        bool stands = absf_(y3 - 0.8f) < 0.03f && up3.y > 0.99f;
        bool fell = absf_(y3 - 0.3f) < 0.03f;
        CHECK(stands || fell, "an upright capsule stands or has fallen over (y %.3f, up.y %.3f)", y3, up3.y);
    }

    // ---- mesh colliders: points given to set_mesh
    {
        World w;
        w.init();
        PhysicsWorld p;
        p.init(&w, PhysicsEvents());
        ground(w, p);
        Vec3 cube[8];
        for (int i = 0; i < 8; i++) cube[i] = Vec3((i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f);
        Vec3 octa[6] = { Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(0, -1, 0), Vec3(0, 0, 1), Vec3(0, 0, -1) };
        Entity mc = make(w, "MeshCube", Vec3(-3, 0.8f, 0));
        Entity mo = make(w, "MeshOcta", Vec3(0, 2.0f, 0));
        w.transform(mo)->rotation = Quat::euler_deg(Vec3(10, 0, 15));
        Entity ms = make(w, "MeshStack", Vec3(-3, 2.0f, 0));
        Entity nopts = make(w, "NoPoints", Vec3(3, 0.8f, 0));
        Entity all[4] = { mc, mo, ms, nopts };
        for (Entity e : all) {
            w.add(e, ModuleKind::RigidBody3D);
            w.add(e, ModuleKind::Collider3D);
            ((Collider3D*)w.get_mut(e, ModuleKind::Collider3D))->shape = ColliderShape::Mesh;
        }
        p.add(mc);                                  // set_mesh after add ...
        p.set_mesh(mc, cube, 8);
        p.set_mesh(mo, octa, 6);                    // ... and before it
        p.add(mo);
        p.set_mesh(ms, cube, 8);
        p.add(ms);
        p.add(nopts);                               // no points: its size box
        ((RigidBody3D*)w.get_mut(mo, ModuleKind::RigidBody3D))->angular_damping = 1.0f;
        run(w, 6.0f);
        float ycube = w.transform(mc)->position.y, ystack = w.transform(ms)->position.y;
        CHECK(absf_(ycube - 0.5f) < 0.02f, "a mesh cube rests like a box (%.3f)", ycube);
        CHECK(absf_(ystack - 1.5f) < 0.04f, "mesh cubes stack (%.3f)", ystack);
        Vec3 up = rotate(w.transform(mc)->rotation, Vec3(0, 1, 0));
        CHECK(up.y > 0.998f, "and lies flat (up.y %.4f)", up.y);
        float yo = w.transform(mo)->position.y;
        CHECK(absf_(yo - 1.0f / fsqrt(3.0f)) < 0.03f, "an octahedron rests on a face (y %.3f, want 0.577)", yo);
        CHECK(absf_(w.transform(nopts)->position.y - 0.5f) < 0.02f, "a mesh collider without points is its size box (%.3f)", w.transform(nopts)->position.y);
        p.set_mesh(mc, nullptr, 0);                 // removing points does not crash
        run(w, 0.1f);
    }

    // ---- angular velocity, freeze_rotation, angular damping
    {
        World w;
        w.init();
        PhysicsWorld p;
        p.init(&w, PhysicsEvents());
        Entity spin = make(w, "Spin", Vec3(0, 5, 0));
        Entity frozen = make(w, "Frozen", Vec3(3, 5, 0));
        Entity damped = make(w, "Damped", Vec3(6, 5, 0));
        Entity kin = make(w, "Kin", Vec3(9, 5, 0));
        Entity all[4] = { spin, frozen, damped, kin };
        for (Entity e : all) {
            w.add(e, ModuleKind::RigidBody3D);
            w.add(e, ModuleKind::Collider3D);
            RigidBody3D* rb = (RigidBody3D*)w.get_mut(e, ModuleKind::RigidBody3D);
            rb->use_gravity = false;
            rb->angular_damping = 0;
            rb->angular_velocity = Vec3(0, 3, 0);
            p.add(e);
        }
        ((RigidBody3D*)w.get_mut(frozen, ModuleKind::RigidBody3D))->freeze_rotation = true;
        ((RigidBody3D*)w.get_mut(damped, ModuleKind::RigidBody3D))->angular_damping = 1.0f;
        ((RigidBody3D*)w.get_mut(kin, ModuleKind::RigidBody3D))->type = BodyType::Kinematic;
        run(w, 1.0f);
        Vec3 x = rotate(w.transform(spin)->rotation, Vec3(1, 0, 0));
        CHECK(absf_(x.x - fcos(3.0f)) < 0.03f && absf_(x.z - fsin(3.0f) * -1.0f) < 0.05f, "a spinning body turns (x axis %.3f %.3f %.3f)", x.x, x.y, x.z);
        CHECK(absf_(((const RigidBody3D*)w.get(spin, ModuleKind::RigidBody3D))->angular_velocity.y - 3.0f) < 0.01f, "angular velocity is kept");
        const Quat& q = w.transform(frozen)->rotation;
        CHECK(q.x == 0 && q.y == 0 && q.z == 0 && q.w == 1, "freeze_rotation keeps the rotation");
        float wd = ((const RigidBody3D*)w.get(damped, ModuleKind::RigidBody3D))->angular_velocity.y;
        CHECK(wd > 0.9f && wd < 1.4f, "angular damping slows the spin (%.2f, ~3/e = 1.10)", wd);
        Vec3 xk = rotate(w.transform(kin)->rotation, Vec3(1, 0, 0));
        CHECK(absf_(xk.x - fcos(3.0f)) < 0.03f, "a kinematic body rotates by its angular velocity (%.3f)", xk.x);
    }

    // ---- a frozen body does not tip on contact; a child body rolls in parent space
    {
        World w;
        w.init();
        PhysicsWorld p;
        p.init(&w, PhysicsEvents());
        ground(w, p);
        Entity b = make(w, "Frozen2", Vec3(0, 0.67f, 0));
        w.transform(b)->rotation = Quat::euler_deg(Vec3(0, 0, 25));
        w.add(b, ModuleKind::RigidBody3D);
        w.add(b, ModuleKind::Collider3D);
        ((RigidBody3D*)w.get_mut(b, ModuleKind::RigidBody3D))->freeze_rotation = true;
        p.add(b);
        Quat q0 = w.transform(b)->rotation;
        run(w, 2.0f);
        Quat q1 = w.transform(b)->rotation;
        CHECK(q0.x == q1.x && q0.y == q1.y && q0.z == q1.z && q0.w == q1.w, "freeze_rotation: no tipping");
        // A child of a moved parent: falls and rests at the right world height.
        Entity par = make(w, "Par", Vec3(10, 0, 0));
        Entity ch = w.create("Child", par);
        w.transform(ch)->position = Vec3(0, 2, 0);
        w.add(ch, ModuleKind::RigidBody3D);
        w.add(ch, ModuleKind::Collider3D);
        p.add(ch);
        Entity g2 = make(w, "Ground3", Vec3(10, -0.1f, 0), Vec3(20, 0.2f, 20));
        w.add(g2, ModuleKind::Collider3D);
        p.add(g2);
        run(w, 3.0f);
        const WorldTransform* wt = w.world_transform(ch);
        CHECK(wt && absf_(wt->matrix.m[13] - 0.5f) < 0.03f, "a child body rests at the right height (%.3f)", wt ? wt->matrix.m[13] : -1.0f);
    }

    // ---- a ball with bounciness 0 stops
    {
        World w;
        w.init();
        PhysicsWorld p;
        p.init(&w, PhysicsEvents());
        ground(w, p);
        Entity ball = make(w, "Dead", Vec3(0, 3, 0));
        w.add(ball, ModuleKind::RigidBody3D);
        w.add(ball, ModuleKind::Collider3D);
        ((Collider3D*)w.get_mut(ball, ModuleKind::Collider3D))->shape = ColliderShape::Sphere;
        p.add(ball);
        run(w, 3.0f);
        const RigidBody3D* rb = (const RigidBody3D*)w.get(ball, ModuleKind::RigidBody3D);
        CHECK(length(rb->velocity) < 0.05f && absf_(w.transform(ball)->position.y - 0.5f) < 0.02f, "a ball with no bounce stops (v %.3f, y %.3f)", length(rb->velocity), w.transform(ball)->position.y);
    }

    // ---- performance: 200 boxes fall into a pile
    {
        World w;
        w.init();
        PhysicsWorld p;
        p.init(&w, PhysicsEvents());
        ground(w, p);
        // A tray: four walls around the pile.
        for (int k = 0; k < 4; k++) {
            Entity wall = make(w, k == 0 ? "WallA" : k == 1 ? "WallB" : k == 2 ? "WallC" : "WallD",
                               Vec3(k < 2 ? (k ? 4.0f : -4.0f) : 0.0f, 5.0f, k < 2 ? 0.0f : (k == 2 ? 4.0f : -4.0f)),
                               k < 2 ? Vec3(0.2f, 10, 8) : Vec3(8, 10, 0.2f));
            w.add(wall, ModuleKind::Collider3D);
            p.add(wall);
        }
        unsigned seed = 12345;
        for (int i = 0; i < 200; i++) {
            char name[32];
            snprintf(name, sizeof(name), "P%d", i);
            seed = seed * 1664525u + 1013904223u;
            float jx = ((seed >> 8) & 255) / 255.0f - 0.5f;
            seed = seed * 1664525u + 1013904223u;
            float jz = ((seed >> 8) & 255) / 255.0f - 0.5f;
            seed = seed * 1664525u + 1013904223u;
            float ja = ((seed >> 8) & 255) / 255.0f * 90.0f;
            Entity e = make(w, name, Vec3((float)(i % 5) * 1.3f - 2.6f + jx * 0.5f, 1.0f + (i / 25) * 1.2f + (i % 3) * 0.1f, (float)((i / 5) % 5) * 1.3f - 2.6f + jz * 0.5f), Vec3(0.8f, 0.8f, 0.8f));
            w.transform(e)->rotation = Quat::euler_deg(Vec3(ja * 0.3f, ja, ja * 0.2f));
            w.add(e, ModuleKind::RigidBody3D);
            w.add(e, ModuleKind::Collider3D);
            p.add(e);
        }
        clock_t c0 = clock();
        int steps = 360;
        float worst = 0;
        for (int i = 0; i < steps; i++) {
            clock_t s0 = clock();
            w.progress(1.0f / 60.0f);
            float ms = (float)(clock() - s0) * 1000.0f / CLOCKS_PER_SEC;
            if (ms > worst) worst = ms;
        }
        float avg = (float)(clock() - c0) * 1000.0f / CLOCKS_PER_SEC / steps;
        printf("physics: 200 boxes in a pile: %.2f ms per step (worst %.2f), %d contacts\n", avg, worst, p.stats().contacts);
        CHECK(avg < 8.0f, "200 boxes: %.2f ms per step", avg);
        float maxy = 0;
        bool sane = true;
        for (int i = 0; i < 200; i++) {
            char name[32];
            snprintf(name, sizeof(name), "P%d", i);
            Entity e = w.lookup(name);
            Vec3 pos = w.transform(e)->position;
            if (pos.y > maxy) maxy = pos.y;
            if (!(pos.y > -0.5f) || pos.y > 12.0f || absf_(pos.x) > 4.5f || absf_(pos.z) > 4.5f) { sane = false; printf("  out: %s %.2f %.2f %.2f\n", name, pos.x, pos.y, pos.z); }
        }
        CHECK(sane, "the pile stays in the tray (highest box %.2f)", maxy);
    }

    // ---- performance sanity: 300 bodies
    {
        World w;
        w.init();
        PhysicsWorld p;
        p.init(&w, PhysicsEvents());
        ground(w, p);
        for (int i = 0; i < 300; i++) {
            char name[32];
            snprintf(name, sizeof(name), "B%d", i);
            Entity e = make(w, name, Vec3((float)(i % 20) - 10, 0.5f + (i / 20) * 1.1f, 0), Vec3(0.9f, 0.9f, 0.9f));
            w.add(e, ModuleKind::RigidBody3D);
            w.add(e, ModuleKind::Collider3D);
            p.add(e);
        }
        run(w, 2.0f);
        CHECK(p.stats().steps >= 110, "fixed steps ran (%d)", p.stats().steps);
        printf("physics: 300 bodies, %d contacts in the last step\n", p.stats().contacts);
    }

    printf("physics_tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
