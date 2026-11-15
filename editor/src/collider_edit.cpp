// Collider 3D in the viewport: the shape of the active object's collider as
// lines, and — with "Edit shape" on — handles on it that change its size,
// radius or height by dragging.
//
// Scale rules (the physics uses the same): a box's size is in object space
// (scaled per axis); a sphere's radius scales by the largest scale; a
// capsule's or cylinder's radius by the larger of the x and z scales and its
// height by the y scale. Capsules and cylinders stand along local Y.
#include "editor.h"
#include "widgets.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <stdio.h>

namespace fe {

namespace {

struct Proj {
    Mat4 viewproj, inv;
    ImVec2 pos, size;
    bool to_screen(const Vec3& p, ImVec2* out) const {
        Vec4 c = viewproj * Vec4{ p.x, p.y, p.z, 1.0f };
        if (c.w <= 0.01f) return false;
        *out = ImVec2(pos.x + (c.x / c.w * 0.5f + 0.5f) * size.x, pos.y + (0.5f - c.y / c.w * 0.5f) * size.y);
        return true;
    }
    Ray ray(ImVec2 m) const {
        float nx = (m.x - pos.x) / size.x * 2.0f - 1.0f, ny = 1.0f - (m.y - pos.y) / size.y * 2.0f;
        Vec4 n = inv * Vec4{ nx, ny, -1.0f, 1.0f }, f = inv * Vec4{ nx, ny, 1.0f, 1.0f };
        Vec3 pn(n.x / n.w, n.y / n.w, n.z / n.w), pf(f.x / f.w, f.y / f.w, f.z / f.w);
        return { pn, normalize(pf - pn) };
    }
};

void seg(ImDrawList* dl, const Proj& p, const Vec3& a, const Vec3& b, ImU32 c, float th) {
    ImVec2 sa, sb;
    if (p.to_screen(a, &sa) && p.to_screen(b, &sb)) dl->AddLine(sa, sb, c, th);
}

// A circle of radius r around c in the plane of u and v (unit vectors).
void circle(ImDrawList* dl, const Proj& p, const Vec3& c, const Vec3& u, const Vec3& v, float r, ImU32 col, float th, float a0 = 0,
            float a1 = 2 * kPi, int n = 32) {
    Vec3 prev = c + u * (fcos(a0) * r) + v * (fsin(a0) * r);
    for (int i = 1; i <= n; i++) {
        float a = a0 + (a1 - a0) * i / n;
        Vec3 q = c + u * (fcos(a) * r) + v * (fsin(a) * r);
        seg(dl, p, prev, q, col, th);
        prev = q;
    }
}

float absf(float x) { return x < 0 ? -x : x; }

// What a handle changes.
enum class HandleKind : uint8_t { BoxSize, Radius, Height };

struct Handle {
    Vec3 pos;           // world
    Vec3 axis;          // unit, world, pointing out of the shape
    HandleKind kind;
    int local_axis;     // BoxSize: 0..2
};

// The collider's frame: center, unit axes and the object's scale per axis.
struct Frame {
    Vec3 c, ax[3], scale;
};

Frame collider_frame(const Mat4& m, const Collider3D& col) {
    Frame f;
    for (int i = 0; i < 3; i++) {
        Vec3 a(m.m[i * 4], m.m[i * 4 + 1], m.m[i * 4 + 2]);
        float l = length(a);
        f.ax[i] = l > 0 ? a * (1.0f / l) : Vec3(i == 0, i == 1, i == 2);
        (&f.scale.x)[i] = l;
    }
    f.c = transform_point(m, col.center);
    return f;
}

int handles_of(const Frame& f, const Collider3D& col, Handle* out) {
    int n = 0;
    float smax = fmaxf_(f.scale.x, fmaxf_(f.scale.y, f.scale.z));
    float sxz = fmaxf_(f.scale.x, f.scale.z);
    switch (col.shape) {
        case ColliderShape::Box:
        case ColliderShape::Mesh:
            for (int i = 0; i < 3; i++) {
                float he = (&col.size.x)[i] * 0.5f * (&f.scale.x)[i];
                for (int s = -1; s <= 1; s += 2) out[n++] = { f.c + f.ax[i] * (he * s), f.ax[i] * (float)s, HandleKind::BoxSize, i };
            }
            break;
        case ColliderShape::Sphere:
            for (int i = 0; i < 3; i++)
                for (int s = -1; s <= 1; s += 2) out[n++] = { f.c + f.ax[i] * (col.radius * smax * s), f.ax[i] * (float)s, HandleKind::Radius, i };
            break;
        case ColliderShape::Capsule:
        case ColliderShape::Cylinder: {
            float hh = col.height * 0.5f * f.scale.y, r = col.radius * sxz;
            for (int s = -1; s <= 1; s += 2) out[n++] = { f.c + f.ax[1] * (hh * s), f.ax[1] * (float)s, HandleKind::Height, 1 };
            for (int i = 0; i < 3; i += 2)
                for (int s = -1; s <= 1; s += 2) out[n++] = { f.c + f.ax[i] * (r * s), f.ax[i] * (float)s, HandleKind::Radius, i };
            break;
        }
    }
    return n;
}

void draw_shape(ImDrawList* dl, const Proj& p, const Frame& f, const Collider3D& col, ImU32 c) {
    const float th = 1.5f;
    float smax = fmaxf_(f.scale.x, fmaxf_(f.scale.y, f.scale.z));
    float sxz = fmaxf_(f.scale.x, f.scale.z);
    switch (col.shape) {
        case ColliderShape::Box:
        case ColliderShape::Mesh: {
            Vec3 h(col.size.x * 0.5f * f.scale.x, col.size.y * 0.5f * f.scale.y, col.size.z * 0.5f * f.scale.z);
            Vec3 corner[8];
            for (int i = 0; i < 8; i++)
                corner[i] = f.c + f.ax[0] * ((i & 1) ? h.x : -h.x) + f.ax[1] * ((i & 2) ? h.y : -h.y) + f.ax[2] * ((i & 4) ? h.z : -h.z);
            for (int i = 0; i < 8; i++)
                for (int b = 1; b < 8; b <<= 1)
                    if (!(i & b)) seg(dl, p, corner[i], corner[i | b], c, th);
            break;
        }
        case ColliderShape::Sphere: {
            float r = col.radius * smax;
            circle(dl, p, f.c, f.ax[0], f.ax[1], r, c, th);
            circle(dl, p, f.c, f.ax[0], f.ax[2], r, c, th);
            circle(dl, p, f.c, f.ax[1], f.ax[2], r, c, th);
            break;
        }
        case ColliderShape::Capsule:
        case ColliderShape::Cylinder: {
            float r = col.radius * sxz, hh = col.height * 0.5f * f.scale.y;
            bool capsule = col.shape == ColliderShape::Capsule;
            float body = capsule ? fmaxf_(hh - r, 0.0f) : hh;           // half the straight part
            Vec3 top = f.c + f.ax[1] * body, bot = f.c - f.ax[1] * body;
            circle(dl, p, top, f.ax[0], f.ax[2], r, c, th);
            circle(dl, p, bot, f.ax[0], f.ax[2], r, c, th);
            for (int i = 0; i < 4; i++) {
                Vec3 d = (i & 1 ? f.ax[2] : f.ax[0]) * ((i & 2) ? -r : r);
                seg(dl, p, top + d, bot + d, c, th);
            }
            if (capsule) {
                // The round ends: half circles in the two vertical planes.
                circle(dl, p, top, f.ax[0], f.ax[1], r, c, th, 0, kPi, 16);
                circle(dl, p, top, f.ax[2], f.ax[1], r, c, th, 0, kPi, 16);
                circle(dl, p, bot, f.ax[0], f.ax[1], r, c, th, kPi, 2 * kPi, 16);
                circle(dl, p, bot, f.ax[2], f.ax[1], r, c, th, kPi, 2 * kPi, 16);
            }
            break;
        }
    }
}

// Where along the line c + axis * t the mouse ray passes closest.
bool closest_t(const Ray& r, const Vec3& c, const Vec3& axis, float* t) {
    Vec3 w0 = c - r.origin;
    float b = dot(axis, r.dir), d = dot(axis, w0), e = dot(r.dir, w0);
    float denom = 1.0f - b * b;                 // both unit
    if (denom < 1e-5f) return false;            // looking straight down the axis
    *t = (b * e - d) / denom;
    return true;
}

}  // namespace

bool Editor::collider_overlay(const Mat4& view, const Mat4& proj, float x, float y, float w, float h) {
    Object* o = scene.get(scene.active());
    if (!o || !o->modules.has(ModuleKind::Collider3D) || !scene.visible_in_tree(*o)) {
        collider_drag_ = -1;
        return false;
    }
    Collider3D& col = o->modules.collider3d;
    Proj p;
    p.viewproj = proj * view;
    invert(p.viewproj, &p.inv);
    p.pos = ImVec2(x, y);
    p.size = ImVec2(w, h);
    Mat4 m = scene.world_matrix(*o);
    Frame f = collider_frame(m, col);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool trigger = col.mode == ColliderMode::Trigger;
    ImU32 line_col = trigger ? IM_COL32(80, 200, 230, 230) : IM_COL32(110, 220, 120, 230);
    draw_shape(dl, p, f, col, line_col);
    if (!edit_collider) { collider_drag_ = -1; return false; }

    Handle hs[8];
    int n = handles_of(f, col, hs);
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;
    bool in_view = mouse.x >= x && mouse.y >= y && mouse.x < x + w && mouse.y < y + h && ImGui::IsWindowHovered();
    int hover = -1;
    float best = 9.0f * 9.0f;
    ImVec2 sp[8];
    bool vis[8];
    for (int i = 0; i < n; i++) {
        vis[i] = p.to_screen(hs[i].pos, &sp[i]);
        if (!vis[i] || !in_view) continue;
        float dx = sp[i].x - mouse.x, dy = sp[i].y - mouse.y;
        if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; hover = i; }
    }
    // Dragging: the handle follows the mouse along its axis.
    if (collider_drag_ >= 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || collider_drag_object_ != o->id || collider_drag_ >= n) {
            collider_drag_ = -1;
        } else {
            const Handle& hd = hs[collider_drag_];
            float t;
            if (closest_t(p.ray(mouse), f.c, hd.axis, &t)) {
                float dist = fmaxf_(t, 0.01f);
                bool snap = settings.snap != io.KeyCtrl;
                auto snapped = [&](float v) { if (!snap) return v; float k = v / 0.05f; return (float)(long)(k + 0.5f) * 0.05f; };
                float smax = fmaxf_(f.scale.x, fmaxf_(f.scale.y, f.scale.z));
                float sxz = fmaxf_(f.scale.x, f.scale.z);
                switch (hd.kind) {
                    case HandleKind::BoxSize: {
                        float s = (&f.scale.x)[hd.local_axis];
                        if (s > 0) (&col.size.x)[hd.local_axis] = fmaxf_(snapped(dist * 2.0f / s), 0.01f);
                        break;
                    }
                    case HandleKind::Radius: {
                        float s = col.shape == ColliderShape::Sphere ? smax : sxz;
                        if (s > 0) col.radius = fmaxf_(snapped(dist / s), 0.01f);
                        if (col.shape == ColliderShape::Capsule && col.height < col.radius * 2) col.height = col.radius * 2;
                        break;
                    }
                    case HandleKind::Height:
                        if (f.scale.y > 0) col.height = fmaxf_(snapped(dist * 2.0f / f.scale.y), 0.01f);
                        if (col.shape == ColliderShape::Capsule && col.height < col.radius * 2) col.height = col.radius * 2;
                        break;
                }
            }
        }
    } else if (hover >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        collider_drag_ = hover;
        collider_drag_object_ = o->id;
    }
    for (int i = 0; i < n; i++) {
        if (!vis[i]) continue;
        bool hot = i == hover || i == collider_drag_;
        dl->AddCircleFilled(sp[i], hot ? 6.5f : 5.0f, hot ? IM_COL32(255, 255, 255, 255) : line_col, 16);
        dl->AddCircle(sp[i], hot ? 6.5f : 5.0f, IM_COL32(20, 30, 25, 200), 16, 1.5f);
        char key[24];
        snprintf(key, sizeof(key), "chandle:%d", i);
        track_rect(key, sp[i].x - 6, sp[i].y - 6, 12, 12);
    }
    if (hover >= 0 || collider_drag_ >= 0) {
        static const char* what[] = { "size", "radius", "height" };
        const Handle& hh = hs[collider_drag_ >= 0 ? collider_drag_ : hover];
        ImGui::SetTooltip("Collider %s: drag (Ctrl: snap)", what[(int)hh.kind]);
    }
    return hover >= 0 || collider_drag_ >= 0;
}

}  // namespace fe
