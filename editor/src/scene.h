// The editor's scene: objects with a name, a transform, a mesh and a look.
//
// Kept behind this small interface (ids, get, create, remove, selection) so
// that the engine's ECS (flecs, plan C3) can replace the storage later
// without touching the panels, picking or gizmos.
#pragma once

#include "core/math.h"
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
    ObjectId parent = kNoObject;    // not used by the tools yet; kept for the hierarchy
    Vec3 position;
    Quat rotation;
    Vec3 scale = Vec3(1, 1, 1);
    MeshKind mesh = MeshKind::Cube;
    char model_path[256] = {};      // MeshKind::Model: the .mof file
    char texture_path[256] = {};    // a texture dropped on the object
    char script_path[256] = {};     // a script dropped on the object
    Vec3 color = Vec3(0.8f, 0.8f, 0.8f);
    bool visible = true;

    Mat4 matrix() const { return Mat4::trs(position, rotation, scale); }
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
    void remove(ObjectId id);
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

    // The built-in mesh of an object (primitives are generated once).
    static const MeshData& mesh_for(const Object& o);

    // World-space ray pick: the nearest visible object hit, or kNoObject.
    ObjectId pick(const Ray& ray, float* distance = nullptr);

private:
    Object** objects_ = nullptr;
    int count_ = 0, cap_ = 0;
    ObjectId next_id_ = 1;
    ObjectId* selection_ = nullptr;
    int sel_count_ = 0, sel_cap_ = 0;
    ObjectId active_ = kNoObject;
};

}  // namespace fe
