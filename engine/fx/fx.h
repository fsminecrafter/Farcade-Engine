// The fx systems of one World, started together: what a host (the editor's
// Play, a game player) creates after the world, the physics and the script
// runtime.
//
//   FxWorld fx;
//   fx.init(&world, host);
//   fx.bind_script(&scripts);      // obj.particles:emit(5), obj.spawner:spawn(), ...
//   ...                            // world.progress(dt) runs them
//   fx.shutdown();
//
// Rendering reads from it: particles.collect(eye, &batches) (gfx/particles.h
// for the renderer), the editor draws previews from the same data.
#pragma once

#include "ecs/ecs.h"
#include "fx/particles.h"
#include "fx/spawner.h"
#include "fx/animator.h"

namespace fe {

class ScriptRuntime;
class PhysicsWorld;

// What the host gives the systems.
struct FxHost {
    void* user = nullptr;
    PhysicsWorld* physics = nullptr;    // for Joint 3D's snap()
    // Makes a copy of `source` (and its children) at the given world matrix and
    // returns the copy's root entity (0 = could not). The Spawner uses it.
    Entity (*spawn)(void* user, Entity source, const Mat4& world) = nullptr;
    // A project file's bytes (malloc'd, the caller frees), or null: animation files.
    void* (*read_file)(const char* path, size_t* size, void* user) = nullptr;
    // Problems worth showing the user (a broken animation file).
    void (*log)(void* user, const char* what, const char* path) = nullptr;
};

class FxWorld {
public:
    FxWorld() {}
    ~FxWorld() { shutdown(); }
    FxWorld(const FxWorld&) = delete;
    FxWorld& operator=(const FxWorld&) = delete;

    bool init(World* world, const FxHost& host);
    void shutdown();
    // Adds the Lua methods of the modules: particles (play, stop, clear, emit),
    // joints (snap), spawners (spawn, clear), animators (play, stop). Call after the script runtime started.
    void bind_script(ScriptRuntime* scripts);

    // Call after World::progress(dt) each frame: the work that makes and
    // destroys entities (spawners).
    void post_frame(float dt) { spawner.update(dt); }

    ParticleSystem particles;
    SpawnerSystem spawner;
    AnimatorSystem animator;

private:
    World* world_ = nullptr;
    FxHost host_;
};

}  // namespace fe
