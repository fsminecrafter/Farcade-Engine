// The Spawner module (scene/modules.h): makes copies of another object.
//
// It runs after the frame (FxWorld::post_frame), not inside the ECS
// pipeline, because creating and destroying entities there is deferred. The
// host makes the copy (FxHost::spawn: the editor duplicates the template's
// object tree into the play scene, world and physics); the spawner places it,
// gives it velocity, counts it and destroys it at the end of its lifetime or
// when more than max_alive exist (the oldest goes first).
#pragma once

#include "ecs/ecs.h"

namespace fe {

struct FxHost;

class SpawnerSystem {
public:
    ~SpawnerSystem() { shutdown(); }
    bool init(World* world, const FxHost* host);
    void shutdown();
    // Advances the timers and lifetimes (call after World::progress).
    void update(float dt);
    // One copy now (Lua spawn()); false without a template or host.
    bool spawn(Entity spawner);
    // Destroys the spawner's copies.
    bool clear(Entity spawner);

private:
    struct Copy { Entity e; float born; };
    struct State {
        Entity spawner = 0;
        Copy* copies = nullptr;
        int n = 0, cap = 0;
        float timer = 0;
        uint32_t rng = 1;
        bool started = false;
        bool have_last = false;
        Vec3 last_pos, vel;
        bool seen = false;
    };
    State* state(Entity e, bool create);
    bool spawn_one(State& st, Spawner& m);
    float rnd(State& st);
    void drop_copy(State& st, int i);
    World* world_ = nullptr;
    const FxHost* host_ = nullptr;
    State* states_ = nullptr;
    int count_ = 0, cap_ = 0;
    Entity* scan_ = nullptr;
    int scan_cap_ = 0;
    float time_ = 0;
};

}  // namespace fe
