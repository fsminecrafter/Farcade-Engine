// The editor's scene: objects with a name, a transform, a mesh and a look.
//
// Kept behind this small interface (ids, get, create, remove, selection) so
// that the engine's ECS (flecs, plan C3) can replace the storage later
// without touching the panels, picking or gizmos.
#pragma once

#include "core/math.h"
#include "script/props.h"
#include "scene/modules.h"
#include <stdint.h>

namespace fe {

typedef uint32_t ObjectId;      // 0 = none
constexpr ObjectId kNoObject = 0;

enum class MeshKind : uint8_t { Cube, Plane, Sphere, Cylinder, Model };

struct MeshData {
    // Triangle list, object space.
    Vec3* positions = nullptr;
    Vec3* normals = nullptr;
    uint32_t vertex_count = 0;
    Vec3 bounds_min, bounds_max;
};

struct Object {
    ObjectId id = kNoObject;
    char name[64] = {};
    ObjectKind kind = ObjectKind::Mesh;  // Mesh draws `mesh`; Camera, Sky ... carry their settings in `modules`
    ObjectId parent = kNoObject;    // kNoObject = at the root; the transform is relative to the parent
    bool expanded = true;           // children shown in the hierarchy
    Vec3 position;
    Quat rotation;
    Vec3 scale = Vec3(1, 1, 1);
    MeshKind mesh = MeshKind::Cube;
    char model_path[256] = {};      // MeshKind::Model: the .mof file
    char texture_path[256] = {};    // a texture dropped on the object
    char shader_path[256] = {};     // custom .fshader material
    // Modules (Rigidbody 3D, Light, ...) and, for special objects, their
    // settings (Camera, Sky, ...): see engine/scene/modules.h.
    ModuleSet modules;
    ScriptSlot scripts[kMaxScripts];    // the scripts on the object, in the order they run
    int script_count = 0;
    Vec3 color = Vec3(0.8f, 0.8f, 0.8f);
    // Material. Roughness, metal and shadows show with the GPU tier (PBR);
    // unlit draws the plain color everywhere.
    float roughness = 0.6f;
    float metallic = 0.0f;
    bool unlit = false;
    bool cast_shadows = true;
    bool visible = true;

    Mat4 matrix() const { return Mat4::trs(position, rotation, scale); }   // local (relative to the parent)
    void set_matrix(const Mat4& m) { decompose(m, &position, &rotation, &scale); }
};

class Scene {
public:
    Scene();
    ~Scene();
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // Makes this scene an exact copy of `other`: objects (with their ids),
    // the next id, the selection and the active object. Play mode uses it to
    // put the scene back when play stops.
    void copy_from(const Scene& other);
    // The id the next created object gets; ids are never handed out twice
    // (Stop keeps the higher of the two, see Editor::stop).
    ObjectId next_id() const { return next_id_; }
    void keep_ids_above(ObjectId next) { if (next > next_id_) next_id_ = next; }

    ObjectId create(const char* name, MeshKind mesh);
    // An empty object with a given id (loading a scene file); null when the
    // id is 0 or taken. Later ids are handed out above it.
    Object* create_with_id(ObjectId id);
    void remove(ObjectId id);           // with all its children
    void clear();

    Object* get(ObjectId id);
    int count() const { return count_; }
    // Objects in creation order (the hierarchy panel's order).
    Object& at(int index) { return *objects_[index]; }

    // ---- selection: a set plus the active object (the last one picked,
    // where the gizmo sits)
    bool is_selected(ObjectId id) const;
    void select_only(ObjectId id);      // kNoObject clears
    void toggle(ObjectId id);           // shift-click
    void clear_selection();
    int selected_count() const { return sel_count_; }
    ObjectId selected(int i) const { return selection_[i]; }
    ObjectId active() const { return active_; }

    // ---- the tree: an object's position, rotation and scale are relative
    // to its parent.
    Mat4 world_matrix(const Object& o);
    Mat4 parent_world_matrix(const Object& o);      // identity at the root
    bool visible_in_tree(const Object& o);          // it and all its parents are visible
    bool is_ancestor(ObjectId ancestor, ObjectId id);   // ancestor is above id (not id itself)
    bool has_children(ObjectId id) const;
    int depth(const Object& o);
    // Moves id under parent (kNoObject: to the root), keeping where it is in
    // the world. Fails when parent is id itself or one of its descendants.
    bool set_parent(ObjectId id, ObjectId parent);
    // True when one of the object's parents is selected (it moves with it).
    bool has_selected_ancestor(ObjectId id);

    // ---- modules and special objects
    // Adds a module (with its defaults) to an object; false when it has one
    // of that kind already or `kind` is a special object's settings.
    bool add_module(ObjectId id, ModuleKind kind);
    void remove_module(ObjectId id, ModuleKind kind);
    // Turns an object into a special object (Camera, Sky, ...) with default
    // settings; it becomes the current one when its kind has no current yet.
    void set_kind(ObjectId id, ObjectKind kind);
    // The "current" flag: only one object per kind has it (the camera the
    // game renders with, the active sky, the listener). Setting one clears
    // the others. Kinds without the flag are ignored.
    void set_current(ObjectId id, ModuleKind kind);
    void clear_current(ObjectId id, ModuleKind kind);
    bool is_current(ObjectId id, ModuleKind kind);
    ObjectId current(ModuleKind kind);              // kNoObject when none
    // Makes sure at most one object per kind is current (the one with the
    // lowest position in the list wins); used after loading or pasting.
    void fix_current();
    // Settings of the current object of a kind, or nullptr.
    void* current_data(ModuleKind kind);

    // ---- scripts. An object holds up to kMaxScripts; the same file may sit
    // on it more than once.
    // Adds a script (no inputs yet: Editor::reload_script reads them).
    // Returns its index, or -1 when the object is full.
    int add_script(ObjectId id, const char* path);
    void remove_script(ObjectId id, int index);
    void move_script(ObjectId id, int from, int to);
    // The first script on `id` called `name` ("door" or "door.lua"), or null:
    // what a script's target:script("door") finds at run time.
    ScriptSlot* find_script(ObjectId id, const char* name);
    // An input of a script on another object, or null.
    ScriptProp* script_prop(ObjectId id, const char* script, const char* prop);
    // How many script inputs (on any object) link to `id`.
    int links_to(ObjectId id);

    // The built-in mesh of an object (primitives are generated once).
    static const MeshData& mesh_for(const Object& o);
    // The loaded .mof of a Model object (model_cache.h: parts, skeleton), or null.
    static const struct Model* model_for(const Object& o);

    // World-space ray pick: the nearest visible object hit, or kNoObject.
    ObjectId pick(const Ray& ray, float* distance = nullptr);

private:
    void remove_one(ObjectId id);
    Object** objects_ = nullptr;
    int count_ = 0, cap_ = 0;
    ObjectId next_id_ = 1;
    ObjectId* selection_ = nullptr;
    int sel_count_ = 0, sel_cap_ = 0;
    ObjectId active_ = kNoObject;
};

}  // namespace fe
