// Tests for the built-in physics step (engine/physics).
#include "physics/physics.h"

#include <stdio.h>
#include <string.h>

using namespace fe;

static int g_checks, g_failed;
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
