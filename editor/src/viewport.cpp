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
#include "platform/platform.h"

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

// Everything the viewport image depends on, hashed (FNV-1a).
uint64_t Editor::view_hash(int w, int h) const {
    uint64_t hsh = 1469598103934665603ull;
    auto mix = [&](const void* data, size_t n) {
        const uint8_t* b = (const uint8_t*)data;
        for (size_t i = 0; i < n; i++) { hsh ^= b[i]; hsh *= 1099511628211ull; }
    };
    mix(&w, sizeof(w));
    mix(&h, sizeof(h));
    mix(&camera, sizeof(camera));
    mix(&view_mode, sizeof(view_mode));
    bool flags[4] = { settings.show_grid, view_shadows_now(), view_supersample_now(), settings.dark_theme };
    mix(flags, sizeof(flags));
    mix(&settings.grid_size, sizeof(settings.grid_size));
    Scene& sc = const_cast<Scene&>(scene);
    for (int i = 0; i < sc.count(); i++) {
        const Object& o = sc.at(i);
        mix(&o.id, sizeof(o.id));
        mix(&o.position, sizeof(o.position));
        mix(&o.rotation, sizeof(o.rotation));
        mix(&o.scale, sizeof(o.scale));
        mix(&o.color, sizeof(o.color));
        mix(&o.mesh, sizeof(o.mesh));
        mix(&o.visible, sizeof(o.visible));
    }
    for (int i = 0; i < sc.selected_count(); i++) { ObjectId id = sc.selected(i); mix(&id, sizeof(id)); }
    ObjectId act = sc.active();
    mix(&act, sizeof(act));
    return hsh;
}

void Editor::render_view(View3D& view3d, const void* view_ptr) {
    const View& v = *(const View*)view_ptr;
    double (*clock)() = perf_clock ? perf_clock : platform::time_seconds;
    double t0 = clock();

    uint32_t top = settings.dark_theme ? 0x2B3038 : 0xC9D0DA;
    uint32_t bottom = settings.dark_theme ? 0x16181C : 0xEEF1F5;
    view3d.begin((int)v.size.x, (int)v.size.y, v.view, v.proj, top, bottom, view_supersample_now() ? 2 : 1);
    bool wire = view_mode == ViewMode::Wireframe;
    {
        uint16_t sel_ids[256];
        int nsel = 0;
        for (int s2 = 0; s2 < scene.selected_count() && nsel < 256; s2++) sel_ids[nsel++] = (uint16_t)scene.selected(s2);
        view3d.select(sel_ids, wire ? 0 : nsel, wire ? 0 : (uint16_t)scene.active());
    }
    int wire_tris = 0;
    for (int i = 0; i < scene.count(); i++) {
        Object& o = scene.at(i);
        if (!o.visible) continue;
        const MeshData& mesh = Scene::mesh_for(o);
        Mat4 model = o.matrix();
        if (!wire) {
            view3d.mesh(mesh.positions, mesh.normals, mesh.vertex_count, model, o.color, (uint16_t)o.id, view_mode == ViewMode::Unlit);
            continue;
        }
        bool sel = scene.is_selected(o.id);
        uint32_t wc = sel ? 0x6EA3FF : (settings.dark_theme ? 0xC8CDD5 : 0x3A3F47);
        for (uint32_t t = 0; t + 2 < mesh.vertex_count; t += 3) {
            Vec3 a = transform_point(model, mesh.positions[t]);
            Vec3 b = transform_point(model, mesh.positions[t + 1]);
            Vec3 c = transform_point(model, mesh.positions[t + 2]);
            wire_tris++;
            view3d.line(a, b, wc, 190);
            view3d.line(b, c, wc, 190);
            view3d.line(c, a, wc, 190);
        }
    }
    // Shadows on the ground from everything that is not flat on it.
    if (view_shadows_now() && !wire && view_mode != ViewMode::Unlit) {
        for (int i = 0; i < scene.count(); i++) {
            Object& o = scene.at(i);
            if (!o.visible || o.mesh == MeshKind::Plane) continue;
            const MeshData& mesh = Scene::mesh_for(o);
            view3d.shadow(mesh.positions, mesh.vertex_count, o.matrix());
        }
    }
    if (settings.show_grid) view3d.grid(settings.grid_size, settings.dark_theme);
    // Shadows, smoothing, and the selection outline (the active object brighter).
    view3d.finish(0xFFB13B, 0xD98A1C, settings.dark_theme ? 0.42f : 0.30f);
    view_triangles = view3d.triangles() + wire_tris;
    view_ms = (float)((clock() - t0) * 1000.0);

}

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

    // The scene into the viewport image, redrawn only when something it
    // shows has changed (the CPU preview is not cheap; idle frames are free).
    static View3D view3d;
    static uint64_t last_hash = 0;
    uint64_t hash = view_hash((int)v.size.x, (int)v.size.y);
    if (hash != last_hash || !view3d.width()) {
        last_hash = hash;
        render_view(view3d, &v);
    }
    dl->AddImage((ImTextureID)SwRenderer::texture_id(view3d.texture()), v.pos, ImVec2(v.pos.x + view3d.width(), v.pos.y + view3d.height()));

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
        gs.Colors[ImGuizmo::DIRECTION_X] = ImGui::ColorConvertU32ToFloat4(pal.axis[0]);
        gs.Colors[ImGuizmo::DIRECTION_Y] = ImGui::ColorConvertU32ToFloat4(pal.axis[1]);
        gs.Colors[ImGuizmo::DIRECTION_Z] = ImGui::ColorConvertU32ToFloat4(pal.axis[2]);
        gs.Colors[ImGuizmo::PLANE_X] = ImGui::ColorConvertU32ToFloat4(pal.axis[0] & 0x80FFFFFF);
        gs.Colors[ImGuizmo::PLANE_Y] = ImGui::ColorConvertU32ToFloat4(pal.axis[1] & 0x80FFFFFF);
        gs.Colors[ImGuizmo::PLANE_Z] = ImGui::ColorConvertU32ToFloat4(pal.axis[2] & 0x80FFFFFF);
        gs.Colors[ImGuizmo::SELECTION] = ImVec4(1.0f, 0.78f, 0.25f, 0.9f);
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
    bool camera_drag = false;
    if (!gizmo_active && ImGui::IsItemActive()) {
        ImVec2 d = io.MouseDelta;
        camera_drag = ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle);
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

    // Play mode: a frame around the view.
    if (playing()) {
        dl->AddRect(ImVec2(v.pos.x + 1, v.pos.y + 1), ImVec2(end.x - 1, end.y - 1), pal.play, 0, 0, 2.0f);
    }
    if (scene.count() == 0) {
        const char* hint = "Create > Cube to add an object, or drop a .mof file here";
        ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(v.pos.x + (v.size.x - ts.x) * 0.5f, v.pos.y + v.size.y * 0.5f), pal.text_dim, hint);
    }
    dl->PopClipRect();

    // While the camera or an object is being dragged, the next frames render
    // at full speed (no supersampling or shadows); the full image comes back
    // when the drag ends (the setting is part of the hash).
    view_interacting_ = (!gizmo_active && camera_drag) || ImGuizmo::IsUsing();

    // Floating toolbars and the orientation gizmo, on top of the view.
    viewport_overlays(v.pos.x, v.pos.y, v.size.x, v.size.y);
    ImGui::End();
}

// ---------------------------------------------------------------- overlays

namespace {

// A floating rounded panel at (x, y) of size (w, h), as a child window so
// that clicks on it never reach the view below.
bool begin_overlay(const char* id, float x, float y, float w, float h) {
    const Palette& pal = ui_palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x + 1, y + 2), ImVec2(x + w + 1, y + h + 3), pal.shadow, 8.0f);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(pal.overlay));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(pal.border));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 7.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
    bool open = ImGui::BeginChild(id, ImVec2(w, h), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
    return open;
}

void overlay_separator(float h) {
    ImGui::SameLine(0, 4);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 4), ImVec2(p.x, p.y + h - 4), ui_palette().border, 1.0f);
    ImGui::Dummy(ImVec2(1, h));
    ImGui::SameLine(0, 4);
}

}  // namespace

void Editor::viewport_overlays(float x, float y, float w, float h) {
    float bs = ImGui::GetFrameHeight() + 2;
    float pad = 4;
    float margin = 10;

    // Tools: select, move, rotate, scale | world/local | snap | grid.
    float snap_w = 58 * settings.ui_scale;
    float tools_w = pad * 2 + bs * 4 + 2 * 3 + 9 + bs + 9 + bs + 2 + snap_w + 9 + bs;
    if (w > tools_w + 140 && begin_overlay("##tools", x + margin, y + margin, tools_w, bs + pad * 2)) {
        struct ToolDef { Tool tool; Icon icon; const char* tip; const char* key; };
        static const ToolDef tools[] = {
            { Tool::Select, Icon::Pointer, "Select (Q)", "tool:select" }, { Tool::Move, Icon::Move, "Move (W)", "tool:move" },
            { Tool::Rotate, Icon::Rotate, "Rotate (E)", "tool:rotate" }, { Tool::Scale, Icon::Scale, "Scale (R)", "tool:scale" },
        };
        for (int i = 0; i < 4; i++) {
            if (i) ImGui::SameLine(0, 2);
            if (icon_button(tools[i].icon, tools[i].tip, tool == tools[i].tool, bs)) tool = tools[i].tool;
            track(tools[i].key);
        }
        overlay_separator(bs);
        if (icon_button(gizmo_local ? Icon::Local : Icon::World, gizmo_local ? "Local axes (click for world)" : "World axes (click for local)",
                        false, bs))
            gizmo_local = !gizmo_local;
        track("tool:space");
        overlay_separator(bs);
        if (icon_button(Icon::Magnet, "Snap (hold Ctrl to invert)", settings.snap, bs)) settings.snap = !settings.snap;
        track("tool:snap");
        ImGui::SameLine(0, 2);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (bs - ImGui::GetFrameHeight()) * 0.5f);
        ImGui::SetNextItemWidth(snap_w);
        float* snapv = tool == Tool::Rotate ? &settings.snap_rotate : (tool == Tool::Scale ? &settings.snap_scale : &settings.snap_move);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(pal.frame));
        ImGui::DragFloat("##snapv", snapv, tool == Tool::Rotate ? 1.0f : 0.05f, 0.01f, 360.0f, tool == Tool::Rotate ? "%.0f\xC2\xB0" : "%.2f");
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Snap step for the current tool");
        overlay_separator(bs);
        if (icon_button(Icon::Grid, "Grid (G)", settings.show_grid, bs)) settings.show_grid = !settings.show_grid;
        track("tool:grid");
        ImGui::EndChild();
    }

    // View menu: fixed views of the scene.
    {
        const char* label = "Perspective";
        float lw = ImGui::CalcTextSize(label).x + ImGui::GetFontSize() + 22;
        float vx = x + margin + tools_w + 8;
        if (w > tools_w + lw + 140 && begin_overlay("##viewmenu", vx, y + margin, lw, bs + pad * 2)) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##view", ImVec2(lw - pad * 2, bs))) ImGui::OpenPopup("##viewpopup");
            track("view:menu");
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (ImGui::IsItemHovered()) dl->AddRectFilled(p, ImVec2(p.x + lw - pad * 2, p.y + bs), pal.hover, 4.0f);
            float fs = ImGui::GetFontSize();
            dl->AddText(ImVec2(p.x + 7, p.y + (bs - fs) * 0.5f), pal.text, label);
            draw_icon(dl, Icon::ChevronDown, ImVec2(p.x + lw - pad * 2 - fs - 4, p.y + (bs - fs) * 0.5f), fs * 0.9f, pal.text_faint);
            if (ImGui::BeginPopup("##viewpopup")) {
                struct V { const char* n; float yaw, pitch; } views[] = {
                    { "Top", camera.yaw, 89.9f }, { "Bottom", camera.yaw, -89.9f }, { "Front", 0, 0 },
                    { "Back", 180, 0 }, { "Right", 90, 0 }, { "Left", -90, 0 },
                };
                for (const V& vw : views) {
                    if (menu_item(vw.n)) set_view(vw.yaw, vw.pitch);
                }
                ImGui::Separator();
                if (menu_item("Frame selection", "F", false, scene.selected_count() > 0)) frame_selection();
                if (menu_item("Reset view")) camera = Camera();
                ImGui::EndPopup();
            }
            ImGui::EndChild();
        }
    }

    // Orientation gizmo in the top-right corner.
    float gsz = 84 * settings.ui_scale;
    if (w > gsz * 3 && h > gsz * 2) orientation_gizmo(x + w - gsz - margin * 0.5f, y + margin * 0.5f, gsz);
}

void Editor::orientation_gizmo(float x, float y, float size) {
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::BeginChild("##orient", ImVec2(size, size), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    if (!open) { ImGui::EndChild(); return; }
    ImVec2 c(x + size * 0.5f, y + size * 0.5f);
    float r = size * 0.5f - 11;         // axis length
    float br = 8.5f * settings.ui_scale; // bubble radius
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::InvisibleButton("##orient_btn", ImVec2(size, size), ImGuiButtonFlags_MouseButtonLeft);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    if (active) view_interacting_ = true;
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered || active) dl->AddCircleFilled(c, size * 0.5f - 1, pal.overlay, 32);

    // The six axis ends in view space, drawn back to front.
    Mat4 view = camera.view();
    struct End { Vec3 d; int axis; bool positive; ImVec2 p; float depth; };
    End ends[6];
    for (int i = 0; i < 6; i++) {
        Vec3 dir(0, 0, 0);
        (&dir.x)[i / 2] = (i & 1) ? -1.0f : 1.0f;
        Vec3 d = transform_dir(view, dir);
        ends[i] = { dir, i / 2, (i & 1) == 0, ImVec2(c.x + d.x * r, c.y - d.y * r), d.z };
    }
    for (int i = 0; i < 6; i++)
        for (int j = i + 1; j < 6; j++)
            if (ends[j].depth < ends[i].depth) { End t = ends[i]; ends[i] = ends[j]; ends[j] = t; }
    static const char* names[3] = { "X", "Y", "Z" };
    static const char* keys[6] = { "axis:+x", "axis:-x", "axis:+y", "axis:-y", "axis:+z", "axis:-z" };
    int hit = -1;
    for (int i = 5; i >= 0 && hovered; i--) {      // front-most first
        float dx = io.MousePos.x - ends[i].p.x, dy = io.MousePos.y - ends[i].p.y;
        if (dx * dx + dy * dy <= br * br) { hit = i; break; }
    }
    for (int i = 0; i < 6; i++) {
        const End& e = ends[i];
        ImU32 col = pal.axis[e.axis];
        if (e.positive) dl->AddLine(c, e.p, col, 2.0f);
        if (e.positive) {
            dl->AddCircleFilled(e.p, br, col, 20);
            ImVec2 ts = ImGui::CalcTextSize(names[e.axis]);
            dl->AddText(ImVec2(e.p.x - ts.x * 0.5f, e.p.y - ts.y * 0.5f), IM_COL32(16, 18, 21, 255), names[e.axis]);
        } else {
            dl->AddCircleFilled(e.p, br * 0.8f, (col & 0x00FFFFFF) | 0x50000000, 20);
            dl->AddCircle(e.p, br * 0.8f, col, 20, 1.5f);
        }
        if (i == hit) dl->AddCircle(e.p, br + 2, IM_COL32(255, 255, 255, 220), 20, 1.5f);
        int k = e.axis * 2 + (e.positive ? 0 : 1);
        track_rect(keys[k], e.p.x - br, e.p.y - br, br * 2, br * 2);
    }
    if (hit >= 0) {
        const End& e = ends[hit];
        static const char* tips[6] = { "View from +X (right)", "View from -X (left)", "View from +Y (top)", "View from -Y (bottom)",
                                       "View from +Z (front)", "View from -Z (back)" };
        ImGui::SetTooltip("%s", tips[e.axis * 2 + (e.positive ? 0 : 1)]);
    }
    // A click on an axis looks along it; dragging the gizmo orbits.
    if (ImGui::IsItemDeactivated() && hit >= 0) {
        ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0);
        if (dd.x * dd.x + dd.y * dd.y < 16) {
            const End& e = ends[hit];
            float s = e.positive ? 1.0f : -1.0f;
            if (e.axis == 0) set_view(90 * s, 0);
            else if (e.axis == 1) set_view(camera.yaw, 89.9f * s);
            else set_view(e.positive ? 0 : 180, 0);
        }
    }
    if (active && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
        camera.yaw -= io.MouseDelta.x * 0.4f;
        camera.pitch = fclamp(camera.pitch + io.MouseDelta.y * 0.4f, -89.9f, 89.9f);
    }
    ImGui::EndChild();
}

}  // namespace fe
