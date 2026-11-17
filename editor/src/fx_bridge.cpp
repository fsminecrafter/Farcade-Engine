#include "fx_bridge.h"
#include "editor.h"
#include "ecs/ecs.h"
#include "fx/fx.h"
#include "fx/particles.h"
#include "script/runtime.h"

#include "imgui/imgui.h"

#include <string.h>

namespace fe {

namespace {

// Projects world points into the viewport rectangle.
struct Proj {
    Mat4 view, proj, viewproj;
    float x, y, w, h;
    bool ortho;
    // false when behind the camera
    bool to_screen(const Vec3& p, ImVec2* out, float* clipw) const {
        Vec4 c = viewproj * Vec4{ p.x, p.y, p.z, 1.0f };
        if (c.w <= 0.05f) return false;
        *out = ImVec2(x + (c.x / c.w * 0.5f + 0.5f) * w, y + (0.5f - c.y / c.w * 0.5f) * h);
        if (clipw) *clipw = c.w;
        return true;
    }
    // On-screen pixels per meter at a point of clip depth `cw`.
    float px_per_meter(float cw) const {
        float k = proj.m[5] * 0.5f * h;
        return ortho ? k : k / cw;
    }
};

ImU32 rgba8(float r, float g, float b, float a) {
    auto ch = [](float v) { return (int)(fclamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return IM_COL32(ch(r), ch(g), ch(b), ch(a));
}

// A world-space line clipped against the near plane.
void gline(ImDrawList* dl, const Proj& P, Vec3 a, Vec3 b, ImU32 col, float thickness = 1.5f) {
    a = transform_point(P.view, a);
    b = transform_point(P.view, b);
    const float zn = -0.1f;
    if (a.z > zn && b.z > zn) return;
    if (a.z > zn) a = a + (b - a) * ((zn - a.z) / (b.z - a.z));
    else if (b.z > zn) b = b + (a - b) * ((zn - b.z) / (a.z - b.z));
    ImVec2 sa, sb;
    Vec4 ca = P.proj * Vec4{ a.x, a.y, a.z, 1.0f }, cb = P.proj * Vec4{ b.x, b.y, b.z, 1.0f };
    if (ca.w <= 1e-4f || cb.w <= 1e-4f) return;
    sa = ImVec2(P.x + (ca.x / ca.w * 0.5f + 0.5f) * P.w, P.y + (0.5f - ca.y / ca.w * 0.5f) * P.h);
    sb = ImVec2(P.x + (cb.x / cb.w * 0.5f + 0.5f) * P.w, P.y + (0.5f - cb.y / cb.w * 0.5f) * P.h);
    dl->AddLine(sa, sb, col, thickness);
}

struct Lines {
    ImDrawList* dl;
    const Proj& P;
    const Mat4& m;      // local to world
    ImU32 col;
    Vec3 W(float x, float y, float z) const { return transform_point(m, Vec3(x, y, z)); }
    void line(const Vec3& a, const Vec3& b) const { gline(dl, P, W(a.x, a.y, a.z), W(b.x, b.y, b.z), col); }
    // A circle of radius r in the plane normal to `axis` (0 x, 1 y, 2 z) at offset `at` along it.
    void circle(int axis, float r, float at, int n = 24) const {
        for (int i = 0; i < n; i++) {
            float a0 = 2 * kPi * i / n, a1 = 2 * kPi * (i + 1) / n;
            float c0 = fcos(a0) * r, s0 = fsin(a0) * r, c1 = fcos(a1) * r, s1 = fsin(a1) * r;
            if (axis == 2) line(Vec3(c0, s0, at), Vec3(c1, s1, at));
            else if (axis == 1) line(Vec3(c0, at, s0), Vec3(c1, at, s1));
            else line(Vec3(at, c0, s0), Vec3(at, c1, s1));
        }
    }
    void box(const Vec3& half) const {
        for (int i = 0; i < 4; i++) {
            float sx = (i & 1) ? 1.0f : -1.0f, sy = (i & 2) ? 1.0f : -1.0f;
            line(Vec3(sx * half.x, sy * half.y, -half.z), Vec3(sx * half.x, sy * half.y, half.z));
            line(Vec3(sx * half.x, -half.y, sy * half.z), Vec3(sx * half.x, half.y, sy * half.z));
            line(Vec3(-half.x, sx * half.y, sy * half.z), Vec3(half.x, sx * half.y, sy * half.z));
        }
    }
    void sphere(float r) const { circle(0, r, 0); circle(1, r, 0); circle(2, r, 0); }
    void arrow(const Vec3& from, const Vec3& to) const {
        line(from, to);
        Vec3 d = normalize(to - from);
        Vec3 side = normalize(cross(d, fabsf_(d.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0)));
        float s = length(to - from) * 0.2f;
        line(to, to - d * s + side * (s * 0.5f));
        line(to, to - d * s - side * (s * 0.5f));
    }
};

void draw_gizmos(ImDrawList* dl, const Proj& P, Editor& ed, const Object& o, const Mat4& m) {
    // Scale does not belong in the gizmo of a particle emitter or a joint (size is in the fields).
    Vec3 t, s;
    Quat r;
    decompose(m, &t, &r, &s);
    Mat4 rigid = Mat4::trs(t, r, Vec3(1, 1, 1));
    if (o.modules.has(ModuleKind::ParticleEmitter)) {
        const ParticleEmitter& e = o.modules.particles;
        Lines L{ dl, P, rigid, IM_COL32(255, 170, 60, 230) };
        switch (e.shape) {
        case EmitShape::Cone: {
            float r0 = e.shape_radius, d = 1.5f, r1 = r0 + ftan(radians(e.cone_angle)) * d;
            L.circle(2, r0 < 0.02f ? 0.02f : r0, 0, 16);
            L.circle(2, r1, -d, 20);
            for (int i = 0; i < 4; i++) {
                float a = kPi * 0.5f * i;
                L.line(Vec3(fcos(a) * r0, fsin(a) * r0, 0), Vec3(fcos(a) * r1, fsin(a) * r1, -d));
            }
            break;
        }
        case EmitShape::Sphere: L.sphere(e.shape_radius < 0.02f ? 0.02f : e.shape_radius); break;
        case EmitShape::Box: L.box(e.box_size * 0.5f); L.arrow(Vec3(0, 0, 0), Vec3(0, 0, -0.8f)); break;
        default:
            L.line(Vec3(-0.15f, 0, 0), Vec3(0.15f, 0, 0));
            L.line(Vec3(0, -0.15f, 0), Vec3(0, 0.15f, 0));
            L.line(Vec3(0, 0, -0.15f), Vec3(0, 0, 0.15f));
            break;
        }
    }
    if (o.modules.has(ModuleKind::ForceField)) {
        const ForceField& f = o.modules.force_field;
        Lines L{ dl, P, m, f.enabled ? IM_COL32(90, 200, 255, 230) : IM_COL32(120, 140, 150, 160) };
        if (f.shape == FieldShape::Sphere) L.sphere(f.radius);
        else L.box(f.size * 0.5f);
        float reach = f.shape == FieldShape::Sphere ? f.radius : fmaxf_(f.size.x, fmaxf_(f.size.y, f.size.z)) * 0.5f;
        Lines A{ dl, P, rigid, L.col };
        if (f.mode == FieldMode::Directional) {
            Vec3 d = normalize(f.direction);
            if (length(d) == 0) d = Vec3(0, 0, -1);
            Vec3 side = normalize(cross(d, fabsf_(d.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0)));
            Vec3 up = cross(side, d);
            for (int i = 0; i < 5; i++) {
                Vec3 off = (i == 0 ? Vec3(0, 0, 0) : (i == 1 ? side : (i == 2 ? side * -1.0f : (i == 3 ? up : up * -1.0f)))) * (reach * 0.3f);
                A.arrow(off - d * (reach * 0.4f), off + d * (reach * 0.4f));
            }
        } else if (f.mode == FieldMode::Radial) {
            float a = reach * 0.25f, b = reach * 0.7f;
            Vec3 axes[6] = { Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(0, -1, 0), Vec3(0, 0, 1), Vec3(0, 0, -1) };
            for (auto& ax : axes) {
                if (f.strength >= 0) A.arrow(ax * a, ax * b);
                else A.arrow(ax * b, ax * a);
            }
        } else {
            A.circle(1, reach * 0.5f, 0, 24);
            A.arrow(Vec3(reach * 0.5f, 0, 0), Vec3(reach * 0.5f, 0, f.strength >= 0 ? reach * 0.25f : -reach * 0.25f));
        }
    }
    if (o.modules.has(ModuleKind::ReverbZone)) {
        const ReverbZone& z = o.modules.reverb_zone;
        Lines L{ dl, P, m, z.enabled ? IM_COL32(190, 130, 255, 230) : IM_COL32(130, 120, 150, 160) };
        if (z.shape == FieldShape::Sphere) L.sphere(z.radius);
        else L.box(z.size * 0.5f);
    }
    if (o.modules.has(ModuleKind::Joint3D)) {
        const Joint3D& j = o.modules.joint3d;
        Lines L{ dl, P, m, j.broken ? IM_COL32(150, 150, 150, 200) : IM_COL32(255, 90, 90, 240) };
        Vec3 a = L.W(j.anchor.x, j.anchor.y, j.anchor.z);
        Lines C{ dl, P, Mat4::translation(a), L.col };
        C.line(Vec3(-0.12f, 0, 0), Vec3(0.12f, 0, 0));
        C.line(Vec3(0, -0.12f, 0), Vec3(0, 0.12f, 0));
        C.line(Vec3(0, 0, -0.12f), Vec3(0, 0, 0.12f));
        Object* other = ed.scene.get(j.connected);
        Vec3 b = a - Vec3(0, 0.6f, 0);
        if (other) { Mat4 om = ed.scene.world_matrix(*other); b = Vec3(om.m[12], om.m[13], om.m[14]); }
        gline(dl, P, a, b, L.col, 1.0f);
        if (!other) { Lines G{ dl, P, Mat4::translation(b), L.col }; G.line(Vec3(-0.15f, 0, 0), Vec3(0.15f, 0, 0)); G.line(Vec3(0, 0, -0.15f), Vec3(0, 0, 0.15f)); }
        if (j.type == JointType::Hinge) {
            Vec3 ax = normalize(transform_dir(m, j.axis));
            gline(dl, P, a - ax * 0.4f, a + ax * 0.4f, IM_COL32(255, 220, 90, 240), 2.0f);
        }
    }
    if (o.modules.has(ModuleKind::Spawner)) {
        const Spawner& sp = o.modules.spawner;
        Lines L{ dl, P, rigid, IM_COL32(120, 230, 140, 230) };
        L.box(Vec3(sp.offset.x < 0.05f ? 0.05f : sp.offset.x, sp.offset.y < 0.05f ? 0.05f : sp.offset.y, sp.offset.z < 0.05f ? 0.05f : sp.offset.z));
        if (Object* src = ed.scene.get(sp.source)) {
            Mat4 sm = ed.scene.world_matrix(*src);
            gline(dl, P, Vec3(t.x, t.y, t.z), Vec3(sm.m[12], sm.m[13], sm.m[14]), IM_COL32(120, 230, 140, 140), 1.0f);
        }
        if (length(sp.velocity) > 0.01f) L.arrow(Vec3(0, 0, 0), sp.velocity * (1.0f / (length(sp.velocity) < 1.0f ? 1.0f : length(sp.velocity))));
    }
    if (o.modules.has(ModuleKind::Billboard)) {
        const Billboard& b = o.modules.billboard;
        Lines L{ dl, P, rigid, IM_COL32(255, 255, 255, 140) };
        // Quad facing the viewer is drawn by the sprite overlay; the outline here follows the object.
        L.box(Vec3(b.width * 0.5f, b.height * 0.5f, 0.001f));
    }
}

}  // namespace

FxBridge::FxBridge() {}
FxBridge::~FxBridge() {
    stop();
    delete preview_;
}

void FxBridge::start(Editor& ed) {
    stop();
    World* w = ed.play_world();
    if (!w) return;
    fx_ = new FxWorld();
    FxHost host;
    host.user = &ed;
    host.physics = ed.play_physics();
    if (!fx_->init(w, host)) { delete fx_; fx_ = nullptr; return; }
    if (ed.play_scripts()) fx_->bind_script(ed.play_scripts());
    delete preview_;
    preview_ = nullptr;
}

void FxBridge::stop() {
    if (fx_) { fx_->shutdown(); delete fx_; fx_ = nullptr; }
}

int FxBridge::preview_emitters() const { return preview_ ? preview_->sim_count() : 0; }

void FxBridge::update_preview(Editor& ed, float dt) {
    if (fx_) return;
    if (!preview_) preview_ = new ParticleBank();
    preview_time_ += dt;
    FieldVolume fields[kMaxFieldsPerStep];
    int nf = 0;
    Scene& sc = ed.scene;
    for (int i = 0; i < sc.count() && nf < kMaxFieldsPerStep; i++) {
        Object& o = sc.at(i);
        if (o.modules.has(ModuleKind::ForceField) && o.modules.force_field.affect_particles && sc.visible_in_tree(o))
            if (field_volume(o.modules.force_field, sc.world_matrix(o), &fields[nf])) nf++;
    }
    preview_->begin();
    for (int i = 0; i < sc.selected_count(); i++) {
        Object* o = sc.get(sc.selected(i));
        if (!o || !o->modules.has(ModuleKind::ParticleEmitter) || !sc.visible_in_tree(*o)) continue;
        preview_->update(o->id, o->modules.particles, sc.world_matrix(*o), dt, fields, nf, (float)preview_time_);
    }
    preview_->end();
}

void FxBridge::draw_overlay(Editor& ed, ImDrawList* dl, const Mat4& view, const Mat4& proj, float x, float y, float w, float h, const Vec3& eye) {
    Proj P;
    P.view = view; P.proj = proj; P.viewproj = proj * view;
    P.x = x; P.y = y; P.w = w; P.h = h;
    P.ortho = proj.m[15] == 1.0f && proj.m[11] == 0.0f;
    last_particles = 0;
    last_sprites = 0;

    // Particles: the play world's, or the preview of the selected emitters.
    const gfx::ParticleBatch* batches = nullptr;
    int nb = 0;
    if (fx_) nb = fx_->particles.collect(eye, &batches);
    else if (preview_) nb = preview_->collect(eye, &batches);
    int budget = 4000;
    for (int b = 0; b < nb && budget > 0; b++) {
        const gfx::ParticleBatch& pb = batches[b];
        for (int i = 0; i < pb.count && budget > 0; i++, budget--) {
            const gfx::Particle& q = pb.items[i];
            ImVec2 c;
            float cw;
            if (!P.to_screen(q.pos, &c, &cw)) continue;
            float rad = q.size * 0.5f * P.px_per_meter(cw);
            if (rad < 0.4f) continue;
            if (c.x + rad < x || c.x - rad > x + w || c.y + rad < y || c.y - rad > y + h) continue;
            // A soft dot: a wide faint disc and a smaller brighter one. Additive shows as glow
            // by drawing brighter.
            float ga = fclamp(q.a, 0.0f, 1.0f);
            if (pb.additive) {
                dl->AddCircleFilled(c, rad, rgba8(q.r, q.g, q.b, ga * 0.35f), rad > 6 ? 14 : 8);
                dl->AddCircleFilled(c, rad * 0.55f, rgba8(q.r * 1.1f, q.g * 1.1f, q.b * 1.1f, ga * 0.8f), rad > 6 ? 12 : 8);
            } else {
                dl->AddCircleFilled(c, rad, rgba8(q.r, q.g, q.b, ga * 0.5f), rad > 6 ? 14 : 8);
                dl->AddCircleFilled(c, rad * 0.6f, rgba8(q.r, q.g, q.b, ga * 0.9f), rad > 6 ? 12 : 8);
            }
            last_particles++;
        }
    }

    // Gizmos of the selected objects.
    Scene& sc = ed.scene;
    for (int i = 0; i < sc.selected_count(); i++) {
        Object* o = sc.get(sc.selected(i));
        if (!o || !sc.visible_in_tree(*o)) continue;
        draw_gizmos(dl, P, ed, *o, sc.world_matrix(*o));
    }
}

}  // namespace fe
