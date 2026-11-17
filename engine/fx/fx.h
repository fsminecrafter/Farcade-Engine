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

namespace fe {

class ScriptRuntime;

// What the host gives the systems.
struct FxHost {
    void* user = nullptr;
};

class FxWorld {
public:
    FxWorld() {}
    ~FxWorld() { shutdown(); }
    FxWorld(const FxWorld&) = delete;
    FxWorld& operator=(const FxWorld&) = delete;

    bool init(World* world, const FxHost& host);
    void shutdown();
    // Adds the Lua methods of the modules: particles (emit, play, stop,
    // clear). Call after the script runtime started.
    void bind_script(ScriptRuntime* scripts);

    ParticleSystem particles;

private:
    World* world_ = nullptr;
    FxHost host_;
};

}  // namespace fe
