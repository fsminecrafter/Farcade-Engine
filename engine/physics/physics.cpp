#include "physics/physics.h"
#include "physics/convex.h"
#include "fx/forcefield.h"

#include <stdlib.h>
#include <string.h>
#include <new>

namespace fe {

struct PhysicsWorld::Body {
    Entity e;
};

struct PhysicsWorld::MeshEntry {
    Entity e;
    Hull* hull;
};

namespace {

constexpr float kMargin = 0.02f;        // contacts are made this far before touching (speculative)
constexpr float kSlop = 0.005f;         // penetration left alone
constexpr float kBeta = 0.2f;           // share of the penetration pushed out per step
constexpr float kMaxPush = 3.0f;        // m/s, cap of the position correction
constexpr float kRestThreshold = 1.0f;  // m/s: slower impacts do not bounce
constexpr float kTouch = 0.005f;        // separation that counts as touching (first contact)
constexpr float kTouchKeep = 0.03f;     // ... and as still touching afterwards
constexpr float kMatchDist = 0.03f;     // warm start: contact points this close are the same point

float maxf(float a, float b) { return a > b ? a : b; }
float absf(float a) { return a < 0 ? -a : a; }

Vec3 column(const Mat4& m, int c) { return Vec3(m.m[c * 4 + 0], m.m[c * 4 + 1], m.m[c * 4 + 2]); }

Vec3 mulm(const float* m, const Vec3& v) {
    return Vec3(m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z);
}

Quat conjugate(const Quat& q) { Quat r; r.x = -q.x; r.y = -q.y; r.z = -q.z; r.w = q.w; return r; }

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

// One simulated entity for one step.
struct Item {
    Entity e = 0;
    RigidBody3D* rb = nullptr;
    CharacterController3D* controller = nullptr;
    Convex cv;
    bool shape = false;
    bool trigger = false;
    float friction = 0.5f, bounce = 0;
    uint32_t layer = 1;
    bool movable = false;               // dynamic or kinematic: its pose is integrated
    bool dynamic = false;
    bool rotates = false;
    Vec3 com, off;                      // centre of mass; origin - com
    Vec3 v, w, vp, wp;                  // velocity, angular velocity, position-correction velocities
    float im = 0;
    float ii[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };    // inverse inertia tensor, world frame
    Vec3 ax[3];
    Vec3 pos0;                          // the origin before the step
    Mat4 mat;                           // the pose (object to world) at the start of the step
};

}  // namespace

struct ManifoldPt {
    Vec3 pos;
    float sep = 0;
    Vec3 la;                            // in A's frame, for matching with the last step
    float ln = 0;                       // accumulated normal impulse (warm start)
    Vec3 lt;                            // accumulated friction impulse (world)
    // Solver working data.
    Vec3 rA, rB;
    float mn = 0, mt1 = 0, mt2 = 0, target = 0, l1 = 0, l2 = 0, lp = 0, bias = 0;
};

struct PhysicsWorld::Manifold {
    Entity ea = 0, eb = 0;
    int ia = 0, ib = 0;
    Vec3 n, t1, t2;
    float mu = 0, e = 0;
    int count = 0;
    bool touching = false;
    ManifoldPt p[4];
};

struct PhysicsWorld::JointState {
    Entity e = 0;
    bool init = false;
    Vec3 la;                            // anchor in A's object space
    Vec3 lb;                            // ... in B's object space (the world position without B)
    Vec3 axl, axb;                      // hinge axis in A's and B's frames
    Vec3 pl, pb;                        // reference perpendicular in A's and B's frames (angle zero)
    Vec3 rel[3];                        // B's axes in A's frame (Fixed)
    Vec3 acc_lin, acc_ang;              // accumulated impulses, for warm starting
    float acc_limit = 0, acc_limit2 = 0, acc_motor = 0, acc_spring = 0;   // upper limit (<= 0), lower limit (>= 0), motor, spring
};

namespace {

typedef PhysicsWorld::Manifold Manifold;

float eff_mass(const Item& A, const Item& B, const Vec3& rA, const Vec3& rB, const Vec3& d) {
    Vec3 ra = cross(rA, d), rb = cross(rB, d);
    float k = A.im + B.im + dot(cross(mulm(A.ii, ra), rA), d) + dot(cross(mulm(B.ii, rb), rB), d);
    return k > 1e-12f ? 1.0f / k : 0.0f;
}

Vec3 rel_vel(const Item& A, const Item& B, const Vec3& rA, const Vec3& rB) {
    return (B.v + cross(B.w, rB)) - (A.v + cross(A.w, rA));
}

void apply(Item& A, Item& B, const Vec3& rA, const Vec3& rB, const Vec3& P) {
    if (A.im > 0 || A.rotates) { A.v -= P * A.im; A.w -= mulm(A.ii, cross(rA, P)); }
    if (B.im > 0 || B.rotates) { B.v += P * B.im; B.w += mulm(B.ii, cross(rB, P)); }
}

void apply_pseudo(Item& A, Item& B, const Vec3& rA, const Vec3& rB, const Vec3& P) {
    if (A.im > 0 || A.rotates) { A.vp -= P * A.im; A.wp -= mulm(A.ii, cross(rA, P)); }
    if (B.im > 0 || B.rotates) { B.vp += P * B.im; B.wp += mulm(B.ii, cross(rB, P)); }
}

void shell_sort(int* idx, const float* key, int n) {
    for (int gap = n / 2; gap > 0; gap /= 2)
        for (int i = gap; i < n; i++) {
            int t = idx[i];
            float k = key[t];
            int j = i;
            while (j >= gap && key[idx[j - gap]] > k) { idx[j] = idx[j - gap]; j -= gap; }
            idx[j] = t;
        }
}

uint64_t pair_hash(Entity a, Entity b) {
    uint64_t h = a * 0x9E3779B97F4A7C15ull ^ (b + 0x7F4A7C15ull) * 0xC2B2AE3D27D4EB4Full;
    return h ^ (h >> 29);
}


// ---- joints

constexpr float kJointBeta = 0.2f;      // share of a joint's error corrected per step
constexpr float kJointMaxPush = 4.0f;   // m/s (rad/s) cap of that correction

Vec3 rot_of(const Item& it, const Vec3& v) { return it.ax[0] * v.x + it.ax[1] * v.y + it.ax[2] * v.z; }
Vec3 unrot_of(const Item& it, const Vec3& w) { return Vec3(dot(it.ax[0], w), dot(it.ax[1], w), dot(it.ax[2], w)); }

void apply_ang(Item& A, Item& B, const Vec3& L) {
    if (A.rotates) A.w -= mulm(A.ii, L);
    if (B.rotates) B.w += mulm(B.ii, L);
}

float ang_mass(const Item& A, const Item& B, const Vec3& d) {
    float k = dot(d, mulm(A.ii, d)) + dot(d, mulm(B.ii, d));
    return k > 1e-12f ? 1.0f / k : 0.0f;
}

bool wants_entity(const World* w, Entity e) {
    return w->has(e, ModuleKind::RigidBody3D) || w->has(e, ModuleKind::Collider3D) || w->has(e, ModuleKind::CharacterController3D) ||
           w->has(e, ModuleKind::Joint3D) || w->has(e, ModuleKind::ForceField);
}
bool has_item(const World* w, Entity e) {
    return w->has(e, ModuleKind::RigidBody3D) || w->has(e, ModuleKind::Collider3D) || w->has(e, ModuleKind::CharacterController3D);
}

// The pose of an entity as the gather step reads it.
bool pose_of(const World* w, Entity e, Mat4* m) {
    const Transform* t = w->transform(e);
    if (!t) return false;
    if (!w->parent(e)) { *m = Mat4::trs(t->position, t->rotation, t->scale); return true; }
    const WorldTransform* wt = w->world_transform(e);
    if (!wt) return false;
    *m = wt->matrix;
    return true;
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// A joint in one step: its bodies, the anchors and the rows' masses.
struct JointRow {
    PhysicsWorld::JointState* js = nullptr;
    Joint3D* m = nullptr;
    Item* A = nullptr;
    Item* B = nullptr;                  // the world: &dummy
    Item dummy;
    Vec3 pa, pb, rA, rB;
    Vec3 axis;                          // Hinge: A's axis in the world
    Vec3 t1, t2;                        // Hinge: perpendicular to it
    Vec3 e_ang;                         // angular error (the rotation B needs to reach its place)
    float mlin[3] = { 0, 0, 0 };
    float mang[3] = { 0, 0, 0 };
    float m_axis = 0;                   // about the hinge axis
    float theta = 0;                    // hinge angle, radians
    float spring_m = 0, gamma = 0, beta_s = 0, len = 0;
    Vec3 n;                             // spring direction
    bool lin = true, fixed = false, hinge = false, spring = false;
};

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
    for (int i = 0; i < joint_count_; i++) joints_[i].~JointState();
    free(joints_);
    joints_ = nullptr;
    joint_count_ = joint_cap_ = 0;
    time_ = 0;
    free(overlaps_);
    free(manifolds_);
    for (int i = 0; i < mesh_count_; i++) hull_free(meshes_[i].hull);
    free(meshes_);
    bodies_ = nullptr;
    overlaps_ = nullptr;
    manifolds_ = nullptr;
    meshes_ = nullptr;
    count_ = cap_ = overlap_count_ = overlap_cap_ = manifold_count_ = manifold_cap_ = mesh_count_ = mesh_cap_ = 0;
    accumulator_ = 0;
    stats_ = PhysicsStats();
}

void PhysicsWorld::add(Entity e) {
    if (!world_ || !wants_entity(world_, e)) return;
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

bool PhysicsWorld::break_joint(Entity e) {
    if (!world_ || !world_->has(e, ModuleKind::Joint3D)) return false;
    Joint3D* j = (Joint3D*)world_->get_mut(e, ModuleKind::Joint3D);
    j->broken = true;
    return true;
}

void PhysicsWorld::remove(Entity e) {
    for (int i = 0; i < count_; i++)
        if (bodies_[i].e == e) { bodies_[i] = bodies_[--count_]; break; }
    for (int i = 0; i < mesh_count_; i++)
        if (meshes_[i].e == e) { hull_free(meshes_[i].hull); meshes_[i] = meshes_[--mesh_count_]; break; }
}

void PhysicsWorld::set_mesh(Entity e, const Vec3* points, int count) {
    int at = -1;
    for (int i = 0; i < mesh_count_; i++) if (meshes_[i].e == e) at = i;
    if (at >= 0) { hull_free(meshes_[at].hull); meshes_[at] = meshes_[--mesh_count_]; at = -1; }
    if (!points || count <= 0) return;
    Hull* h = hull_build(points, count);
    if (!h) return;
    if (mesh_count_ == mesh_cap_) {
        int nc = mesh_cap_ ? mesh_cap_ * 2 : 16;
        MeshEntry* n = (MeshEntry*)realloc(meshes_, sizeof(MeshEntry) * nc);
        if (!n) { hull_free(h); return; }
        meshes_ = n;
        mesh_cap_ = nc;
    }
    meshes_[mesh_count_].e = e;
    meshes_[mesh_count_].hull = h;
    mesh_count_++;
}

void PhysicsWorld::set_terrains(const PhysicsTerrain* terrains, int count) {
    if (count < 0) count = 0;
    if (count > kMaxPhysicsTerrains) count = kMaxPhysicsTerrains;
    for (int i = 0; i < count; i++) terrains_[i] = terrains[i];
    terrain_count_ = count;
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
    if (!world_ || dt <= 0) return;
    // Drop dead entities and those that lost both modules.
    for (int i = 0; i < count_;) {
        Entity e = bodies_[i].e;
        if (!world_->alive(e) || !wants_entity(world_, e)) bodies_[i] = bodies_[--count_];
        else i++;
    }
    for (int i = 0; i < mesh_count_;) {
        if (!world_->alive(meshes_[i].e)) { hull_free(meshes_[i].hull); meshes_[i] = meshes_[--mesh_count_]; }
        else i++;
    }
    time_ += dt;
    // Force fields that push bodies.
    FieldVolume fields[32];
    int nfields = 0;
    for (int i = 0; i < count_ && nfields < 32; i++) {
        Entity fe_ = bodies_[i].e;
        if (!world_->has(fe_, ModuleKind::ForceField)) continue;
        const ForceField* f = (const ForceField*)world_->get(fe_, ModuleKind::ForceField);
        Mat4 fm;
        if (f->affect_bodies && pose_of(world_, fe_, &fm) && field_volume(*f, fm, &fields[nfields])) nfields++;
    }
    int cap = count_ + terrain_count_ + 1;
    Item* items = (Item*)malloc(sizeof(Item) * cap);
    int* order = (int*)malloc(sizeof(int) * cap);
    float* keys = (float*)malloc(sizeof(float) * cap);
    if (!items || !order || !keys) { free(items); free(order); free(keys); return; }
    int ns = 0;
    stats_.bodies = stats_.colliders = 0;

    // ---- gather: poses, shapes, mass properties, velocities with gravity and damping
    for (int i = 0; i < count_; i++) {
        Entity e = bodies_[i].e;
        if (!has_item(world_, e)) continue;       // a joint or a field only
        Mat4 m;
        Entity parent = world_->parent(e);
        const Transform* t = world_->transform(e);
        if (!t) continue;
        if (!parent) m = Mat4::trs(t->position, t->rotation, t->scale);
        else if (const WorldTransform* wt = world_->world_transform(e)) m = wt->matrix;
        else continue;
        Item* ip = new (&items[ns]) Item();
        Item& it = *ip;
        it.e = e;
        it.mat = m;
        it.controller = world_->has(e, ModuleKind::CharacterController3D)
            ? (CharacterController3D*)world_->get_mut(e, ModuleKind::CharacterController3D) : nullptr;
        it.rb = !it.controller && world_->has(e, ModuleKind::RigidBody3D)
            ? (RigidBody3D*)world_->get_mut(e, ModuleKind::RigidBody3D) : nullptr;
        if (it.rb || it.controller) stats_.bodies++;
        Vec3 sx = column(m, 0), sy = column(m, 1), sz = column(m, 2);
        float lx = length(sx), ly = length(sy), lz = length(sz);
        it.ax[0] = lx > 0 ? sx * (1.0f / lx) : Vec3(1, 0, 0);
        it.ax[1] = ly > 0 ? sy * (1.0f / ly) : Vec3(0, 1, 0);
        it.ax[2] = lz > 0 ? sz * (1.0f / lz) : Vec3(0, 0, 1);
        Vec3 origin(m.m[12], m.m[13], m.m[14]);
        it.pos0 = origin;
        it.com = origin;

        // Shape and (diagonal, local) inertia for a unit mass.
        Vec3 inertia(0.1f, 0.1f, 0.1f);     // a body without a collider: a ball of radius 0.5
        const Collider3D* col = world_->has(e, ModuleKind::Collider3D) ? (const Collider3D*)world_->get(e, ModuleKind::Collider3D) : nullptr;
        if (col || it.controller) {
            stats_.colliders++;
            Convex& cv = it.cv;
            for (int k = 0; k < 3; k++) cv.ax[k] = it.ax[k];
            it.trigger = !it.controller && col && col->mode == ColliderMode::Trigger;
            it.friction = col ? col->friction : 0.5f;
            it.bounce = col ? col->bounciness : 0.0f;
            it.layer = col ? col->layer : 1;
            float rxz = maxf(lx, lz);
            ShapeKind kind = ShapeKind::Box;
            const Hull* hull = nullptr;
            if (!it.controller && col && col->shape == ColliderShape::Mesh) {
                for (int k = 0; k < mesh_count_; k++) if (meshes_[k].e == e) hull = meshes_[k].hull;
                if (hull) kind = ShapeKind::Hull;
            }
            Vec3 center;
            if (it.controller) {
                // Keep the capsule upright while the object turns to face its movement.
                cv.ax[0] = Vec3(1, 0, 0);
                cv.ax[1] = Vec3(0, 1, 0);
                cv.ax[2] = Vec3(0, 0, 1);
                kind = ShapeKind::Capsule;
                center = it.controller->center;
            } else {
                center = col->center;
                switch (col->shape == ColliderShape::Mesh && !hull ? ColliderShape::Box : col->shape) {
                case ColliderShape::Sphere: kind = ShapeKind::Sphere; break;
                case ColliderShape::Capsule: kind = ShapeKind::Capsule; break;
                case ColliderShape::Cylinder: kind = ShapeKind::Cylinder; break;
                case ColliderShape::Mesh: kind = ShapeKind::Hull; break;
                default: kind = ShapeKind::Box; break;
                }
                if (kind == ShapeKind::Hull) center = hull->center;
            }
            it.com = transform_point(m, center);
            cv.c = it.com;
            cv.kind = kind;
            if (it.controller) {
                float r = it.controller->radius * rxz, half = it.controller->height * 0.5f * ly;
                if (half <= r) {
                    cv.kind = ShapeKind::Sphere;
                    cv.r = r;
                } else {
                    cv.r = r;
                    cv.he = Vec3(0, half - r, 0);
                }
            } else if (kind == ShapeKind::Sphere) {
                cv.r = col->radius * maxf(lx, maxf(ly, lz));
                float i = 0.4f * cv.r * cv.r;
                inertia = Vec3(i, i, i);
            } else if (kind == ShapeKind::Capsule) {
                float r = col->radius * rxz, half = col->height * 0.5f * ly;
                if (half <= r) {                                    // too short for a capsule: a sphere
                    cv.kind = ShapeKind::Sphere;
                    cv.r = r;
                    float i = 0.4f * r * r;
                    inertia = Vec3(i, i, i);
                } else {
                    cv.r = r;
                    cv.he = Vec3(0, half - r, 0);
                    float L = 2 * cv.he.y;
                    float vc = kPi * r * r * L, vh = 4.0f / 3.0f * kPi * r * r * r;
                    float mc = vc / (vc + vh), mh = 1 - mc;         // mass shares (unit mass)
                    float iy = mc * r * r * 0.5f + mh * 0.4f * r * r;
                    float ix = mc * (r * r / 4 + L * L / 12) + mh * (0.4f * r * r + L * L / 4 + 3.0f * L * r / 8);
                    inertia = Vec3(ix, iy, ix);
                }
            } else if (kind == ShapeKind::Cylinder) {
                float r = col->radius * rxz, hh = col->height * 0.5f * ly;
                cv.he = Vec3(r, hh, r);
                float ix = (3 * r * r + 4 * hh * hh) / 12.0f;
                inertia = Vec3(ix, 0.5f * r * r, ix);
            } else if (kind == ShapeKind::Hull) {
                cv.hull = hull;
                cv.scale = Vec3(lx, ly, lz);
                Vec3 hh(hull->half.x * lx, hull->half.y * ly, hull->half.z * lz);
                inertia = Vec3((hh.y * hh.y + hh.z * hh.z) / 3.0f, (hh.x * hh.x + hh.z * hh.z) / 3.0f, (hh.x * hh.x + hh.y * hh.y) / 3.0f);
            } else {
                Vec3 size = col->size;
                cv.he = Vec3(size.x * 0.5f * lx, size.y * 0.5f * ly, size.z * 0.5f * lz);
                inertia = Vec3((cv.he.y * cv.he.y + cv.he.z * cv.he.z) / 3.0f, (cv.he.x * cv.he.x + cv.he.z * cv.he.z) / 3.0f,
                               (cv.he.x * cv.he.x + cv.he.y * cv.he.y) / 3.0f);
            }
            convex_bounds(&cv);
            it.shape = true;
        }
        it.off = origin - it.com;

        // Mass properties and velocities.
        if (it.controller) {
            it.controller->grounded = false;
            it.dynamic = true;
            it.movable = true;
            it.v = it.controller->velocity;
            it.im = 1.0f;
            if (it.controller->use_gravity)
                it.v += gravity * (it.controller->gravity_scale * dt);
        } else if (it.rb) {
            RigidBody3D& rb = *it.rb;
            bool dyn = rb.type == BodyType::Dynamic && rb.mass > 0;
            bool kin = rb.type == BodyType::Kinematic;
            it.dynamic = dyn;
            it.movable = dyn || kin;
            it.rotates = (dyn || kin) && !rb.freeze_rotation;
            it.v = rb.velocity;
            it.w = it.rotates ? rb.angular_velocity : Vec3(0, 0, 0);
            if (dyn) {
                it.im = 1.0f / rb.mass;
                if (rb.use_gravity) it.v += gravity * (rb.gravity_scale * dt);
                it.v = it.v * (1.0f / (1.0f + rb.linear_damping * dt));
                it.w = it.w * (1.0f / (1.0f + rb.angular_damping * dt));
                for (int f = 0; f < nfields; f++) {
                    if (!(fields[f].f.layers & it.layer)) continue;
                    it.v += field_force(fields[f], it.com, (float)time_) * (it.im * dt);
                }
                if (it.rotates) {
                    float im3[3] = { 1.0f / (inertia.x * rb.mass), 1.0f / (inertia.y * rb.mass), 1.0f / (inertia.z * rb.mass) };
                    for (int a = 0; a < 3; a++)
                        for (int r = 0; r < 3; r++) {
                            float* vr = &it.ii[r * 3];
                            const float av[3] = { it.ax[a].x, it.ax[a].y, it.ax[a].z };
                            for (int c = 0; c < 3; c++) vr[c] += im3[a] * av[r] * av[c];
                        }
                }
            }
        }
        ns++;
    }

    // ---- joints: the active ones, their bodies and anchors
    JointRow* jr = nullptr;
    int njr = 0;
    uint64_t* jskip = nullptr;          // pairs that must not collide: (smaller, larger) entity
    int njskip = 0, jskip_cap = 0;
    for (int i = 0; i < joint_count_;) {
        if (!world_->alive(joints_[i].e) || !world_->has(joints_[i].e, ModuleKind::Joint3D)) { joints_[i].~JointState(); if (i != joint_count_ - 1) memcpy((void*)&joints_[i], (void*)&joints_[joint_count_ - 1], sizeof(JointState)); joint_count_--; }
        else i++;
    }
    for (int i = 0; i < count_; i++) {
        Entity je = bodies_[i].e;
        if (!world_->has(je, ModuleKind::Joint3D)) continue;
        bool known = false;
        for (int k = 0; k < joint_count_; k++) if (joints_[k].e == je) known = true;
        if (known) continue;
        if (joint_count_ == joint_cap_) {
            int nc = joint_cap_ ? joint_cap_ * 2 : 16;
            JointState* nj = (JointState*)malloc(sizeof(JointState) * nc);
            if (!nj) break;
            for (int k = 0; k < joint_count_; k++) { new (&nj[k]) JointState(joints_[k]); joints_[k].~JointState(); }
            free(joints_);
            joints_ = nj;
            joint_cap_ = nc;
        }
        JointState* st = new (&joints_[joint_count_++]) JointState();
        st->e = je;
    }
    if (joint_count_ > 0) {
        jr = (JointRow*)malloc(sizeof(JointRow) * joint_count_);
        for (int k = 0; k < joint_count_ && jr; k++) {
            JointState& st = joints_[k];
            Joint3D* jm = (Joint3D*)world_->get_mut(st.e, ModuleKind::Joint3D);
            if (!jm) continue;
            if (jm->broken) { jm->force = 0; continue; }
            Item* A = nullptr;
            for (int q = 0; q < ns; q++) if (items[q].e == st.e) A = &items[q];
            if (!A || !A->rb) continue;
            Item* B = nullptr;
            if (jm->connected) {
                Entity be = world_->link_entity(jm->connected);
                if (!be) { jm->broken = true; continue; }        // the other body is gone
                for (int q = 0; q < ns; q++) if (items[q].e == be) B = &items[q];
                if (!B) continue;
            }
            bool a_moves = A->im > 0 || A->rotates, b_moves = B && (B->im > 0 || B->rotates);
            if (!a_moves && !b_moves) continue;
            JointRow* r = new (&jr[njr]) JointRow();
            r->js = &st;
            r->m = jm;
            r->A = A;
            if (B) r->B = B;
            else {
                r->B = &r->dummy;
                r->dummy.ax[0] = Vec3(1, 0, 0); r->dummy.ax[1] = Vec3(0, 1, 0); r->dummy.ax[2] = Vec3(0, 0, 1);
                r->dummy.mat = Mat4::identity();
            }
            Item& Bi = *r->B;
            if (!st.init) {
                Vec3 wa = transform_point(A->mat, jm->anchor);
                // A spring hangs from the anchor (on the other body or in the world) to this object's origin.
                st.la = jm->type == JointType::Spring ? Vec3(0, 0, 0) : jm->anchor;
                Mat4 inv;
                st.lb = B && invert(B->mat, &inv) ? transform_point(inv, wa) : wa;
                Vec3 axl = normalize(jm->axis);
                if (length(axl) == 0) axl = Vec3(0, 1, 0);
                st.axl = axl;
                Vec3 a0 = rot_of(*A, axl);
                st.axb = unrot_of(Bi, a0);
                Vec3 h = fabsf_(axl.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
                st.pl = normalize(cross(axl, h));
                st.pb = unrot_of(Bi, rot_of(*A, st.pl));
                for (int c = 0; c < 3; c++) st.rel[c] = unrot_of(*A, Bi.ax[c]);
                st.init = true;
            }
            r->pa = transform_point(A->mat, st.la);
            r->pb = B ? transform_point(B->mat, st.lb) : st.lb;
            if (!B) r->dummy.com = r->pb;
            r->rA = r->pa - A->com;
            r->rB = r->pb - Bi.com;
            r->fixed = jm->type == JointType::Fixed;
            r->hinge = jm->type == JointType::Hinge;
            r->spring = jm->type == JointType::Spring;
            r->lin = !r->spring;
            njr++;
            if (!jm->collide_connected && B) {
                Entity lo = A->e < B->e ? A->e : B->e, hi = A->e < B->e ? B->e : A->e;
                pair_add(&jskip, &njskip, &jskip_cap, lo, hi);
            }
        }
    }
    stats_.joints = njr;

    // Terrains: static solver bodies after the others (not in the 0..ns loops).
    for (int t = 0; t < terrain_count_; t++) {
        Item* tp = new (&items[ns + t]) Item();
        tp->e = terrains_[t].entity;
        tp->ax[0] = Vec3(1, 0, 0); tp->ax[1] = Vec3(0, 1, 0); tp->ax[2] = Vec3(0, 0, 1);
        tp->com = terrains_[t].origin;
        tp->mat = Mat4::translation(terrains_[t].origin);
        tp->friction = terrains_[t].friction;
        tp->bounce = terrains_[t].bounciness;
        tp->layer = terrains_[t].layer;
    }

    // ---- broadphase: sweep and prune on x
    for (int i = 0; i < ns; i++) { order[i] = i; keys[i] = items[i].shape ? items[i].cv.lo.x : 1e30f; }
    shell_sort(order, keys, ns);

    // The last step's contacts, by pair, for warm starting and first contacts.
    int tcap = 16;
    while (tcap < manifold_count_ * 2) tcap *= 2;
    int* table = (int*)malloc(sizeof(int) * tcap);
    if (table) {
        for (int i = 0; i < tcap; i++) table[i] = -1;
        for (int i = 0; i < manifold_count_; i++) {
            uint64_t h = pair_hash(manifolds_[i].ea, manifolds_[i].eb) & (uint64_t)(tcap - 1);
            while (table[h] >= 0) h = (h + 1) & (uint64_t)(tcap - 1);
            table[h] = i;
        }
    }
    auto find_prev = [&](Entity a, Entity b) -> const Manifold* {
        if (!table) return nullptr;
        uint64_t h = pair_hash(a, b) & (uint64_t)(tcap - 1);
        while (table[h] >= 0) {
            const Manifold& mm = manifolds_[table[h]];
            if (mm.ea == a && mm.eb == b) return &mm;
            h = (h + 1) & (uint64_t)(tcap - 1);
        }
        return nullptr;
    };

    uint64_t* new_overlaps = nullptr;
    int new_overlap_count = 0, new_overlap_cap = 0;
    Manifold* cur = nullptr;
    int cur_count = 0, cur_cap = 0;
    stats_.contacts = 0;
    stats_.terrain_contacts = 0;
    // Entities whose first contact is reported (collected, sent after the step).
    uint64_t* hits = nullptr;
    int hit_count = 0, hit_cap = 0;

    for (int oi = 0; oi < ns; oi++) {
        Item& a = items[order[oi]];
        if (!a.shape) continue;
        for (int oj = oi + 1; oj < ns; oj++) {
            Item& b = items[order[oj]];
            if (!b.shape) continue;
            if (b.cv.lo.x > a.cv.hi.x + kMargin) break;
            if (!(a.layer & b.layer)) continue;
            if (njskip && pair_has(jskip, njskip, a.e < b.e ? a.e : b.e, a.e < b.e ? b.e : a.e)) continue;
            bool trig = a.trigger || b.trigger;
            if (!trig && a.im == 0 && b.im == 0 && !a.dynamic && !b.dynamic) {
                // Nothing here can be pushed (static / kinematic / no body).
                continue;
            }
            if (trig && !(a.rb || a.controller) && !(b.rb || b.controller)) continue;
            if (a.cv.lo.y > b.cv.hi.y + kMargin || b.cv.lo.y > a.cv.hi.y + kMargin) continue;
            if (a.cv.lo.z > b.cv.hi.z + kMargin || b.cv.lo.z > a.cv.hi.z + kMargin) continue;
            if (trig) {
                if (convex_overlap(a.cv, b.cv)) {
                    if (a.trigger) pair_add(&new_overlaps, &new_overlap_count, &new_overlap_cap, a.e, b.e);
                    if (b.trigger) pair_add(&new_overlaps, &new_overlap_count, &new_overlap_cap, b.e, a.e);
                }
                continue;
            }
            Item* pa = &a;
            Item* pb = &b;
            if (pa->e > pb->e) { Item* t = pa; pa = pb; pb = t; }
            ContactManifold cm;
            if (!convex_collide(pa->cv, pb->cv, kMargin, &cm)) continue;
            if (cur_count == cur_cap) {
                int nc = cur_cap ? cur_cap * 2 : 128;
                Manifold* n = (Manifold*)realloc(cur, sizeof(Manifold) * nc);
                if (!n) continue;
                cur = n;
                cur_cap = nc;
            }
            Manifold* mf = new (&cur[cur_count++]) Manifold();
            mf->ea = pa->e; mf->eb = pb->e;
            mf->ia = (int)(pa - items); mf->ib = (int)(pb - items);
            mf->n = cm.n;
            mf->count = cm.count;
            mf->mu = fsqrt(pa->friction * pb->friction);
            mf->e = maxf(pa->bounce, pb->bounce);
            const Manifold* prev = find_prev(mf->ea, mf->eb);
            float minsep = 1e30f;
            for (int k = 0; k < cm.count; k++) {
                ManifoldPt& pt = mf->p[k];
                pt.pos = cm.p[k].pos;
                pt.sep = cm.p[k].sep;
                if (pt.sep < minsep) minsep = pt.sep;
                Vec3 d = pt.pos - pa->com;
                pt.la = Vec3(dot(d, pa->ax[0]), dot(d, pa->ax[1]), dot(d, pa->ax[2]));
                if (prev) {
                    float bd = kMatchDist * kMatchDist;
                    for (int q = 0; q < prev->count; q++) {
                        Vec3 e = prev->p[q].la - pt.la;
                        float dd = dot(e, e);
                        if (dd < bd) { bd = dd; pt.ln = prev->p[q].ln; pt.lt = prev->p[q].lt; }
                    }
                }
            }
            bool was = prev && prev->touching;
            mf->touching = minsep < (was ? kTouchKeep : kTouch);
            if (mf->touching) {
                stats_.contacts++;
                if (!was) pair_add(&hits, &hit_count, &hit_cap, mf->ea, mf->eb);
                if (pa->controller && -mf->n.y >= fcos(radians(pa->controller->slope_limit)))
                    pa->controller->grounded = true;
                if (pb->controller && mf->n.y >= fcos(radians(pb->controller->slope_limit)))
                    pb->controller->grounded = true;
            }
        }
    }
    // ---- terrain contacts: the lowest points of each moving shape against the heightfields
    for (int ti = 0; ti < terrain_count_; ti++) {
        const PhysicsTerrain& T = terrains_[ti];
        if (!T.height) continue;
        const int ib = ns + ti;
        for (int i = 0; i < ns; i++) {
            Item& a = items[i];
            if (!a.shape || a.trigger || !(a.layer & T.layer)) continue;
            if (!a.dynamic && !a.controller) continue;          // only what falls or walks rests on the ground
            Vec3 lo = a.cv.lo - T.origin, hi = a.cv.hi - T.origin;
            if (hi.x < 0 || hi.z < 0 || lo.x > T.size_x || lo.z > T.size_z) continue;
            // The ground under the shape's centre gives the direction of its lowest points.
            bool cin = false;
            Vec3 cn(0, 1, 0);
            Vec3 cl = a.cv.c - T.origin;
            T.height(T.user, cl.x, cl.z, &cin, &cn);
            if (!(cn.y > 0.01f)) cn = Vec3(0, 1, 0);
            Vec3 pts[264];
            int np = 0;
            const Convex& cv = a.cv;
            switch (cv.kind) {
            case ShapeKind::Sphere: pts[np++] = cv.c - cn * cv.r; break;
            case ShapeKind::Capsule: {
                Vec3 e = cv.ax[1] * cv.he.y;
                pts[np++] = cv.c + e - cn * cv.r;
                pts[np++] = cv.c - e - cn * cv.r;
                pts[np++] = cv.c - cn * cv.r;
                break;
            }
            case ShapeKind::Box:
                for (int k = 0; k < 8; k++)
                    pts[np++] = cv.c + cv.ax[0] * ((k & 1) ? cv.he.x : -cv.he.x) + cv.ax[1] * ((k & 2) ? cv.he.y : -cv.he.y) +
                                cv.ax[2] * ((k & 4) ? cv.he.z : -cv.he.z);
                break;
            case ShapeKind::Cylinder:
                for (int k = 0; k < 16; k++) {
                    float ang = (float)(k % 8) * (kPi / 4.0f);
                    pts[np++] = cv.c + cv.ax[1] * (k < 8 ? -cv.he.y : cv.he.y) + (cv.ax[0] * fcos(ang) + cv.ax[2] * fsin(ang)) * cv.he.x;
                }
                break;
            case ShapeKind::Hull:
                if (cv.hull) {
                    int step = cv.hull->nv > 256 ? (cv.hull->nv + 255) / 256 : 1;
                    for (int k = 0; k < cv.hull->nv && np < 256; k += step) {
                        const Vec3& v = cv.hull->v[k];
                        pts[np++] = cv.c + cv.ax[0] * (v.x * cv.scale.x) + cv.ax[1] * (v.y * cv.scale.y) + cv.ax[2] * (v.z * cv.scale.z);
                    }
                }
                break;
            default: break;
            }
            // Points below (or just above) the ground there.
            struct Hit { Vec3 pos; Vec3 n; float sep; };
            Hit hit[264];
            int nh = 0;
            for (int k = 0; k < np; k++) {
                Vec3 lp = pts[k] - T.origin;
                bool inside = false;
                Vec3 n(0, 1, 0);
                float h = T.height(T.user, lp.x, lp.z, &inside, &n);
                if (!inside) continue;
                float sep = (lp.y - h) * n.y;
                if (sep > kMargin) continue;
                hit[nh].pos = pts[k] - n * (sep * 0.5f);
                hit[nh].n = n;
                hit[nh].sep = sep;
                nh++;
            }
            if (!nh) continue;
            // Up to four: the deepest, then each time the point furthest from those taken.
            int pick[4];
            int npk = 0;
            int deepest = 0;
            for (int k = 1; k < nh; k++) if (hit[k].sep < hit[deepest].sep) deepest = k;
            pick[npk++] = deepest;
            while (npk < 4 && npk < nh) {
                int best = -1;
                float bd = -1.0f;
                for (int k = 0; k < nh; k++) {
                    float dmin = 1e30f;
                    for (int q = 0; q < npk; q++) {
                        Vec3 d = hit[k].pos - hit[pick[q]].pos;
                        float dd = dot(d, d);
                        if (dd < dmin) dmin = dd;
                    }
                    if (dmin > bd) { bd = dmin; best = k; }
                }
                if (best < 0 || bd < 1e-6f) break;
                pick[npk++] = best;
            }
            Vec3 gn(0, 0, 0);
            for (int q = 0; q < npk; q++) gn += hit[pick[q]].n;
            gn = normalize(gn);
            if (!(gn.y > 0.0f)) gn = Vec3(0, 1, 0);
            if (cur_count == cur_cap) {
                int nc = cur_cap ? cur_cap * 2 : 128;
                Manifold* nm = (Manifold*)realloc(cur, sizeof(Manifold) * nc);
                if (!nm) continue;
                cur = nm;
                cur_cap = nc;
            }
            Manifold* mf = new (&cur[cur_count++]) Manifold();
            mf->ea = a.e;
            mf->eb = T.entity ? T.entity : (Entity)(0xFFFFFFFF00000000ull | (uint64_t)ti);
            mf->ia = i;
            mf->ib = ib;
            mf->n = gn * -1.0f;                                 // from the body (A) into the ground (B)
            mf->count = npk;
            mf->mu = fsqrt(a.friction * T.friction);
            mf->e = maxf(a.bounce, T.bounciness);
            const Manifold* prev = find_prev(mf->ea, mf->eb);
            float minsep = 1e30f;
            for (int q = 0; q < npk; q++) {
                ManifoldPt& pt = mf->p[q];
                pt.pos = hit[pick[q]].pos;
                pt.sep = hit[pick[q]].sep;
                if (pt.sep < minsep) minsep = pt.sep;
                Vec3 d = pt.pos - a.com;
                pt.la = Vec3(dot(d, a.ax[0]), dot(d, a.ax[1]), dot(d, a.ax[2]));
                if (prev) {
                    float bdist = kMatchDist * kMatchDist;
                    for (int r = 0; r < prev->count; r++) {
                        Vec3 e = prev->p[r].la - pt.la;
                        float dd = dot(e, e);
                        if (dd < bdist) { bdist = dd; pt.ln = prev->p[r].ln; pt.lt = prev->p[r].lt; }
                    }
                }
            }
            bool was = prev && prev->touching;
            mf->touching = minsep < (was ? kTouchKeep : kTouch);
            if (mf->touching) {
                stats_.contacts++;
                stats_.terrain_contacts++;
                if (!was && T.entity) pair_add(&hits, &hit_count, &hit_cap, mf->ea, mf->eb);
                if (a.controller && gn.y >= fcos(radians(a.controller->slope_limit))) a.controller->grounded = true;
            }
        }
    }
    free(table);

    // ---- solver setup
    for (int mi = 0; mi < cur_count; mi++) {
        Manifold& mf = cur[mi];
        Item& A = items[mf.ia];
        Item& B = items[mf.ib];
        const Vec3& n = mf.n;
        Vec3 h = absf(n.x) > 0.6f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
        mf.t1 = normalize(cross(n, h));
        mf.t2 = cross(n, mf.t1);
        for (int k = 0; k < mf.count; k++) {
            ManifoldPt& p = mf.p[k];
            p.rA = p.pos - A.com;
            p.rB = p.pos - B.com;
            p.mn = eff_mass(A, B, p.rA, p.rB, n);
            p.mt1 = eff_mass(A, B, p.rA, p.rB, mf.t1);
            p.mt2 = eff_mass(A, B, p.rA, p.rB, mf.t2);
            float vn0 = dot(rel_vel(A, B, p.rA, p.rB), n);
            p.target = p.sep > 0 ? -p.sep / dt : 0.0f;
            if (mf.e > 0 && vn0 < -kRestThreshold && (p.sep <= 0 || -vn0 * dt >= p.sep)) p.target = maxf(p.target, -mf.e * vn0);
            p.l1 = dot(p.lt, mf.t1);
            p.l2 = dot(p.lt, mf.t2);
            p.lp = 0;
            p.bias = p.sep < -kSlop ? (-p.sep - kSlop) * kBeta / dt : 0.0f;
            if (p.bias > kMaxPush) p.bias = kMaxPush;
            // Warm start.
            apply(A, B, p.rA, p.rB, n * p.ln + mf.t1 * p.l1 + mf.t2 * p.l2);
        }
    }

    // ---- joint setup: masses, anchors, warm start
    for (int k = 0; k < njr; k++) {
        JointRow& r = jr[k];
        JointState& st = *r.js;
        Item& A = *r.A;
        Item& B = *r.B;
        const Joint3D& jm = *r.m;
        static const Vec3 W[3] = { Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1) };
        if (r.lin) {
            for (int c = 0; c < 3; c++) r.mlin[c] = eff_mass(A, B, r.rA, r.rB, W[c]);
            apply(A, B, r.rA, r.rB, st.acc_lin);
        }
        if (r.fixed) {
            // B should keep its orientation relative to A: the rotation it needs.
            Vec3 des[3], cur[3] = { B.ax[0], B.ax[1], B.ax[2] };
            for (int c = 0; c < 3; c++) des[c] = rot_of(A, st.rel[c]);
            float E[3][3] = {};
            for (int c = 0; c < 3; c++) {
                const float dv[3] = { des[c].x, des[c].y, des[c].z }, cv[3] = { cur[c].x, cur[c].y, cur[c].z };
                for (int i2 = 0; i2 < 3; i2++) for (int j2 = 0; j2 < 3; j2++) E[i2][j2] += dv[i2] * cv[j2];
            }
            r.e_ang = Vec3(0.5f * (E[2][1] - E[1][2]), 0.5f * (E[0][2] - E[2][0]), 0.5f * (E[1][0] - E[0][1]));
            for (int c = 0; c < 3; c++) r.mang[c] = ang_mass(A, B, W[c]);
            apply_ang(A, B, st.acc_ang);
        } else if (r.hinge) {
            // The rows turn about -axis, so that positive angle, speed and limits mean
            // this body turning positively about its axis relative to the connected one.
            Vec3 a_user = normalize(rot_of(A, st.axl));
            r.axis = a_user * -1.0f;
            Vec3 b = rot_of(B, st.axb);
            r.e_ang = cross(b, a_user);
            Vec3 h = fabsf_(r.axis.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
            r.t1 = normalize(cross(r.axis, h));
            r.t2 = cross(r.axis, r.t1);
            r.mang[0] = ang_mass(A, B, r.t1);
            r.mang[1] = ang_mass(A, B, r.t2);
            r.m_axis = ang_mass(A, B, r.axis);
            Vec3 u = rot_of(A, st.pl), v = rot_of(B, st.pb);
            r.theta = fatan2(dot(r.axis, cross(u, v)), dot(u, v));
            apply_ang(A, B, r.t1 * st.acc_ang.x + r.t2 * st.acc_ang.y + r.axis * (st.acc_limit + st.acc_limit2 + st.acc_motor));
        } else if (r.spring) {
            Vec3 d = r.pb - r.pa;
            r.len = length(d);
            r.n = r.len > 1e-5f ? d * (1.0f / r.len) : Vec3(0, 0, 0);
            float k_ = jm.stiffness, c_ = jm.damping;
            float den = dt * (c_ + dt * k_);
            if (r.len > 1e-5f && den > 1e-9f) {
                r.gamma = 1.0f / den;
                r.beta_s = dt * k_ * r.gamma;
                float m = eff_mass(A, B, r.rA, r.rB, r.n);
                r.spring_m = m > 0 ? 1.0f / (1.0f / m + r.gamma) : 0.0f;
                apply(A, B, r.rA, r.rB, r.n * st.acc_spring);
            } else {
                r.spring_m = 0;
            }
        }
    }

    // ---- velocity iterations
    for (int it = 0; it < velocity_iterations; it++) {
        // Joints first, then the contacts.
        for (int k = 0; k < njr; k++) {
            JointRow& r = jr[k];
            JointState& st = *r.js;
            Item& A = *r.A;
            Item& B = *r.B;
            const Joint3D& jm = *r.m;
            static const Vec3 W[3] = { Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1) };
            if (r.hinge) {
                if (jm.use_motor && r.m_axis > 0) {
                    float cdot = dot(r.axis, B.w - A.w);
                    float lam = (radians(jm.motor_speed) - cdot) * r.m_axis;
                    float maxi = jm.motor_torque * dt;
                    float nl = clampf(st.acc_motor + lam, -maxi, maxi);
                    lam = nl - st.acc_motor;
                    st.acc_motor = nl;
                    apply_ang(A, B, r.axis * lam);
                } else {
                    st.acc_motor = 0;
                }
                if (jm.use_limits && r.m_axis > 0) {
                    float lo = jm.limit_min < jm.limit_max ? jm.limit_min : jm.limit_max, hi = jm.limit_min < jm.limit_max ? jm.limit_max : jm.limit_min;
                    float tlo = radians(lo), thi = radians(hi);
                    float cdot = dot(r.axis, B.w - A.w);
                    // upper limit: Cdot may not exceed (thi - theta) / dt; lower: not fall below (tlo - theta) / dt
                    float up = (thi - r.theta) / dt, down = (tlo - r.theta) / dt;
                    if (r.theta > thi) up = -clampf((r.theta - thi) * kJointBeta / dt, 0, kJointMaxPush);
                    if (r.theta < tlo) down = clampf((tlo - r.theta) * kJointBeta / dt, 0, kJointMaxPush);
                    float lam_u = (up - cdot) * r.m_axis;
                    float nu = st.acc_limit + lam_u;
                    if (nu > 0) nu = 0;
                    float du = nu - st.acc_limit;
                    st.acc_limit = nu;
                    apply_ang(A, B, r.axis * du);
                    cdot = dot(r.axis, B.w - A.w);
                    float lam_d = (down - cdot) * r.m_axis;
                    float nd = st.acc_limit2 + lam_d;
                    if (nd < 0) nd = 0;
                    float dd = nd - st.acc_limit2;
                    st.acc_limit2 = nd;
                    apply_ang(A, B, r.axis * dd);
                } else {
                    st.acc_limit = st.acc_limit2 = 0;
                }
                const Vec3 ts[2] = { r.t1, r.t2 };
                float* acc[2] = { &st.acc_ang.x, &st.acc_ang.y };
                for (int c = 0; c < 2; c++) {
                    float cdot = dot(ts[c], B.w - A.w);
                    float bias = clampf(kJointBeta * dot(r.e_ang, ts[c]) / dt, -kJointMaxPush, kJointMaxPush);
                    float lam = (bias - cdot) * r.mang[c];
                    *acc[c] += lam;
                    apply_ang(A, B, ts[c] * lam);
                }
            } else if (r.fixed) {
                float* acc[3] = { &st.acc_ang.x, &st.acc_ang.y, &st.acc_ang.z };
                const float ev[3] = { r.e_ang.x, r.e_ang.y, r.e_ang.z };
                for (int c = 0; c < 3; c++) {
                    float cdot = dot(W[c], B.w - A.w);
                    float bias = clampf(kJointBeta * ev[c] / dt, -kJointMaxPush, kJointMaxPush);
                    float lam = (bias - cdot) * r.mang[c];
                    *acc[c] += lam;
                    apply_ang(A, B, W[c] * lam);
                }
            }
            if (r.lin) {
                float* acc[3] = { &st.acc_lin.x, &st.acc_lin.y, &st.acc_lin.z };
                const Vec3 err = r.pb - r.pa;
                const float ev[3] = { err.x, err.y, err.z };
                for (int c = 0; c < 3; c++) {
                    float cdot = dot(rel_vel(A, B, r.rA, r.rB), W[c]);
                    float bias = clampf(kJointBeta * ev[c] / dt, -kJointMaxPush, kJointMaxPush);
                    // pb - pa = ev: B's anchor is ahead by ev; a negative relative velocity fixes it.
                    float lam = -(cdot + bias) * r.mlin[c];
                    *acc[c] += lam;
                    apply(A, B, r.rA, r.rB, W[c] * lam);
                }
            } else if (r.spring && r.spring_m > 0) {
                float cdot = dot(rel_vel(A, B, r.rA, r.rB), r.n);
                float C = r.len - jm.rest_length;
                float lam = -r.spring_m * (cdot + r.beta_s * C + r.gamma * st.acc_spring);
                st.acc_spring += lam;
                apply(A, B, r.rA, r.rB, r.n * lam);
            }
        }
        for (int q = 0; q < cur_count; q++) {
            Manifold& mf = cur[(it & 1) ? cur_count - 1 - q : q];
            Item& A = items[mf.ia];
            Item& B = items[mf.ib];
            for (int k = 0; k < mf.count; k++) {
                ManifoldPt& p = mf.p[k];
                // Friction (within the cone of the normal impulse so far).
                Vec3 rel = rel_vel(A, B, p.rA, p.rB);
                float n1 = p.l1 - dot(rel, mf.t1) * p.mt1;
                float n2 = p.l2 - dot(rel, mf.t2) * p.mt2;
                float lim = mf.mu * p.ln, len = fsqrt(n1 * n1 + n2 * n2);
                if (len > lim) { float s = len > 0 ? lim / len : 0.0f; n1 *= s; n2 *= s; }
                float d1 = n1 - p.l1, d2 = n2 - p.l2;
                p.l1 = n1; p.l2 = n2;
                apply(A, B, p.rA, p.rB, mf.t1 * d1 + mf.t2 * d2);
                // Normal.
                rel = rel_vel(A, B, p.rA, p.rB);
                float vn = dot(rel, mf.n);
                float nl = p.ln + (p.target - vn) * p.mn;
                if (nl < 0) nl = 0;
                float dl = nl - p.ln;
                p.ln = nl;
                apply(A, B, p.rA, p.rB, mf.n * dl);
            }
        }
    }
    for (int mi = 0; mi < cur_count; mi++)
        for (int k = 0; k < cur[mi].count; k++) {
            ManifoldPt& p = cur[mi].p[k];
            p.lt = cur[mi].t1 * p.l1 + cur[mi].t2 * p.l2;
        }

    // ---- position correction by split impulses (no energy added)
    for (int it = 0; it < position_iterations; it++) {
        for (int q = 0; q < cur_count; q++) {
            Manifold& mf = cur[q];
            Item& A = items[mf.ia];
            Item& B = items[mf.ib];
            for (int k = 0; k < mf.count; k++) {
                ManifoldPt& p = mf.p[k];
                if (p.bias <= 0) continue;
                Vec3 rel = (B.vp + cross(B.wp, p.rB)) - (A.vp + cross(A.wp, p.rA));
                float nl = p.lp + (p.bias - dot(rel, mf.n)) * p.mn;
                if (nl < 0) nl = 0;
                float dl = nl - p.lp;
                p.lp = nl;
                apply_pseudo(A, B, p.rA, p.rB, mf.n * dl);
            }
        }
    }

    // ---- joints: forces, angles, breaking
    for (int k = 0; k < njr; k++) {
        JointRow& r = jr[k];
        JointState& st = *r.js;
        Joint3D& jm = *r.m;
        float f = r.spring ? absf(st.acc_spring) / dt : length(st.acc_lin) / dt;
        jm.force = f;
        if (r.hinge) jm.angle = degrees(r.theta);
        if (jm.break_force > 0 && f > jm.break_force) {
            jm.broken = true;
            st.acc_lin = st.acc_ang = Vec3(0, 0, 0);
            st.acc_limit = st.acc_limit2 = st.acc_motor = st.acc_spring = 0;
        }
    }
    free(jr);
    free(jskip);

    // ---- integrate and write back
    for (int i = 0; i < ns; i++) {
        Item& s = items[i];
        if (!s.movable || (!s.rb && !s.controller)) continue;
        if (s.controller) {
            s.controller->velocity = s.v;
        } else if (s.dynamic) {
            s.rb->velocity = s.v;
            if (s.rotates) s.rb->angular_velocity = s.w;
        }
        Vec3 vt = s.v + s.vp, wt = s.w + s.wp;
        bool moved = dot(vt, vt) > 0, turned = s.rotates && dot(wt, wt) > 0;
        if (!moved && !turned) continue;
        Transform* t = world_->transform(s.e);
        if (!t) continue;
        Vec3 delta = vt * dt;
        Quat dq;
        if (turned) {
            float ang = length(wt) * dt;
            dq = Quat::axis_angle(normalize(wt), ang);
            delta += rotate(dq, s.off) - s.off;
        }
        Entity parent = world_->parent(s.e);
        Vec3 local = delta;
        const WorldTransform* pw = parent ? world_->world_transform(parent) : nullptr;
        if (pw) {
            Mat4 inv;
            if (invert(pw->matrix, &inv)) local = transform_dir(inv, delta);
        }
        t->position += local;
        if (turned) {
            if (pw) {
                Vec3 pt_, ps;
                Quat pr;
                decompose(pw->matrix, &pt_, &pr, &ps);
                dq = conjugate(pr) * dq * pr;
            }
            t->rotation = normalize(dq * t->rotation);
        }
    }

    // ---- keep the contacts for the next step; events
    free(manifolds_);
    manifolds_ = cur;
    manifold_count_ = cur_count;
    manifold_cap_ = cur_cap;
    // The warm-start data stays in the kept manifolds (ln, lt, la); anchors were set above.

    if (events_.trigger_enter)
        for (int i = 0; i < new_overlap_count; i++)
            if (!pair_has(overlaps_, overlap_count_, new_overlaps[i * 2], new_overlaps[i * 2 + 1]))
                events_.trigger_enter(new_overlaps[i * 2], new_overlaps[i * 2 + 1], events_.user);
    if (events_.trigger_exit)
        for (int i = 0; i < overlap_count_; i++)
            if (!pair_has(new_overlaps, new_overlap_count, overlaps_[i * 2], overlaps_[i * 2 + 1]))
                events_.trigger_exit(overlaps_[i * 2], overlaps_[i * 2 + 1], events_.user);
    if (events_.collision)
        for (int i = 0; i < hit_count; i++) events_.collision(hits[i * 2], hits[i * 2 + 1], events_.user);
    free(hits);
    free(overlaps_);
    overlaps_ = new_overlaps;
    overlap_count_ = new_overlap_count;
    overlap_cap_ = new_overlap_cap;
    free(items);
    free(order);
    free(keys);
    stats_.steps++;
}

}  // namespace fe
