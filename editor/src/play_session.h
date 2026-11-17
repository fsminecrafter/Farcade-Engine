// A running scene: the object list turned into an ECS world with physics,
// Lua scripts and audio. Editor Play and the standalone player (player/)
// both run a scene through this class, so a game behaves the same in both.
//
// The session does not own the scene. Each step() writes the entities'
// transforms, looks and modules back into it, which is what the viewport,
// the Inspector and the renderer read.
#pragma once

#include "scene.h"
#include <stddef.h>
#include <stdint.h>

namespace fe {

class World;
class PhysicsWorld;
class ScriptRuntime;
class AudioSystem;
class AudioDevice;
namespace platform { struct Event; }
namespace gfx { struct GraphicsSettings; }

enum class PlayLog : uint8_t { Info, Warning, Error };

struct PlayHost {
    void* user = nullptr;
    // Console lines: script print(), script errors, "Audio: ..." problems, start-up problems.
    void (*log)(PlayLog level, const char* text, void* user) = nullptr;
    // Project files (scripts, audio): malloc'd bytes or null. Required.
    void* (*read_file)(const char* path, size_t* size, void* user) = nullptr;
    // Called for physics events after the scripts have been told ("entered",
    // "left", "hit"). May be null.
    void (*physics_event)(const char* what, uint64_t a, uint64_t b, void* user) = nullptr;
    // The host's live graphics settings, for the scripts' fe.graphics API
    // (null: scripts cannot change them). *graphics_changed is set when a
    // script changed them; the host applies them before the next frame.
    gfx::GraphicsSettings* graphics = nullptr;
    bool* graphics_changed = nullptr;
};

class PlaySession {
public:
    ~PlaySession() { stop(); }

    // Builds the world from `scene`. `audio_device` stays owned by the caller
    // (null: no sound). False when the world could not start.
    bool start(Scene& scene, const PlayHost& host, AudioDevice* audio_device);
    void stop();
    bool running() const { return world_ != nullptr; }
    // Advances the game by dt seconds and writes the results back into the scene.
    void step(float dt);

    World* world() { return world_; }
    PhysicsWorld* physics() { return physics_; }
    ScriptRuntime* scripts() { return scripts_; }
    AudioSystem* audio() { return audio_; }
    int entity_count() const { return count_; }
    int script_count() const { return script_count_; }
    uint64_t entity_of(ObjectId id) const;
    ObjectId object_of(uint64_t entity) const;

    // Keyboard and mouse into the scripts' fe.key_down / fe.mouse_x ... API.
    // True when the event was an input event the scripts get.
    bool forward_event(const platform::Event& e);

    // Used by the physics callbacks.
    void physics_event(const char* what, uint64_t a, uint64_t b);

private:
    Scene* scene_ = nullptr;
    PlayHost host_;
    World* world_ = nullptr;
    PhysicsWorld* physics_ = nullptr;
    ScriptRuntime* scripts_ = nullptr;
    AudioSystem* audio_ = nullptr;
    ObjectId* objects_ = nullptr;           // [i] = object of entities_[i]
    uint64_t* entities_ = nullptr;
    int count_ = 0;
    int script_count_ = 0;
    void log(PlayLog level, const char* fmt, ...);
};

}  // namespace fe
