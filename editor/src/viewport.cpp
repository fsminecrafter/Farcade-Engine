// The 3D viewport: grid, objects, picking, camera controls and gizmos.
//
// For now the scene is drawn by a small depth-buffered CPU renderer
// (view3d.h) into an image, the same on the host and on Minimal-OS. The
// FarcadeGL renderer replaces that drawing later; picking, camera and
// gizmos stay.
#include "editor.h"
#include "icons.h"
#include "swraster.h"
#include "view3d.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imguizmo/ImGuizmo.h"

#include <stdlib.h>
#include <string.h>

namespace fe {

namespace {

// 0xRRGGBB for a base color under a light factor; selected objects get a
// touch of the accent color.
uint32_t shade(const Vec3& base, float light, bool selected) {
    Vec3 c = base * light;
    if (selected) c = c * 0.8f + Vec3(0.30f, 0.55f, 1.0f) * 0.2f;
    auto ch = [](float v) { return (uint32_t)(fclamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return ch(c.x) << 16 | ch(c.y) << 8 | ch(c.z);
}

struct View {
    ImVec2 pos, size;
    Mat4 view, proj, viewproj;
    float near_z = 0.1f;
    ImVec2 to_screen(const Vec3& view_space) const {
        Vec4 c = proj * Vec4{ view_space.x, view_space.y, view_space.z, 1.0f };
        float iw = 1.0f / c.w;
        return ImVec2(pos.x + (c.x * iw * 0.5f + 0.5f) * size.x, pos.y + (0.5f - c.y * iw * 0.5f) * size.y);
    }
};

// Draws a world-space line, clipped against the near plane.
void line3(ImDrawList* dl, const View& v, Vec3 a, Vec3 b, ImU32 col, float thickness) {
    a = transform_point(v.view, a);
    b = transform_point(v.view, b);
    float zn = -v.near_z;
    if (a.z > zn && b.z > zn) return;
    if (a.z > zn) a = a + (b - a) * ((zn - a.z) / (b.z - a.z));
    else if (b.z > zn) b = b + (a - b) * ((zn - b.z) / (a.z - b.z));
    dl->AddLine(v.to_screen(a), v.to_screen(b), col, thickness);
}

Ray mouse_ray(const View& v, ImVec2 mouse) {
    float nx = (mouse.x - v.pos.x) / v.size.x * 2.0f - 1.0f;
    float ny = 1.0f - (mouse.y - v.pos.y) / v.size.y * 2.0f;
    Mat4 inv;
    invert(v.viewproj, &inv);
    Vec4 n = inv * Vec4{ nx, ny, -1.0f, 1.0f };
    Vec4 f = inv * Vec4{ nx, ny, 1.0f, 1.0f };
    Vec3 pn(n.x / n.w, n.y / n.w, n.z / n.w), pf(f.x / f.w, f.y / f.w, f.z / f.w);
    return { pn, normalize(pf - pn) };
}

bool ray_ground(const Ray& r, Vec3* hit) {
    if (r.dir.y > -1e-5f && r.dir.y < 1e-5f) return false;
    float t = -r.origin.y / r.dir.y;
    if (t < 0) return false;
    *hit = r.origin + r.dir * t;
    return true;
}

Quat conjugate(const Quat& q) { return { -q.x, -q.y, -q.z, q.w }; }

}  // namespace

void Editor::panel_viewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    View v;
    v.pos = ImGui::GetCursorScreenPos();
    v.size = ImGui::GetContentRegionAvail();
    if (v.size.x < 16 || v.size.y < 16) { ImGui::End(); return; }
    view_rect[0] = v.pos.x; view_rect[1] = v.pos.y; view_rect[2] = v.size.x; view_rect[3] = v.size.y;
    v.view = camera.view();
    v.proj = camera.projection(v.size.x / v.size.y);
    v.viewproj = v.proj * v.view;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 end(v.pos.x + v.size.x, v.pos.y + v.size.y);
    dl->PushClipRect(v.pos, end, true);

    // The scene into the viewport image: gradient background, grid, objects.
    static View3D view3d;
    uint32_t top = settings.dark_theme ? 0x2C2F36 : 0xD6DCE6;
    uint32_t bottom = settings.dark_theme ? 0x1B1D21 : 0xEEF1F5;
    view3d.begin((int)v.size.x, (int)v.size.y, v.viewproj, top, bottom);
    Vec3 light_dir = normalize(Vec3(0.45f, 0.85f, 0.35f));
    for (int i = 0; i < scene.count(); i++) {
        Object& o = scene.at(i);
        if (!o.visible) continue;
        const MeshData& mesh = Scene::mesh_for(o);
        Mat4 model = o.matrix();
        bool sel = scene.is_selected(o.id);
        for (uint32_t t = 0; t + 2 < mesh.vertex_count; t += 3) {
            Vec3 a = transform_point(model, mesh.positions[t]);
            Vec3 b = transform_point(model, mesh.positions[t + 1]);
            Vec3 c = transform_point(model, mesh.positions[t + 2]);
            Vec3 n = normalize(cross(b - a, c - a));
            float lit = 0.35f + 0.65f * fmaxf_(0.0f, dot(n, light_dir));
            view3d.triangle(a, b, c, shade(o.color, lit, sel));
        }
    }
    if (settings.show_grid) {
        float g = settings.grid_size;
        int n = settings.grid_lines;
        float ext = g * n;
        uint32_t gc = settings.dark_theme ? 0xFFFFFF : 0x000000;
        for (int i = -n; i <= n; i++) {
            if (i == 0) continue;
            int alpha = (i % 10 == 0) ? 48 : 20;
            view3d.line(Vec3(i * g, 0, -ext), Vec3(i * g, 0, ext), gc, alpha);
            view3d.line(Vec3(-ext, 0, i * g), Vec3(ext, 0, i * g), gc, alpha);
        }
        view3d.line(Vec3(-ext, 0, 0), Vec3(ext, 0, 0), 0xDC5050, 170);
        view3d.line(Vec3(0, 0, -ext), Vec3(0, 0, ext), 0x5082E6, 170);
    }
    dl->AddImage((ImTextureID)SwRenderer::texture_id(view3d.texture()), v.pos, ImVec2(v.pos.x + view3d.width(), v.pos.y + view3d.height()));

    // Selection: the bounding box of each selected object in the accent color.
    for (int s = 0; s < scene.selected_count(); s++) {
        Object* o = scene.get(scene.selected(s));
        if (!o) continue;
        const MeshData& m = Scene::mesh_for(*o);
        Mat4 mm = o->matrix();
        Vec3 c[8];
        for (int k = 0; k < 8; k++)
            c[k] = transform_point(mm, Vec3((k & 1) ? m.bounds_max.x : m.bounds_min.x, (k & 2) ? m.bounds_max.y : m.bounds_min.y,
                                            (k & 4) ? m.bounds_max.z : m.bounds_min.z));
        static const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 },
                                          { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
        ImU32 col = o->id == scene.active() ? IM_COL32(90, 150, 255, 255) : IM_COL32(90, 150, 255, 140);
        for (auto& e : edges) line3(dl, v, c[e[0]], c[e[1]], col, 1.5f);
    }

    // Gizmo on the active object. It comes before the input area: ImGuizmo
    // only starts a drag when no other item is hovered, so the input area is
    // left out while the mouse is over the gizmo or dragging it.
    bool gizmo_active = false;
    ImGuiIO& gio = ImGui::GetIO();
    Object* act = scene.get(scene.active());
    if (act && tool != Tool::Select) {
        ImGuizmo::SetDrawlist(dl);
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetRect(v.pos.x, v.pos.y, v.size.x, v.size.y);
        ImGuizmo::SetGizmoSizeClipSpace(0.17f);
        ImGuizmo::Style& gs = ImGuizmo::GetStyle();
        gs.TranslationLineThickness = 3.0f;
        gs.TranslationLineArrowSize = 7.0f;
        gs.RotationLineThickness = 2.5f;
        gs.RotationOuterLineThickness = 2.5f;
        gs.ScaleLineThickness = 3.0f;
        gs.ScaleLineCircleSize = 6.5f;
        gs.CenterCircleSize = 5.0f;
        ImGuizmo::OPERATION op = tool == Tool::Move ? ImGuizmo::TRANSLATE : (tool == Tool::Rotate ? ImGuizmo::ROTATE : ImGuizmo::SCALE);
        ImGuizmo::MODE mode = (gizmo_local || tool == Tool::Scale) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
        bool snap = settings.snap != gio.KeyCtrl;
        float step = tool == Tool::Move ? settings.snap_move : (tool == Tool::Rotate ? settings.snap_rotate : settings.snap_scale);
        float snap3[3] = { step, step, step };
        Mat4 m = act->matrix();
        Vec3 old_pos = act->position, old_scale = act->scale;
        Quat old_rot = act->rotation;
        if (ImGuizmo::Manipulate(v.view.m, v.proj.m, op, mode, m.m, nullptr, snap ? snap3 : nullptr)) {
            act->set_matrix(m);
            if (tool == Tool::Move && snap && !gizmo_local) {
                // Keep positions on the snap grid, not just the steps.
                auto q = [&](float x) { float k = x / step; long n = (long)(k + (k >= 0 ? 0.5f : -0.5f)); return n * step; };
                act->position = Vec3(q(act->position.x), q(act->position.y), q(act->position.z));
            }
            // The rest of the selection follows the active object.
            Vec3 dpos = act->position - old_pos;
            Quat drot = normalize(act->rotation * conjugate(old_rot));
            Vec3 ratio(old_scale.x != 0 ? act->scale.x / old_scale.x : 1, old_scale.y != 0 ? act->scale.y / old_scale.y : 1,
                       old_scale.z != 0 ? act->scale.z / old_scale.z : 1);
            for (int s = 0; s < scene.selected_count(); s++) {
                Object* o = scene.get(scene.selected(s));
                if (!o || o == act) continue;
                if (tool == Tool::Move) o->position += dpos;
                else if (tool == Tool::Rotate) {
                    o->position = old_pos + rotate(drot, o->position - old_pos);
                    o->rotation = normalize(drot * o->rotation);
                } else o->scale = mul(o->scale, ratio);
            }
        }
        gizmo_active = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetCursorScreenPos(v.pos);
    if (gizmo_active) ImGui::Dummy(v.size);
    else ImGui::InvisibleButton("##view", v.size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                                      ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = !gizmo_active && ImGui::IsItemHovered();

    // Files dropped on the viewport: onto an object, or a model onto the ground.
    if (!gizmo_active && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("FE_FILE")) {
            const char* path = (const char*)p->Data;
            Ray r = mouse_ray(v, io.MousePos);
            ObjectId hit = scene.pick(r);
            if (hit != kNoObject) apply_file_to_object(path, hit);
            else if (path_is_model(path)) {
                Vec3 at;
                if (!ray_ground(r, &at)) at = camera.target;
                ObjectId id = create_object(path_filename(path), MeshKind::Model, at + Vec3(0, 0.5f, 0));
                apply_file_to_object(path, id);
            } else {
                log(LogLevel::Warning, "Drop %s on an object", path_filename(path));
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Camera: right-drag orbits, middle-drag (or shift + right) pans, wheel zooms.
    if (hovered && !gizmo_active) {
        if (io.MouseWheel != 0) camera.distance = fclamp(camera.distance * (io.MouseWheel > 0 ? 0.85f : 1.18f), 0.5f, 400.0f);
    }
    if (!gizmo_active && ImGui::IsItemActive()) {
        ImVec2 d = io.MouseDelta;
        bool pan = ImGui::IsMouseDown(ImGuiMouseButton_Middle) || (ImGui::IsMouseDown(ImGuiMouseButton_Right) && io.KeyShift);
        if (pan) {
            float k = camera.distance * 0.0016f;
            Mat4 inv;
            invert(v.view, &inv);
            Vec3 right = normalize(transform_dir(inv, Vec3(1, 0, 0)));
            Vec3 up = normalize(transform_dir(inv, Vec3(0, 1, 0)));
            camera.target -= right * (d.x * k);
            camera.target += up * (d.y * k);
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            camera.yaw -= d.x * 0.3f;
            camera.pitch = fclamp(camera.pitch + d.y * 0.3f, -89.0f, 89.0f);
        }
    }

    // Click (without dragging) selects; shift-click adds or removes.
    if (hovered && !gizmo_active && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0).x * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0).x +
                ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0).y * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0).y < 16.0f) {
        ObjectId hit = scene.pick(mouse_ray(v, io.MousePos));
        if (io.KeyShift) { if (hit != kNoObject) scene.toggle(hit); }
        else scene.select_only(hit);
    }

    // Overlay: what is shown and the axes.
    ImU32 dimc = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    char info[96];
    snprintf(info, sizeof(info), "Perspective   %d object%s   %d selected", scene.count(), scene.count() == 1 ? "" : "s",
             scene.selected_count());
    dl->AddText(ImVec2(v.pos.x + 12, v.pos.y + 10), dimc, info);
    {
        ImVec2 o(v.pos.x + 40, end.y - 40);
        struct Ax { Vec3 d; ImU32 c; const char* n; } axes[3] = {
            { { 1, 0, 0 }, IM_COL32(230, 80, 80, 255), "X" }, { { 0, 1, 0 }, IM_COL32(110, 200, 90, 255), "Y" },
            { { 0, 0, 1 }, IM_COL32(80, 140, 240, 255), "Z" } };
        for (auto& a : axes) {
            Vec3 d = transform_dir(v.view, a.d);
            ImVec2 tip(o.x + d.x * 24, o.y - d.y * 24);
            dl->AddLine(o, tip, a.c, 2.0f);
            dl->AddText(ImVec2(tip.x - 3, tip.y - 7), a.c, a.n);
        }
    }
    if (scene.count() == 0) {
        const char* hint = "Create > Cube to add an object, or drop a .mof file here";
        ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(v.pos.x + (v.size.x - ts.x) * 0.5f, v.pos.y + v.size.y * 0.5f), dimc, hint);
    }
    dl->PopClipRect();
    ImGui::End();
}

}  // namespace fe
