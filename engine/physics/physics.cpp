#include "physics/physics.h"
#include "physics/convex.h"

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
        if (!world_->alive(e) || !(world_->has(e, ModuleKind::RigidBody3D) || world_->has(e, ModuleKind::Collider3D))) bodies_[i] = bodies_[--count_];
        else i++;
    }
    for (int i = 0; i < mesh_count_;) {
        if (!world_->alive(meshes_[i].e)) { hull_free(meshes_[i].hull); meshes_[i] = meshes_[--mesh_count_]; }
        else i++;
    }
    int cap = count_ ? count_ : 1;
    Item* items = (Item*)malloc(sizeof(Item) * cap);
    int* order = (int*)malloc(sizeof(int) * cap);
    float* keys = (float*)malloc(sizeof(float) * cap);
    if (!items || !order || !keys) { free(items); free(order); free(keys); return; }
    int ns = 0;
    stats_.bodies = stats_.colliders = 0;

    // ---- gather: poses, shapes, mass properties, velocities with gravity and damping
    for (int i = 0; i < count_; i++) {
        Entity e = bodies_[i].e;
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
        it.rb = world_->has(e, ModuleKind::RigidBody3D) ? (RigidBody3D*)world_->get_mut(e, ModuleKind::RigidBody3D) : nullptr;
        if (it.rb) stats_.bodies++;
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
        if (col) {
            stats_.colliders++;
            Convex& cv = it.cv;
            for (int k = 0; k < 3; k++) cv.ax[k] = it.ax[k];
            it.trigger = col->mode == ColliderMode::Trigger;
            it.friction = col->friction;
            it.bounce = col->bounciness;
            it.layer = col->layer;
            float rxz = maxf(lx, lz);
            ShapeKind kind = ShapeKind::Box;
            const Hull* hull = nullptr;
            if (col->shape == ColliderShape::Mesh) {
                for (int k = 0; k < mesh_count_; k++) if (meshes_[k].e == e) hull = meshes_[k].hull;
                if (hull) kind = ShapeKind::Hull;
            }
            Vec3 center = col->center;
            switch (col->shape == ColliderShape::Mesh && !hull ? ColliderShape::Box : col->shape) {
            case ColliderShape::Sphere: kind = ShapeKind::Sphere; break;
            case ColliderShape::Capsule: kind = ShapeKind::Capsule; break;
            case ColliderShape::Cylinder: kind = ShapeKind::Cylinder; break;
            case ColliderShape::Mesh: kind = ShapeKind::Hull; break;
            default: kind = ShapeKind::Box; break;
            }
            if (kind == ShapeKind::Hull) center = hull->center;
            it.com = transform_point(m, center);
            cv.c = it.com;
            cv.kind = kind;
            if (kind == ShapeKind::Sphere) {
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
        if (it.rb) {
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
            bool trig = a.trigger || b.trigger;
            if (!trig && a.im == 0 && b.im == 0 && !a.dynamic && !b.dynamic) {
                // Nothing here can be pushed (static / kinematic / no body).
                continue;
            }
            if (trig && !a.rb && !b.rb) continue;
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

    // ---- velocity iterations
    for (int it = 0; it < velocity_iterations; it++) {
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

    // ---- integrate and write back
    for (int i = 0; i < ns; i++) {
        Item& s = items[i];
        if (!s.movable || !s.rb) continue;
        if (s.dynamic) {
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
