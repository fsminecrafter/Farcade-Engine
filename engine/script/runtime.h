// The Lua 5.4 script runtime: runs the scripts on objects inside an ECS
// world (engine/ecs), with the API described in docs/scripting.md and
// docs/modules.md.
//
// One instance per script on an entity: its own `self` table holding its
// inputs (the ScriptProp values set in the editor) plus whatever the script
// stores, and `self.object`, a handle to its entity. The script file's chunk
// runs once per instance in its own environment (a sandbox whose globals
// fall back to a shared, safe library table), so functions it defines
// (on_start, on_update, open, ...) belong to that instance.
//
// Callbacks a script may define (all optional, `self` first):
//   on_start(self)                    once, before the first on_update
//   on_update(self, dt)               every frame (ECS phase Update)
//   on_event(self, name, ...)         any event sent to its object
//   on_<name>(self, ...)              an event called <name> (send("used", x) calls on_used)
//   on_trigger_enter(self, other)     another object entered a trigger collider on this object
//   on_trigger_exit(self, other)
//   on_collision(self, other)         this object's body hit another one
//
// Errors (load or run time) go to the host's log with "file:line: message"
// and a traceback; an instance that fails stops running (the others go on).
//
// This header does not include Lua or flecs.
#pragma once

#include "ecs/ecs.h"
#include "script/props.h"
#include <stdint.h>

namespace fe {

enum class ScriptLogLevel : uint8_t { Info, Warning, Error };

struct ScriptHost {
    // Where print() and errors go (the editor's console).
    void (*log)(ScriptLogLevel level, const char* text, void* user) = nullptr;
    void* user = nullptr;
};

struct ScriptStats {
    int instances = 0;
    int failed = 0;                     // instances stopped by an error
    double update_ms = 0;               // the last frame's on_update total
};

class ScriptRuntime {
public:
    ScriptRuntime();
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    // Creates the Lua state and registers an ECS system (Phase::Update) on
    // `world` that calls on_start/on_update. The world must outlive the
    // runtime (or call shutdown() first).
    bool init(World* world, const ScriptHost& host);
    void shutdown();

    // Adds an instance of a script to entity `e`. `source` is the file's text
    // (size bytes), `path` names it in errors and is what obj:script(name)
    // matches ("door" finds ".../door.lua"). `props` are the inputs; for
    // PropType::Object inputs, `object_entities[i]` is the entity props[i]
    // links to (0 = nil), already resolved by the caller. PropType::File inputs
    // reach Lua as project-relative path strings. Returns an
    // instance id (> 0), or 0 when the chunk failed to load or run (logged).
    int add_instance(Entity e, const char* path, const char* source, uint32_t size, const ScriptProp* props, int prop_count,
                     const Entity* object_entities);
    void remove_instances(Entity e);

    // Events from the engine (physics, input, the editor).
    void send(Entity target, const char* event);                  // obj:send(event) without arguments
    void trigger_enter(Entity trigger_owner, Entity other);
    void trigger_exit(Entity trigger_owner, Entity other);
    void collision(Entity a, Entity b);                           // calls on_collision on both

    // Reads an instance's value by name, for tests and the editor (numbers,
    // booleans and strings). False when absent or of another type.
    bool get_number(int instance, const char* field, double* out);
    bool get_bool(int instance, const char* field, bool* out);
    bool get_string(int instance, const char* field, char* out, int cap);

    // A method on a module proxy, provided by an engine system: after
    // add_module_method(ModuleKind::AudioSampler, "play", fn, user), a script's
    // obj.audio_sampler:play() calls fn(entity, user) and gets `fn(...) != 0`
    // back as a boolean. Up to 32 methods; the same name again replaces it.
    typedef int (*ModuleMethod)(Entity e, void* user);
    bool add_module_method(ModuleKind kind, const char* name, ModuleMethod fn, void* user);

    // Runs a line of Lua in the shared environment (a console / REPL; tests).
    bool run(const char* code);

    const ScriptStats& stats() const;
    double time() const;                // seconds since init (fe.time())

    struct Impl;
private:
    Impl* impl_ = nullptr;
};

}  // namespace fe
