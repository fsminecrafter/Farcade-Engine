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
#include "fx/sprites.h"

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
    // HOOK for skinned / model animation (not used by the editor yet): called
    // every frame for each Animator that has a clip, after the transform tracks
    // were applied, with the clip name, its time (seconds) and the crossfade
    // weight of the clip being faded out (0 = none) with its own clip and time.
    // A host that loads a .mof model with a skeleton samples its pose from
    // this and hands the posed vertices to the renderer; the Animator owns the
    // state (clip, time, speed, loop, fade), the host only reads it.
    void (*animator_pose)(void* user, Entity e, const char* clip, float time, const char* from_clip, float from_time, float fade_mix) = nullptr;
    // Problems worth showing the user (a broken animation file).
    void (*log)(void* user, const char* what, const char* path) = nullptr;
};

class FxWorld {
public:
    FxWorld() {}
    ~FxWorld();
    FxWorld(const FxWorld&) = delete;
    FxWorld& operator=(const FxWorld&) = delete;

    bool init(World* world, const FxHost& host);
    void shutdown();
    // Adds the Lua methods of the modules: particles (play, stop, clear, emit),
    // billboards and text (properties only), joints (snap), spawners (spawn, clear), animators (play, stop). Call after the script runtime started.
    void bind_script(ScriptRuntime* scripts);

    // Call after World::progress(dt) each frame: the work that makes and
    // destroys entities (spawners).
    void post_frame(float dt) { particles.bank().quality = quality; spawner.update(dt); }

    // Everything the renderer draws over the image for a camera at `eye`:
    // particles, billboards (as batches) and text labels. The arrays belong to
    // the FxWorld and stay valid until the next collect(). Fill a
    // gfx::RenderScene's particles, particle_batch_count, labels and
    // label_count from it.
    struct Frame {
        const gfx::ParticleBatch* batches = nullptr;
        int batch_count = 0;
        const gfx::TextLabel* labels = nullptr;
        int label_count = 0;
    };
    Frame collect(const Vec3& eye);
    FxQuality quality;                  // applied by collect() and the systems

    ParticleSystem particles;
    SpriteBank sprites;
    SpawnerSystem spawner;
    AnimatorSystem animator;

private:
    World* world_ = nullptr;
    FxHost host_;
    gfx::ParticleBatch* merged_ = nullptr;
    int merged_cap_ = 0;
};

}  // namespace fe
