// Force Field volumes (ForceField module, scene/modules.h): the math shared
// by physics (rigid bodies) and particles. A volume is the module plus the
// object's world matrix; its size follows the object's scale.
#pragma once

#include "core/math.h"
#include "scene/modules.h"

namespace fe {

struct FieldVolume {
    ForceField f;
    Mat4 world;
    Mat4 inv;
    Vec3 center;        // world position of the object
    Vec3 up;            // world direction of the object's +Y (the vortex axis)
    Vec3 dir;           // world direction of the Directional force (normalized)
};

// Fills `out` from the module and the object's world matrix. False when the
// field is disabled or the matrix cannot be inverted (nothing to evaluate).
bool field_volume(const ForceField& f, const Mat4& world, FieldVolume* out);

// 0 outside the volume, else 0..1: 1 everywhere with no falloff, fading to
// 0 at the edge with Linear or Quadratic falloff.
float field_weight(const FieldVolume& v, const Vec3& world_point);

// The force in newtons at a world point at time t seconds (zero outside the
// volume). Radial pushes away from the center, Vortex turns around the
// object's Y axis; turbulence varies the force in space and time.
Vec3 field_force(const FieldVolume& v, const Vec3& world_point, float time);

}  // namespace fe
