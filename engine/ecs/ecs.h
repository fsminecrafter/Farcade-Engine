// The entity system: a thin C++ layer over flecs (v4). This header does not
// include flecs.h; game and editor code use only this, flecs stays inside
// engine/ecs/*.cpp. See docs/ecs.md.
//
// Entities carry Transform + WorldTransform (always), optionally a
// Renderable, and any of the ModuleKinds of scene/modules.h as components
// (generic access by ModuleKind, by-name field access for scripts). Entities
// form a tree (flecs ChildOf); World::progress runs the pipeline, whose
// transform system computes WorldTransform parents-first.
#pragma once

#include "core/math.h"
#include "ecs/components.h"
#include "scene/modules.h"
#include <stdint.h>

struct ecs_world_t;     // flecs; only for World::raw()

namespace fe {

typedef uint64_t Entity;    // a flecs id; 0 = none / the root

// A value read from or written to a module field by name.
// Float, Int, Bool, Enum (i = index; on read s = the constant's name), Color
// and Vec3 (v), Path (s), Bits (i = the bits).
struct FieldValue {
    FieldType type = FieldType::Float;
    float f = 0;
    int32_t i = 0;
    bool b = false;
    Vec3 v;
    const char* s = nullptr;    // on read: points into the component, valid until it changes

    static FieldValue from_float(float x) { FieldValue r; r.type = FieldType::Float; r.f = x; return r; }
    static FieldValue from_int(int32_t x) { FieldValue r; r.type = FieldType::Int; r.i = x; return r; }
    static FieldValue from_bool(bool x) { FieldValue r; r.type = FieldType::Bool; r.b = x; return r; }
    static FieldValue from_enum(int32_t x) { FieldValue r; r.type = FieldType::Enum; r.i = x; return r; }
    static FieldValue from_bits(uint32_t x) { FieldValue r; r.type = FieldType::Bits; r.i = (int32_t)x; return r; }
    static FieldValue from_vec3(const Vec3& x) { FieldValue r; r.type = FieldType::Vec3; r.v = x; return r; }
    static FieldValue from_color(const Vec3& x) { FieldValue r; r.type = FieldType::Color; r.v = x; return r; }
    static FieldValue from_string(const char* x) { FieldValue r; r.type = FieldType::Path; r.s = x; return r; }
};

// Where a callback added with World::add_system runs each frame.
// Order within a frame: PreUpdate, Update, PostUpdate (the transform system
// is here), Store.
enum class Phase : uint8_t {
    PreUpdate,   // input, script on_start / pre-frame work
    Update,      // scripts, physics: write Transform
    PostUpdate,  // after WorldTransform is computed (it runs first in this phase)
    Store,       // audio sync, render extraction: read WorldTransform
};

class World;
struct WorldImpl;        // engine/ecs/world.cpp
typedef void (*FrameFn)(World& world, float dt, void* user);

class World {
public:
    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // Creates the flecs world and registers everything. false on failure.
    bool init();
    void shutdown();
    bool valid() const { return impl_ != nullptr; }

    // ---- entities
    // A new entity with Transform and WorldTransform. `name` must be unique
    // among its siblings (here: the roots), else 0 is returned. Names are
    // plain text ('.' is fine). The root names "fe" and "flecs" are taken
    // by the engine (components live under fe.*).
    Entity create(const char* name = nullptr, Entity parent = 0);
    // Destroys e and all its descendants.
    void destroy(Entity e);
    bool alive(Entity e) const;
    // The root entity called `name` (0 if none), or the child of `parent`.
    Entity lookup(const char* name) const;
    Entity lookup_child(Entity parent, const char* name) const;
    // nullptr when unnamed.
    const char* name(Entity e) const;
    // nullptr or "" clears the name. false if a sibling has it.
    bool set_name(Entity e, const char* name);

    // ---- hierarchy
    // parent 0 = make e a root. false if it would make a cycle, or a sibling
    // of the new parent has e's name.
    bool set_parent(Entity child, Entity parent);
    Entity parent(Entity e) const;      // 0 for roots
    // Children of `parent` (not of the root: keep your own list of roots).
    // Order is not specified. Returns the count; writes at most `max`.
    int children(Entity parent, Entity* out, int max) const;
    int child_count(Entity parent) const;

    // ---- transform and renderable
    Transform* transform(Entity e);                     // nullptr if e is dead
    const Transform* transform(Entity e) const;
    const WorldTransform* world_transform(Entity e) const;  // as of the last transform update
    // Runs the transform system now (progress() does it every frame).
    void update_transforms();
    Renderable* add_renderable(Entity e);               // existing one kept
    Renderable* renderable(Entity e);                   // nullptr if none
    const Renderable* renderable(Entity e) const;
    void remove_renderable(Entity e);

    // ---- modules (RigidBody3D ... PostProcess), generic access by kind
    bool has(Entity e, ModuleKind k) const;
    // Adds the module with its default values (kept as is if already there).
    bool add(Entity e, ModuleKind k);
    void remove(Entity e, ModuleKind k);
    // nullptr if e does not have it. After changing a `current` flag through
    // get_mut call modified(); everything else works without it.
    void* get_mut(Entity e, ModuleKind k);
    const void* get(Entity e, ModuleKind k) const;
    void modified(Entity e, ModuleKind k);
    // Adds the module if needed and copies `data` (module_info(k).size bytes) in.
    bool set(Entity e, ModuleKind k, const void* data);
    // Adds every module present in `set` with its values.
    void load(Entity e, const ModuleSet& set);
    // Copies the entity's modules into `out` (present = what e has).
    void store(Entity e, ModuleSet* out) const;

    // ---- fields by name ("intensity"), validated against the FieldInfo:
    // numbers are converted and clamped to the range, enums take an index
    // inside the names (or the name as a Path string), strings are cut to
    // kPathMax - 1. Both return false for a missing module, an unknown
    // field, a value of the wrong kind or an out of range enum.
    bool get_field(Entity e, ModuleKind k, const char* field, FieldValue* out) const;
    bool set_field(Entity e, ModuleKind k, const char* field, const FieldValue& v);

    // ---- the "current" rule (Camera, SkyLight, Sky, PostProcess, AudioListener):
    // at most one entity per kind has the flag; the one set last wins, whether
    // it is set here, with set_field, set() or get_mut()+modified().
    // false if k has no current flag or e does not have the module.
    bool set_current(Entity e, ModuleKind k);
    void clear_current(Entity e, ModuleKind k);
    Entity current(ModuleKind k) const;     // 0 if none

    // ---- frame
    // Runs the pipeline: PreUpdate, Update, PostUpdate, Store.
    // Inside a callback, structural changes (create, destroy, add, remove,
    // set_parent) take effect at the end of the phase; reads of existing
    // components and field writes are immediate.
    void progress(float dt);
    // Calls fn every frame in `phase`. Returns an id for remove_system.
    Entity add_system(Phase phase, FrameFn fn, void* user = nullptr);
    void remove_system(Entity system);

    // ---- engine internals (these need flecs.h; most code does not)
    ecs_world_t* raw() const;
    Entity component(ModuleKind k) const;       // the flecs component id of a module
    Entity transform_component() const;
    Entity world_transform_component() const;
    Entity renderable_component() const;

private:
    WorldImpl* impl_ = nullptr;
};

}  // namespace fe
