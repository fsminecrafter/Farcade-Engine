// Convex shapes for the built-in physics: support functions, GJK distance,
// EPA penetration and contact manifolds (up to 4 points) by clipping the
// supporting faces of the two shapes. Used by physics.cpp; no standard C++
// library, no libm.
#pragma once

#include "core/math.h"
#include <stdint.h>

namespace fe {

// A convex hull of a point set. Vertices are relative to `center` (the
// bounding-box centre of the points, in the space the points were given in).
struct Hull {
    Vec3* v = nullptr;
    int nv = 0;
    int* tri = nullptr;             // 3 vertex indices per triangle
    Vec3* tn = nullptr;             // unit outward normal per triangle
    int nt = 0;
    Vec3 center;
    Vec3 half;                      // bounding box half extents (unscaled)
};

// Builds the hull of `n` points (duplicates and interior points are dropped;
// more than 1024 points are sampled). Flat or degenerate sets give the hull
// of their (slightly thickened) bounding box. nullptr when n < 1.
Hull* hull_build(const Vec3* pts, int n);
void hull_free(Hull* h);

enum class ShapeKind : uint8_t { None, Sphere, Box, Capsule, Cylinder, Hull };

// A convex shape in world space. Sphere and capsule are a "core" (point,
// segment) plus a radius `r`; box, cylinder and hull have r = 0.
struct Convex {
    ShapeKind kind = ShapeKind::None;
    Vec3 c;                         // centre (the body's centre of mass)
    Vec3 ax[3];                     // unit axes (local x, y, z in world space)
    Vec3 he;                        // Box: half extents. Capsule: (0, core half length, 0).
                                    // Cylinder: (radius, half height, radius)
    float r = 0;                    // Sphere, Capsule: radius
    const Hull* hull = nullptr;
    Vec3 scale = Vec3(1, 1, 1);     // Hull: per-axis scale along the local axes
    Vec3 lo, hi;                    // world AABB (set by convex_bounds)
};

Vec3 convex_support(const Convex& s, const Vec3& dir);      // of the core
void convex_bounds(Convex* s);

struct ContactPoint {
    Vec3 pos;                       // world, midway between the surfaces
    float sep;                      // separation along the normal; < 0 = penetration
};

struct ContactManifold {
    Vec3 n;                         // unit, from a to b
    int count = 0;
    ContactPoint p[4];
};

// Contacts between a and b whose separation is below `margin`. false when
// the shapes are further apart than that.
bool convex_collide(const Convex& a, const Convex& b, float margin, ContactManifold* out);
// Overlap test (surfaces touching or interpenetrating).
bool convex_overlap(const Convex& a, const Convex& b);

}  // namespace fe
