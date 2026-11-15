// A small built-in physics step for Rigidbody 3D and Collider 3D: a
// stand-in until Jolt (plan C6), which needs the C++ standard library the
// engine does not use yet.
//
// What it does, on an ECS world (engine/ecs), at a fixed 60 Hz:
// - dynamic bodies fall (gravity * gravity_scale), are damped, and move by
//   their velocity (RigidBody3D::velocity is the live velocity, so scripts
//   read and set it: obj.rigidbody3d.velocity);
// - colliders (box, sphere; capsule and mesh are treated as boxes around
//   them) push dynamic bodies out, with bounciness and friction; colliders
//   without a rigidbody, static and kinematic bodies do not move;
// - trigger colliders report objects entering and leaving them, and first
//   contacts between bodies are reported as collisions (the script runtime
//   turns these into on_trigger_enter/exit and on_collision).
// Not done: rotation from collisions (bodies slide, they don't roll or tip),
// joints, continuous collision (very fast small bodies can pass through
// thin colliders), anything faster than O(n^2) pair tests.
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
    int bodies = 0;                     // with a rigidbody
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

    // Registers the physics step on `world` (Phase::Update, after whatever
    // was added before it, e.g. scripts).
    bool init(World* world, const PhysicsEvents& events);
    void shutdown();

    // Entities to simulate: call for every entity that has a Rigidbody 3D
    // and/or a Collider 3D (others are ignored). Dead entities drop out.
    void add(Entity e);
    void remove(Entity e);

    Vec3 gravity = Vec3(0, -9.81f, 0);
    float fixed_step = 1.0f / 60.0f;
    int max_steps_per_frame = 4;

    // One fixed step now (tests); the registered system calls it as time passes.
    void step(float dt);
    const PhysicsStats& stats() const { return stats_; }

private:
    struct Body;
    static void system(World& w, float dt, void* user);
    World* world_ = nullptr;
    Entity system_ = 0;
    PhysicsEvents events_;
    Body* bodies_ = nullptr;
    int count_ = 0, cap_ = 0;
    // Trigger overlaps of the last step (owner, other) and body contacts.
    uint64_t* overlaps_ = nullptr;
    int overlap_count_ = 0, overlap_cap_ = 0;
    uint64_t* touching_ = nullptr;
    int touching_count_ = 0, touching_cap_ = 0;
    float accumulator_ = 0;
    PhysicsStats stats_;
};

}  // namespace fe
