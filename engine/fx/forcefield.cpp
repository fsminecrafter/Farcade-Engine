#include "fx/forcefield.h"

namespace fe {

namespace {
Vec3 column(const Mat4& m, int c) { return Vec3(m.m[c * 4 + 0], m.m[c * 4 + 1], m.m[c * 4 + 2]); }
}

bool field_volume(const ForceField& f, const Mat4& world, FieldVolume* out) {
    if (!f.enabled) return false;
    out->f = f;
    out->world = world;
    if (!invert(world, &out->inv)) return false;
    out->center = Vec3(world.m[12], world.m[13], world.m[14]);
    out->up = normalize(column(world, 1));
    Vec3 d = normalize(transform_dir(world, f.direction));
    out->dir = length(d) > 0 ? d : Vec3(0, 0, -1);
    return true;
}

float field_weight(const FieldVolume& v, const Vec3& p) {
    Vec3 l = transform_point(v.inv, p);
    float d;
    if (v.f.shape == FieldShape::Sphere) {
        float r = v.f.radius > 1e-4f ? v.f.radius : 1e-4f;
        d = length(l) / r;
    } else {
        float hx = v.f.size.x * 0.5f, hy = v.f.size.y * 0.5f, hz = v.f.size.z * 0.5f;
        if (hx < 1e-4f) hx = 1e-4f;
        if (hy < 1e-4f) hy = 1e-4f;
        if (hz < 1e-4f) hz = 1e-4f;
        d = fmaxf_(fabsf_(l.x) / hx, fmaxf_(fabsf_(l.y) / hy, fabsf_(l.z) / hz));
    }
    if (d > 1.0f) return 0.0f;
    switch (v.f.falloff) {
    case FieldFalloff::Linear: return 1.0f - d;
    case FieldFalloff::Quadratic: return (1.0f - d) * (1.0f - d);
    default: return 1.0f;
    }
}

Vec3 field_force(const FieldVolume& v, const Vec3& p, float t) {
    float w = field_weight(v, p);
    if (w <= 0.0f) return Vec3(0, 0, 0);
    Vec3 dir;
    switch (v.f.mode) {
    case FieldMode::Radial: dir = normalize(p - v.center); break;
    case FieldMode::Vortex: dir = normalize(cross(v.up, p - v.center)); break;
    default: dir = v.dir; break;
    }
    Vec3 force = dir;
    if (v.f.turbulence > 0.0f) {
        float inv_s = 6.2831853f / (v.f.turbulence_scale > 0.01f ? v.f.turbulence_scale : 0.01f);
        Vec3 q = p * inv_s;
        float tt = t * v.f.turbulence_speed;
        Vec3 n(fsin(q.y * 1.3f + q.z * 0.7f + tt * 1.1f),
               fsin(q.z * 1.1f + q.x * 0.9f + tt * 1.3f + 1.7f),
               fsin(q.x * 1.2f + q.y * 0.8f + tt * 0.9f + 3.1f));
        float k = v.f.turbulence;
        force = dir * (1.0f + k * 0.5f * n.x) + n * (k * 0.7f);
    }
    return force * (v.f.strength * w);
}

}  // namespace fe
