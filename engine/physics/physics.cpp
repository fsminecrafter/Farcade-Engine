#include "physics/physics.h"

#include <stdlib.h>
#include <string.h>

namespace fe {

struct PhysicsWorld::Body {
    Entity e;
};

namespace {

// A collider in world space for one step.
struct Shape {
    Entity e;
    bool sphere;                // else an oriented box
    Vec3 c;                     // center
    float r;                    // sphere radius; bounding radius for boxes
    Vec3 ax[3];                 // box axes (unit)
    Vec3 he;                    // box half extents
    bool trigger;
    float friction, bounce;
    uint32_t layer;
    // The body (if any): inverse mass (0 = does not move), velocity.
    float inv_mass;
    bool has_body;
    RigidBody3D* rb;
    Vec3 delta;                 // world-space movement this step
};

float maxf(float a, float b) { return a > b ? a : b; }
float absf(float a) { return a < 0 ? -a : a; }

Vec3 column(const Mat4& m, int c) { return Vec3(m.m[c * 4 + 0], m.m[c * 4 + 1], m.m[c * 4 + 2]); }

bool pair_has(const uint64_t* pairs, int count, Entity a, Entity b) {
    for (int i = 0; i < count; i++)
        if (pairs[i * 2] == a && pairs[i * 2 + 1] == b) return true;
    return false;
}

void pair_add(uint64_t** pairs, int* count, int* cap, Entity a, Entity b) {
    if (*count == *cap) {
        int nc = *cap ? *cap * 2 : 64;
        uint64_t* n = (uint64_t*)realloc(*pairs, sizeof(uint64_t) * 2 * nc);
        if (!n) return;
        *pairs = n;
        *cap = nc;
    }
    (*pairs)[*count * 2] = a;
    (*pairs)[*count * 2 + 1] = b;
    (*count)++;
}

// Contact between a and b: normal from a to b and penetration depth.
bool sphere_sphere(const Shape& a, const Shape& b, Vec3* n, float* depth) {
    Vec3 d = b.c - a.c;
    float dist2 = dot(d, d), rs = a.r + b.r;
    if (dist2 >= rs * rs) return false;
    float dist = fsqrt(dist2);
    *n = dist > 1e-6f ? d * (1.0f / dist) : Vec3(0, 1, 0);
    *depth = rs - dist;
    return true;
}

// Sphere a against box b.
bool sphere_box(const Shape& s, const Shape& b, Vec3* n, float* depth) {
    Vec3 d = s.c - b.c;
    float p[3] = { dot(d, b.ax[0]), dot(d, b.ax[1]), dot(d, b.ax[2]) };
    float h[3] = { b.he.x, b.he.y, b.he.z };
    float q[3];
    bool inside = true;
    for (int i = 0; i < 3; i++) {
        q[i] = p[i] < -h[i] ? -h[i] : (p[i] > h[i] ? h[i] : p[i]);
        if (q[i] != p[i]) inside = false;
    }
    if (!inside) {
        Vec3 closest = b.c + b.ax[0] * q[0] + b.ax[1] * q[1] + b.ax[2] * q[2];
        Vec3 out = s.c - closest;                       // box -> sphere
        float dist2 = dot(out, out);
        if (dist2 >= s.r * s.r) return false;
        float dist = fsqrt(dist2);
        Vec3 box_to_sphere = dist > 1e-6f ? out * (1.0f / dist) : b.ax[1];
        *n = -box_to_sphere;                            // sphere -> box
        *depth = s.r - dist;
        return true;
    }
    // The center is inside: out through the nearest face.
    int best = 0;
    float best_d = 1e30f;
    for (int i = 0; i < 3; i++) {
        float dd = h[i] - absf(p[i]);
        if (dd < best_d) { best_d = dd; best = i; }
    }
    Vec3 box_to_sphere = b.ax[best] * (p[best] < 0 ? -1.0f : 1.0f);
    *n = -box_to_sphere;
    *depth = best_d + s.r;
    return true;
}

// Two oriented boxes, separating axis test (15 axes).
bool box_box(const Shape& a, const Shape& b, Vec3* n, float* depth) {
    Vec3 t = b.c - a.c;
    Vec3 axes[15];
    int na = 0;
    for (int i = 0; i < 3; i++) axes[na++] = a.ax[i];
    for (int i = 0; i < 3; i++) axes[na++] = b.ax[i];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            Vec3 c = cross(a.ax[i], b.ax[j]);
            float l2 = dot(c, c);
            if (l2 > 1e-6f) axes[na++] = c * (1.0f / fsqrt(l2));
        }
    float ha[3] = { a.he.x, a.he.y, a.he.z }, hb[3] = { b.he.x, b.he.y, b.he.z };
    float best = 1e30f;
    Vec3 best_axis(0, 1, 0);
    for (int k = 0; k < na; k++) {
        Vec3 L = axes[k];
        float ra = 0, rb = 0;
        for (int i = 0; i < 3; i++) {
            ra += ha[i] * absf(dot(a.ax[i], L));
            rb += hb[i] * absf(dot(b.ax[i], L));
        }
        float dist = dot(t, L);
        float overlap = ra + rb - absf(dist);
        if (overlap <= 0) return false;
        // Face axes first and a small bias, so resting boxes use face normals.
        if (overlap < best - (k >= 6 ? 1e-4f : 0.0f)) {
            best = overlap;
            best_axis = dist < 0 ? -L : L;
        }
    }
    *n = best_axis;
    *depth = best;
    return true;
}

bool contact(const Shape& a, const Shape& b, Vec3* n, float* depth) {
    if (a.sphere && b.sphere) return sphere_sphere(a, b, n, depth);
    if (a.sphere) return sphere_box(a, b, n, depth);
    if (b.sphere) {
        if (!sphere_box(b, a, n, depth)) return false;
        *n = -*n;
        return true;
    }
    return box_box(a, b, n, depth);
}

}  // namespace

PhysicsWorld::PhysicsWorld() {}
PhysicsWorld::~PhysicsWorld() { shutdown(); }

bool PhysicsWorld::init(World* world, const PhysicsEvents& events) {
    shutdown();
    if (!world) return false;
    world_ = world;
    events_ = events;
    system_ = world->add_system(Phase::Update, system, this);
    return system_ != 0;
}

void PhysicsWorld::shutdown() {
    if (world_ && system_ && world_->valid()) world_->remove_system(system_);
    world_ = nullptr;
    system_ = 0;
    free(bodies_);
    free(overlaps_);
    free(touching_);
    bodies_ = nullptr;
    overlaps_ = touching_ = nullptr;
    count_ = cap_ = overlap_count_ = overlap_cap_ = touching_count_ = touching_cap_ = 0;
    accumulator_ = 0;
    stats_ = PhysicsStats();
}

void PhysicsWorld::add(Entity e) {
    if (!world_ || !(world_->has(e, ModuleKind::RigidBody3D) || world_->has(e, ModuleKind::Collider3D))) return;
    for (int i = 0; i < count_; i++)
        if (bodies_[i].e == e) return;
    if (count_ == cap_) {
        int nc = cap_ ? cap_ * 2 : 64;
        Body* n = (Body*)realloc(bodies_, sizeof(Body) * nc);
        if (!n) return;
        bodies_ = n;
        cap_ = nc;
    }
    bodies_[count_++].e = e;
}

void PhysicsWorld::remove(Entity e) {
    for (int i = 0; i < count_; i++)
        if (bodies_[i].e == e) { bodies_[i] = bodies_[--count_]; return; }
}

void PhysicsWorld::system(World& w, float dt, void* user) {
    PhysicsWorld* p = (PhysicsWorld*)user;
    if (dt <= 0) return;
    if (dt > 0.25f) dt = 0.25f;
    p->accumulator_ += dt;
    int n = 0;
    while (p->accumulator_ >= p->fixed_step && n < p->max_steps_per_frame) {
        p->step(p->fixed_step);
        p->accumulator_ -= p->fixed_step;
        n++;
    }
    if (n == p->max_steps_per_frame) p->accumulator_ = 0;     // too slow: drop time rather than spiral
    (void)w;
}

void PhysicsWorld::step(float dt) {
    if (!world_) return;
    // Drop dead entities and those that lost both modules.
    for (int i = 0; i < count_;) {
        Entity e = bodies_[i].e;
        if (!world_->alive(e) || !(world_->has(e, ModuleKind::RigidBody3D) || world_->has(e, ModuleKind::Collider3D))) bodies_[i] = bodies_[--count_];
        else i++;
    }
    Shape* shapes = (Shape*)malloc(sizeof(Shape) * (count_ ? count_ : 1));
    if (!shapes) return;
    int ns = 0;
    stats_.bodies = stats_.colliders = 0;
    for (int i = 0; i < count_; i++) {
        Entity e = bodies_[i].e;
        Shape& s = shapes[ns];
        s = Shape();
        s.e = e;
        // World transform: a root's from its Transform (current), a child's
        // from the last computed WorldTransform.
        Mat4 m;
        Entity parent = world_->parent(e);
        const Transform* t = world_->transform(e);
        if (!t) continue;
        if (!parent) m = Mat4::trs(t->position, t->rotation, t->scale);
        else if (const WorldTransform* wt = world_->world_transform(e)) m = wt->matrix;
        else continue;
        s.rb = world_->has(e, ModuleKind::RigidBody3D) ? (RigidBody3D*)world_->get_mut(e, ModuleKind::RigidBody3D) : nullptr;
        s.has_body = s.rb != nullptr;
        if (s.has_body) stats_.bodies++;
        s.inv_mass = (s.rb && s.rb->type == BodyType::Dynamic && s.rb->mass > 0) ? 1.0f / s.rb->mass : 0.0f;
        // Gravity, damping, velocity.
        if (s.inv_mass > 0) {
            RigidBody3D& rb = *s.rb;
            if (rb.use_gravity) rb.velocity += gravity * (rb.gravity_scale * dt);
            float damp = 1.0f / (1.0f + rb.linear_damping * dt);
            rb.velocity = rb.velocity * damp;
            s.delta = rb.velocity * dt;
        } else if (s.rb && s.rb->type == BodyType::Kinematic) {
            s.delta = s.rb->velocity * dt;          // kinematic bodies move by their velocity, nothing pushes them
        }
        const Collider3D* col = world_->has(e, ModuleKind::Collider3D) ? (const Collider3D*)world_->get(e, ModuleKind::Collider3D) : nullptr;
        if (!col) {
            // A body without a collider still moves; it just touches nothing.
            Vec3 c(m.m[12], m.m[13], m.m[14]);
            s.c = c + s.delta;
            s.r = -1;                               // no shape
            ns++;
            continue;
        }
        stats_.colliders++;
        Vec3 sx = column(m, 0), sy = column(m, 1), sz = column(m, 2);
        float lx = length(sx), ly = length(sy), lz = length(sz);
        s.c = transform_point(m, col->center) + s.delta;
        s.trigger = col->mode == ColliderMode::Trigger;
        s.friction = col->friction;
        s.bounce = col->bounciness;
        s.layer = col->layer;
        if (col->shape == ColliderShape::Sphere) {
            s.sphere = true;
            s.r = col->radius * maxf(lx, maxf(ly, lz));
        } else {
            s.sphere = false;
            s.ax[0] = lx > 0 ? sx * (1.0f / lx) : Vec3(1, 0, 0);
            s.ax[1] = ly > 0 ? sy * (1.0f / ly) : Vec3(0, 1, 0);
            s.ax[2] = lz > 0 ? sz * (1.0f / lz) : Vec3(0, 0, 1);
            Vec3 size = col->size;
            if (col->shape == ColliderShape::Capsule) size = Vec3(col->radius * 2, col->height, col->radius * 2);
            s.he = Vec3(size.x * 0.5f * lx, size.y * 0.5f * ly, size.z * 0.5f * lz);
            s.r = length(s.he);
        }
        ns++;
    }

    // Pairs: push apart, bounce, and note triggers and contacts.
    uint64_t* new_overlaps = nullptr;
    int new_overlap_count = 0, new_overlap_cap = 0;
    uint64_t* new_touching = nullptr;
    int new_touching_count = 0, new_touching_cap = 0;
    stats_.contacts = 0;
    for (int iter = 0; iter < 3; iter++) {
        for (int i = 0; i < ns; i++) {
            Shape& a = shapes[i];
            if (a.r < 0) continue;
            for (int j = i + 1; j < ns; j++) {
                Shape& b = shapes[j];
                if (b.r < 0 || !(a.layer & b.layer)) continue;
                bool trig = a.trigger || b.trigger;
                if (!trig && a.inv_mass == 0 && b.inv_mass == 0) continue;
                if (trig && !a.has_body && !b.has_body) continue;
                Vec3 d = b.c - a.c;
                float rr = a.r + b.r;
                if (dot(d, d) > rr * rr) continue;
                Vec3 nrm;
                float depth;
                if (!contact(a, b, &nrm, &depth)) continue;
                if (trig) {
                    if (iter == 0) {
                        // Report it to the trigger's owner; both if both are triggers.
                        if (a.trigger) pair_add(&new_overlaps, &new_overlap_count, &new_overlap_cap, a.e, b.e);
                        if (b.trigger) pair_add(&new_overlaps, &new_overlap_count, &new_overlap_cap, b.e, a.e);
                    }
                    continue;
                }
                if (iter == 0) {
                    stats_.contacts++;
                    Entity lo = a.e < b.e ? a.e : b.e, hi = a.e < b.e ? b.e : a.e;
                    pair_add(&new_touching, &new_touching_count, &new_touching_cap, lo, hi);
                }
                float w = a.inv_mass + b.inv_mass;
                // Push out (most of the way; a little slop keeps resting contacts stable).
                float corr = (depth - 0.001f) * 0.9f;
                if (corr > 0) {
                    Vec3 move = nrm * (corr / w);
                    if (a.inv_mass > 0) { a.c -= move * a.inv_mass; a.delta -= move * a.inv_mass; }
                    if (b.inv_mass > 0) { b.c += move * b.inv_mass; b.delta += move * b.inv_mass; }
                }
                // Velocities along the normal: bounce; along the surface: friction.
                Vec3 va = a.inv_mass > 0 ? a.rb->velocity : Vec3(0, 0, 0);
                Vec3 vb = b.inv_mass > 0 ? b.rb->velocity : Vec3(0, 0, 0);
                Vec3 rel = vb - va;
                float vn = dot(rel, nrm);
                if (vn >= 0) continue;
                float e = maxf(a.bounce, b.bounce);
                if (-vn < 0.6f) e = 0;                  // resting: no bouncing in place
                float jn = -(1.0f + e) * vn / w;
                Vec3 tangent = rel - nrm * vn;
                float tl = length(tangent);
                Vec3 impulse = nrm * jn;
                if (tl > 1e-5f) {
                    tangent = tangent * (1.0f / tl);
                    float mu = fsqrt(a.friction * b.friction);
                    float jt = -tl / w;
                    float lim = mu * jn;
                    if (jt < -lim) jt = -lim;
                    impulse += tangent * jt;
                }
                if (a.inv_mass > 0) a.rb->velocity -= impulse * a.inv_mass;
                if (b.inv_mass > 0) b.rb->velocity += impulse * b.inv_mass;
            }
        }
    }

    // Movement back into the transforms (children: into their parent's space).
    for (int i = 0; i < ns; i++) {
        Shape& s = shapes[i];
        if (dot(s.delta, s.delta) == 0) continue;
        Transform* t = world_->transform(s.e);
        if (!t) continue;
        Entity parent = world_->parent(s.e);
        Vec3 local = s.delta;
        if (parent) {
            const WorldTransform* pw = world_->world_transform(parent);
            Mat4 inv;
            if (pw && invert(pw->matrix, &inv)) local = transform_dir(inv, s.delta);
        }
        t->position += local;
    }

    // Events: triggers entered and left, first contacts.
    if (events_.trigger_enter)
        for (int i = 0; i < new_overlap_count; i++)
            if (!pair_has(overlaps_, overlap_count_, new_overlaps[i * 2], new_overlaps[i * 2 + 1]))
                events_.trigger_enter(new_overlaps[i * 2], new_overlaps[i * 2 + 1], events_.user);
    if (events_.trigger_exit)
        for (int i = 0; i < overlap_count_; i++)
            if (!pair_has(new_overlaps, new_overlap_count, overlaps_[i * 2], overlaps_[i * 2 + 1]))
                events_.trigger_exit(overlaps_[i * 2], overlaps_[i * 2 + 1], events_.user);
    if (events_.collision)
        for (int i = 0; i < new_touching_count; i++)
            if (!pair_has(touching_, touching_count_, new_touching[i * 2], new_touching[i * 2 + 1]))
                events_.collision(new_touching[i * 2], new_touching[i * 2 + 1], events_.user);
    free(overlaps_);
    overlaps_ = new_overlaps;
    overlap_count_ = new_overlap_count;
    overlap_cap_ = new_overlap_cap;
    free(touching_);
    touching_ = new_touching;
    touching_count_ = new_touching_count;
    touching_cap_ = new_touching_cap;
    free(shapes);
    stats_.steps++;
}

}  // namespace fe
