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

namespace fe::gfx { struct GraphicsSettings; }

namespace fe {

enum class ScriptLogLevel : uint8_t { Info, Warning, Error };

struct ScriptHost {
    // Where print() and errors go (the editor's console).
    void (*log)(ScriptLogLevel level, const char* text, void* user) = nullptr;
    void* user = nullptr;
};

// What a game shows about itself while it loads or works (fe.status): the
// player and the editor's Play mode draw it as a bar with a line of text,
// together with their own "compiling shaders" progress.
struct GameStatus {
    bool shown = false;
    char text[128] = {};
    float progress = -1.0f;             // 0..1, or < 0 for a bar that only moves
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

    // Input from the host, read by scripts as fe.key_down("w"), fe.key_pressed("space"),
    // fe.mouse_x(), fe.mouse_y(), fe.mouse_down(0), fe.mouse_pressed(0), fe.mouse_wheel()
    // (docs/scripting.md). Key names: "a".."z", "0".."9", "space", "enter", "escape",
    // "tab", "backspace", "left", "right", "up", "down", "shift", "ctrl", "alt", "f1".."f12".
    // "pressed" / "released" / the wheel hold for the next update only.
    void key_event(const char* name, bool down);
    void mouse_move(float x, float y);                // pixels of the game's view
    void mouse_button(int button, bool down);         // 0 left, 1 right, 2 middle
    void mouse_wheel(float dy);

    // Reads an instance's value by name, for tests and the editor (numbers,
    // booleans and strings). False when absent or of another type.
    bool get_number(int instance, const char* field, double* out);
    bool get_bool(int instance, const char* field, bool* out);
    bool get_string(int instance, const char* field, char* out, int cap);

    // A method on a module proxy, provided by an engine system: after
    // add_module_method(ModuleKind::AudioSampler, "play", fn, user), a script's
    // obj.audio_sampler:play() calls fn(entity, user) and gets `fn(...) != 0`
    // back as a boolean. Up to 64 methods; the same name again replaces it.
    typedef int (*ModuleMethod)(Entity e, void* user);
    bool add_module_method(ModuleKind kind, const char* name, ModuleMethod fn, void* user);

    // The same with arguments and a result: obj.particles:emit(10),
    // obj.animator:play("walk", 0.3), obj.joint3d:break(). Up to 4 arguments
    // are passed (numbers in num[i], strings in str[i], which is null for
    // others; booleans count as numbers 0/1, nil and tables as 0); they are
    // valid during the call. The result is a boolean unless fn sets another.
    struct MethodCall {
        int argc = 0;
        double num[4] = {};
        const char* str[4] = {};
    };
    struct MethodResult {
        enum Type { Bool, Number, String, Nothing } type = Bool;
        bool b = true;
        double n = 0;
        const char* s = nullptr;        // copied before fn returns to Lua: may point to a local
    };
    typedef void (*ModuleMethodEx)(Entity e, const MethodCall& call, MethodResult* out, void* user);
    bool add_module_method_ex(ModuleKind kind, const char* name, ModuleMethodEx fn, void* user);

    // The graphics settings scripts see as fe.graphics (lua_graphics.h): `settings` is the host's live struct,
    // `changed` (optional) is set to true whenever a script changes it (the host then calls Renderer::set_settings
    // before the next frame). Null settings: fe.graphics raises errors.
    void set_graphics_settings(gfx::GraphicsSettings* settings, bool* changed = nullptr);

    // Runs a line of Lua in the shared environment (a console / REPL; tests).
    bool run(const char* code);

    const ScriptStats& stats() const;
    // fe.status(text, progress): what the game wants shown (shown = false after fe.status()).
    const GameStatus& status() const;
    double time() const;                // seconds since init (fe.time())

    struct Impl;
private:
    Impl* impl_ = nullptr;
};

}  // namespace fe
