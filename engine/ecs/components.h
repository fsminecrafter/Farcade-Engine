// Components of the entity system that are not modules (modules are in
// scene/modules.h): the local Transform, the computed WorldTransform and the
// Renderable. Plain data, copyable with memcpy.
#pragma once

#include "core/math.h"
#include "scene/modules.h"
#include <stdint.h>

namespace fe {

// Local transform, relative to the parent entity (or the world, for roots).
struct Transform {
    Vec3 position;
    Quat rotation;
    Vec3 scale = Vec3(1, 1, 1);
};

// Computed from Transform down the hierarchy by the transform system
// (World::progress / World::update_transforms). Read only for everybody else.
struct WorldTransform {
    Mat4 matrix;
};

// What the renderer draws for an entity. `mesh` is a plain index that the
// editor maps to its MeshKind (Cube, Plane, Sphere, Cylinder, Model);
// `model_path` is used by Model.
struct Renderable {
    uint8_t mesh = 0;
    char model_path[kPathMax] = {};
    Vec3 color = Vec3(1, 1, 1);
    bool visible = true;
};

}  // namespace fe
