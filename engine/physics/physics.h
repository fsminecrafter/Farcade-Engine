// A small built-in physics step for Rigidbody 3D, Collider 3D and Character
// Controller 3D: a stand-in until Jolt (plan C6), which needs the C++ standard
// library the engine does not use yet.
//
// What it does, on an ECS world (engine/ecs), at a fixed 60 Hz:
// - character controllers use an upright capsule, gravity and their live
//   velocity; collisions update their grounded state. Scripts move them by
//   setting character_controller3d.velocity (m/s);
// - dynamic bodies fall (gravity * gravity_scale), are damped (linear and
//   angular), and move and turn by their velocity and angular velocity
//   (RigidBody3D::velocity / angular_velocity are live: scripts read and
//   set them). Inertia comes from the collider shape (box, sphere, capsule,
//   cylinder exactly; a mesh as its bounding box). freeze_rotation keeps the
//   rotation fixed; kinematic bodies move and turn by their velocities and
//   nothing pushes them;
// - colliders: box, sphere, capsule, cylinder (along local Y) and mesh
//   (set_mesh, below). Every pair is tested with GJK (distance) and EPA
//   (penetration) on convex shapes; sphere and capsule are a point / segment
//   plus a radius. Contacts are manifolds of up to 4 points built by clipping
//   the supporting faces of the two shapes, so boxes rest flat and stacks
//   stand. Colliders without a rigidbody and static bodies do not move;
// - the solver is sequential impulses with angular terms: Coulomb friction
//   (spheres roll, boxes tip), restitution (bounciness; no bouncing below
//   1 m/s), warm starting from the last step's contacts, speculative
//   contacts (a small margin, so resting contacts do not flicker) and split
//   impulses for penetration (no energy from position correction);
// - trigger colliders report objects entering and leaving them, and first
//   contacts between bodies are reported as collisions (the script runtime
//   turns these into on_trigger_enter/exit and on_collision).
// Mesh colliders: the convex hull of the points given to set_mesh (the
// collider's `center` is not applied to them; scale is). Without points a
// mesh collider is its `size` box. Concave triangle meshes are not done.
// Joints (the Joint 3D module, scene/modules.h): fixed, hinge (limits, motor),
// spring and ball socket between two bodies or a body and the world. They are
// rows of the same solver (a point constraint on the anchors, angular rows,
// accumulated impulses warm started), solved with the contacts; a joint
// breaks above its break force (module field `broken`). The entity added
// with add() must also have a Rigidbody 3D; the connected object is found
// with World::link_entity and must be added too.
// Force Fields (the Force Field module): volumes whose force acts on dynamic
// bodies inside them (add() the field's entity too).
// Not done: continuous collision (very fast small bodies can pass
// through thin colliders), sleeping, concave meshes. Broadphase: sweep and
// prune on x.
#pragma once

#include "ecs/ecs.h"

namespace fe {

struct PhysicsEvents {
    void (*trigger_enter)(Entity trigger_owner, Entity other, void* user) = nullptr;
    void (*trigger_exit)(Entity trigger_owner, Entity other, void* user) = nullptr;
    void (*collision)(Entity a, Entity b, void* user) = nullptr;
    void* user = nullptr;
};

struct PhysicsStats {
    int bodies = 0;                     // with a rigidbody or character controller
    int joints = 0;                     // active joints in the last step
    int colliders = 0;
    int contacts = 0;                   // last step
    int steps = 0;                      // fixed steps so far
};

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    struct Body;                        // internals (physics.cpp)
    struct JointState;
    struct MeshEntry;
    struct Manifold;

    // Registers the physics step on `world` (Phase::Update, after whatever
    // was added before it, e.g. scripts).
    bool init(World* world, const PhysicsEvents& events);
    void shutdown();

    // Entities to simulate: call for every entity that has a Rigidbody 3D
    // and/or a Collider 3D, a Joint 3D or a Force Field (others are
    // ignored). Dead entities drop out.
    void add(Entity e);
    // Breaks the entity's joint now (the Joint 3D `broken` field says so).
    bool break_joint(Entity e);
    void remove(Entity e);

    // Points of the entity's mesh in object space, for a Mesh collider (copied;
    // the physics keeps their convex hull, at most 1024 points are used).
    // Call it before or after add(); calling it again replaces the points,
    // count 0 removes them. The editor calls it for every Mesh collider
    // (the object's mesh vertices) when play starts.
    void set_mesh(Entity e, const Vec3* points, int count);

    Vec3 gravity = Vec3(0, -9.81f, 0);
    float fixed_step = 1.0f / 60.0f;
    int max_steps_per_frame = 4;
    int velocity_iterations = 10;
    int position_iterations = 3;

    // One fixed step now (tests); the registered system calls it as time passes.
    void step(float dt);
    const PhysicsStats& stats() const { return stats_; }

private:
    static void system(World& w, float dt, void* user);
    World* world_ = nullptr;
    Entity system_ = 0;
    PhysicsEvents events_;
    Body* bodies_ = nullptr;
    int count_ = 0, cap_ = 0;
    // Trigger overlaps of the last step (owner, other) and body contacts.
    uint64_t* overlaps_ = nullptr;
    int overlap_count_ = 0, overlap_cap_ = 0;
    // Contacts of the last step, kept for warm starting and first-contact events.
    Manifold* manifolds_ = nullptr;
    int manifold_count_ = 0, manifold_cap_ = 0;
    MeshEntry* meshes_ = nullptr;
    int mesh_count_ = 0, mesh_cap_ = 0;
    JointState* joints_ = nullptr;
    int joint_count_ = 0, joint_cap_ = 0;
    double time_ = 0;                   // simulated seconds (the clock of field turbulence)
    float accumulator_ = 0;
    PhysicsStats stats_;
};

}  // namespace fe
