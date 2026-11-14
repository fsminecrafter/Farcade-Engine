#include "scene.h"

#include <stdlib.h>
#include <string.h>

namespace fe {

Scene::Scene() {}

Scene::~Scene() {
    clear();
    free(objects_);
    free(selection_);
}

void Scene::copy_from(const Scene& other) {
    if (&other == this) return;
    clear();
    if (cap_ < other.count_) {
        objects_ = (Object**)realloc(objects_, sizeof(Object*) * (other.count_ ? other.count_ : 1));
        cap_ = other.count_;
    }
    for (int i = 0; i < other.count_; i++) objects_[i] = new Object(*other.objects_[i]);
    count_ = other.count_;
    next_id_ = other.next_id_;
    if (sel_cap_ < other.sel_count_) {
        selection_ = (ObjectId*)realloc(selection_, sizeof(ObjectId) * other.sel_count_);
        sel_cap_ = other.sel_count_;
    }
    if (other.sel_count_) memcpy(selection_, other.selection_, sizeof(ObjectId) * other.sel_count_);
    sel_count_ = other.sel_count_;
    active_ = other.active_;
}

ObjectId Scene::create(const char* name, MeshKind mesh) {
    if (count_ == cap_) {
        int ncap = cap_ ? cap_ * 2 : 32;
        Object** n = (Object**)realloc(objects_, sizeof(Object*) * ncap);
        if (!n) return kNoObject;
        objects_ = n;
        cap_ = ncap;
    }
    Object* o = new Object();
    o->id = next_id_++;
    strncpy(o->name, name, sizeof(o->name) - 1);
    o->mesh = mesh;
    objects_[count_++] = o;
    return o->id;
}

void Scene::remove(ObjectId id) {
    // The object and everything below it: list them while the parent links
    // still exist, then delete.
    if (!get(id)) return;
    ObjectId* doomed = (ObjectId*)malloc(sizeof(ObjectId) * count_);
    if (!doomed) return;
    int n = 0;
    for (int i = 0; i < count_; i++)
        if (objects_[i]->id == id || is_ancestor(id, objects_[i]->id)) doomed[n++] = objects_[i]->id;
    for (int k = 0; k < n; k++) remove_one(doomed[k]);
    free(doomed);
}

void Scene::remove_one(ObjectId id) {
    for (int i = 0; i < count_; i++) {
        if (objects_[i]->id != id) continue;
        delete objects_[i];
        memmove(&objects_[i], &objects_[i + 1], sizeof(Object*) * (count_ - i - 1));
        count_--;
        break;
    }
    if (is_selected(id)) toggle(id);
}

Mat4 Scene::parent_world_matrix(const Object& o) {
    Object* p = o.parent ? get(o.parent) : nullptr;
    return p ? world_matrix(*p) : Mat4();
}

Mat4 Scene::world_matrix(const Object& o) {
    Mat4 m = o.matrix();
    const Object* cur = &o;
    for (int guard = 0; guard < 64 && cur->parent; guard++) {
        Object* p = get(cur->parent);
        if (!p) break;
        m = p->matrix() * m;
        cur = p;
    }
    return m;
}

bool Scene::visible_in_tree(const Object& o) {
    const Object* cur = &o;
    for (int guard = 0; guard < 64 && cur; guard++) {
        if (!cur->visible) return false;
        cur = cur->parent ? get(cur->parent) : nullptr;
    }
    return true;
}

bool Scene::is_ancestor(ObjectId ancestor, ObjectId id) {
    if (ancestor == kNoObject) return false;
    Object* cur = get(id);
    for (int guard = 0; guard < 64 && cur && cur->parent; guard++) {
        if (cur->parent == ancestor) return true;
        cur = get(cur->parent);
    }
    return false;
}

bool Scene::has_children(ObjectId id) const {
    for (int i = 0; i < count_; i++)
        if (objects_[i]->parent == id) return true;
    return false;
}

int Scene::depth(const Object& o) {
    int d = 0;
    const Object* cur = &o;
    while (d < 64 && cur->parent) {
        cur = get(cur->parent);
        if (!cur) break;
        d++;
    }
    return d;
}

bool Scene::set_parent(ObjectId id, ObjectId parent) {
    Object* o = get(id);
    if (!o) return false;
    if (parent == id || (parent != kNoObject && (!get(parent) || is_ancestor(id, parent)))) return false;
    if (o->parent == parent) return true;
    Mat4 world = world_matrix(*o);
    Mat4 pw, inv;
    if (parent != kNoObject) pw = world_matrix(*get(parent));
    if (!invert(pw, &inv)) return false;
    o->parent = parent;
    o->set_matrix(inv * world);
    return true;
}

bool Scene::has_selected_ancestor(ObjectId id) {
    Object* cur = get(id);
    for (int guard = 0; guard < 64 && cur && cur->parent; guard++) {
        if (is_selected(cur->parent)) return true;
        cur = get(cur->parent);
    }
    return false;
}

void Scene::clear() {
    for (int i = 0; i < count_; i++) delete objects_[i];
    count_ = 0;
    clear_selection();
}

Object* Scene::get(ObjectId id) {
    for (int i = 0; i < count_; i++)
        if (objects_[i]->id == id) return objects_[i];
    return nullptr;
}

bool Scene::is_selected(ObjectId id) const {
    for (int i = 0; i < sel_count_; i++)
        if (selection_[i] == id) return true;
    return false;
}

void Scene::clear_selection() {
    sel_count_ = 0;
    active_ = kNoObject;
}

void Scene::select_only(ObjectId id) {
    clear_selection();
    if (id != kNoObject) toggle(id);
}

void Scene::toggle(ObjectId id) {
    for (int i = 0; i < sel_count_; i++) {
        if (selection_[i] != id) continue;
        memmove(&selection_[i], &selection_[i + 1], sizeof(ObjectId) * (sel_count_ - i - 1));
        sel_count_--;
        if (active_ == id) active_ = sel_count_ ? selection_[sel_count_ - 1] : kNoObject;
        return;
    }
    if (sel_count_ == sel_cap_) {
        int ncap = sel_cap_ ? sel_cap_ * 2 : 16;
        ObjectId* n = (ObjectId*)realloc(selection_, sizeof(ObjectId) * ncap);
        if (!n) return;
        selection_ = n;
        sel_cap_ = ncap;
    }
    selection_[sel_count_++] = id;
    active_ = id;
}

// ---- built-in meshes

namespace {

struct Builder {
    Vec3* pos = nullptr;
    Vec3* nrm = nullptr;
    uint32_t n = 0, cap = 0;
    void tri(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& na, const Vec3& nb, const Vec3& nc) {
        if (n + 3 > cap) {
            cap = cap ? cap * 2 : 256;
            pos = (Vec3*)realloc(pos, sizeof(Vec3) * cap);
            nrm = (Vec3*)realloc(nrm, sizeof(Vec3) * cap);
        }
        pos[n] = a; nrm[n++] = na;
        pos[n] = b; nrm[n++] = nb;
        pos[n] = c; nrm[n++] = nc;
    }
    void quad(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, const Vec3& normal) {
        tri(a, b, c, normal, normal, normal);
        tri(a, c, d, normal, normal, normal);
    }
    MeshData finish() {
        MeshData m;
        m.positions = pos;
        m.normals = nrm;
        m.vertex_count = n;
        Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        for (uint32_t i = 0; i < n; i++) { lo = vmin(lo, pos[i]); hi = vmax(hi, pos[i]); }
        m.bounds_min = lo;
        m.bounds_max = hi;
        return m;
    }
};

MeshData make_cube() {
    Builder b;
    const float h = 0.5f;
    Vec3 p[8] = { { -h, -h, -h }, { h, -h, -h }, { h, h, -h }, { -h, h, -h },
                  { -h, -h, h },  { h, -h, h },  { h, h, h },  { -h, h, h } };
    b.quad(p[4], p[5], p[6], p[7], { 0, 0, 1 });    // +z
    b.quad(p[1], p[0], p[3], p[2], { 0, 0, -1 });   // -z
    b.quad(p[5], p[1], p[2], p[6], { 1, 0, 0 });    // +x
    b.quad(p[0], p[4], p[7], p[3], { -1, 0, 0 });   // -x
    b.quad(p[7], p[6], p[2], p[3], { 0, 1, 0 });    // +y
    b.quad(p[0], p[1], p[5], p[4], { 0, -1, 0 });   // -y
    return b.finish();
}

MeshData make_plane() {
    Builder b;
    b.quad({ -0.5f, 0, 0.5f }, { 0.5f, 0, 0.5f }, { 0.5f, 0, -0.5f }, { -0.5f, 0, -0.5f }, { 0, 1, 0 });
    return b.finish();
}

MeshData make_sphere() {
    Builder b;
    const int rings = 12, segs = 20;
    auto pt = [&](int r, int s) {
        float th = kPi * r / rings, ph = 2 * kPi * s / segs;
        return Vec3(fsin(th) * fcos(ph) * 0.5f, fcos(th) * 0.5f, fsin(th) * fsin(ph) * 0.5f);
    };
    for (int r = 0; r < rings; r++)
        for (int s = 0; s < segs; s++) {
            Vec3 a = pt(r, s), c = pt(r + 1, s), d = pt(r + 1, s + 1), e = pt(r, s + 1);
            if (r > 0) b.tri(a, e, d, normalize(a), normalize(e), normalize(d));
            if (r < rings - 1) b.tri(a, d, c, normalize(a), normalize(d), normalize(c));
        }
    return b.finish();
}

MeshData make_cylinder() {
    Builder b;
    const int segs = 20;
    for (int s = 0; s < segs; s++) {
        float a0 = 2 * kPi * s / segs, a1 = 2 * kPi * (s + 1) / segs;
        Vec3 p0(fcos(a0) * 0.5f, 0, fsin(a0) * 0.5f), p1(fcos(a1) * 0.5f, 0, fsin(a1) * 0.5f);
        Vec3 lo0 = p0 + Vec3(0, -0.5f, 0), lo1 = p1 + Vec3(0, -0.5f, 0);
        Vec3 hi0 = p0 + Vec3(0, 0.5f, 0), hi1 = p1 + Vec3(0, 0.5f, 0);
        Vec3 n0 = normalize(p0), n1 = normalize(p1);
        b.tri(lo0, hi1, lo1, n0, n1, n1);
        b.tri(lo0, hi0, hi1, n0, n0, n1);
        b.tri(Vec3(0, 0.5f, 0), hi1, hi0, { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 });
        b.tri(Vec3(0, -0.5f, 0), lo0, lo1, { 0, -1, 0 }, { 0, -1, 0 }, { 0, -1, 0 });
    }
    return b.finish();
}

}  // namespace

const MeshData& Scene::mesh_for(const Object& o) {
    static MeshData cube = make_cube(), plane = make_plane(), sphere = make_sphere(), cylinder = make_cylinder();
    switch (o.mesh) {
        case MeshKind::Plane: return plane;
        case MeshKind::Sphere: return sphere;
        case MeshKind::Cylinder: return cylinder;
        default: return cube;   // models show as a cube until .mof loading lands
    }
}

ObjectId Scene::pick(const Ray& ray, float* distance) {
    ObjectId best = kNoObject;
    float best_t = 1e30f;
    for (int i = 0; i < count_; i++) {
        Object& o = *objects_[i];
        if (!visible_in_tree(o)) continue;
        Mat4 m = world_matrix(o), inv;
        if (!invert(m, &inv)) continue;
        // Into object space; t stays comparable because the direction is
        // transformed without normalizing.
        Ray local = { transform_point(inv, ray.origin), transform_dir(inv, ray.dir) };
        const MeshData& mesh = mesh_for(o);
        if (ray_aabb(local, mesh.bounds_min, mesh.bounds_max) < 0) continue;
        for (uint32_t v = 0; v + 2 < mesh.vertex_count; v += 3) {
            float t = ray_triangle(local, mesh.positions[v], mesh.positions[v + 1], mesh.positions[v + 2]);
            if (t > 0 && t < best_t) { best_t = t; best = o.id; }
        }
    }
    if (distance) *distance = best_t;
    return best;
}

}  // namespace fe
