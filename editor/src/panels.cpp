// Hierarchy, Inspector, Files, Console and Settings.
#include "editor.h"
#include "icon_glyphs.h"
#include "icons.h"
#include "model_import.h"
#include "widgets.h"
#include "platform/platform.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe {

static const char* kFilePayload = "FE_FILE";   // drag-and-drop payload: a NUL-terminated path

static Icon mesh_icon(MeshKind k);
// The icon of an object: its kind, or for empties the module that says most.
Icon object_icon(const Object& o) {
    switch (o.kind) {
        case ObjectKind::Camera: return Icon::Camera;
        case ObjectKind::Sky: return Icon::Cloud;
        case ObjectKind::SkyLight: return Icon::Sun;
        case ObjectKind::PostProcess: return Icon::Wand;
        case ObjectKind::Empty:
            if (o.modules.has(ModuleKind::Light)) return Icon::Bulb;
            if (o.modules.has(ModuleKind::AudioStream) || o.modules.has(ModuleKind::AudioSampler)) return Icon::Speaker;
            if (o.modules.has(ModuleKind::AudioListener)) return Icon::Ear;
            if (o.modules.has(ModuleKind::ReverbZone)) return Icon::Reverb;
            if (o.modules.has(ModuleKind::ParticleEmitter)) return Icon::Sparks;
            if (o.modules.has(ModuleKind::ForceField)) return Icon::Wind;
            if (o.modules.has(ModuleKind::Spawner)) return Icon::Spawn;
            if (o.modules.has(ModuleKind::Billboard)) return Icon::Sprite;
            if (o.modules.has(ModuleKind::Text3D)) return Icon::TextT;
            if (o.modules.has(ModuleKind::Joint3D)) return Icon::Joint;
            return Icon::Empty;
        default: return mesh_icon(o.mesh);
    }
}

Icon module_icon(ModuleKind k) {
    switch (k) {
        case ModuleKind::RigidBody3D: return Icon::Body;
        case ModuleKind::Collider3D: return Icon::Collider;
        case ModuleKind::CharacterController3D: return Icon::Body;
        case ModuleKind::AudioListener: return Icon::Ear;
        case ModuleKind::AudioStream: return Icon::Wave;
        case ModuleKind::AudioSampler: return Icon::Speaker;
        case ModuleKind::Light: return Icon::Bulb;
        case ModuleKind::ParticleEmitter: return Icon::Sparks;
        case ModuleKind::Joint3D: return Icon::Joint;
        case ModuleKind::Animator: return Icon::Clip;
        case ModuleKind::ForceField: return Icon::Wind;
        case ModuleKind::Spawner: return Icon::Spawn;
        case ModuleKind::ReverbZone: return Icon::Reverb;
        case ModuleKind::Billboard: return Icon::Sprite;
        case ModuleKind::Text3D: return Icon::TextT;
        case ModuleKind::Camera: return Icon::Camera;
        case ModuleKind::SkyLight: return Icon::Sun;
        case ModuleKind::Sky: return Icon::Cloud;
        case ModuleKind::PostProcess: return Icon::Wand;
        default: return Icon::File;
    }
}

static Icon mesh_icon(MeshKind k) {
    switch (k) {
        case MeshKind::Sphere: return Icon::Sphere;
        case MeshKind::Cylinder: return Icon::Cylinder;
        case MeshKind::Plane: return Icon::Plane;
        case MeshKind::Model: return Icon::Model;
        default: return Icon::Cube;
    }
}

static ImU32 dim_text() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }

// Accepts a file dropped on the last item; returns the path or NULL.
static const char* accept_file_drop() {
    const char* path = nullptr;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kFilePayload)) path = (const char*)p->Data;
        ImGui::EndDragDropTarget();
    }
    return path;
}

// ---------------------------------------------------------------- shared

// A text input with a search icon drawn inside, at the left.
static bool search_field(const char* id, const char* hint, char* buf, size_t cap, float width) {
    const Palette& pal = ui_palette();
    float fs = ImGui::GetFontSize();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(fs + 12, ImGui::GetStyle().FramePadding.y));
    bool r = ImGui::InputTextWithHint(id, hint, buf, cap);
    ImGui::PopStyleVar();
    float h = ImGui::GetItemRectSize().y;
    draw_icon(ImGui::GetWindowDrawList(), Icon::Search, ImVec2(p.x + 7, p.y + (h - fs) * 0.5f), fs, pal.text_faint);
    return r;
}

// ---------------------------------------------------------------- Hierarchy

void Editor::panel_hierarchy() {
    ImGui::Begin(FE_ICON_HIERARCHY "  Hierarchy###Hierarchy");
    const float bs = ImGui::GetFrameHeight();
    static char filter[64];
    search_field("##filter", "Filter objects", filter, sizeof(filter), ImGui::GetContentRegionAvail().x - bs - 4);
    track("hier:filter");
    ImGui::SameLine(0, 4);
    if (icon_button(Icon::Plus, "Add an object", false, bs)) ImGui::OpenPopup("##add");
    track("hier:add");
    if (ImGui::BeginPopup("##add")) {
        create_menu_items(kNoObject, camera.target);
        ImGui::EndPopup();
    }
    ImGui::Dummy(ImVec2(0, 2));

    ImGui::BeginChild("##objects");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float fs = ImGui::GetFontSize();
    float row_h = bs + 1;
    static const char* kObjectPayload = "FE_OBJECT";   // a dragged hierarchy row: an ObjectId
    // Reparents the object in a dropped row payload under `parent`.
    auto drop_object = [&](ObjectId parent) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kObjectPayload)) {
            ObjectId id = *(const ObjectId*)pl->Data;
            Object* o = scene.get(id);
            Object* p = scene.get(parent);
            if (!o) return;
            if (scene.set_parent(id, parent)) {
                if (p) { p->expanded = true; log(LogLevel::Info, "%s is now a child of %s", o->name, p->name); }
                else log(LogLevel::Info, "%s is now at the top of the scene", o->name);
            } else {
                log(LogLevel::Warning, "%s cannot go under %s (it is one of its own children)", o->name, p ? p->name : "?");
            }
        }
    };

    // The scene root: dropping a row here takes it out of its parent.
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##root", ImVec2(ImGui::GetContentRegionAvail().x, row_h));
        track("hier:root");
        if (ImGui::IsItemClicked(0)) scene.clear_selection();
        if (ImGui::BeginDragDropTarget()) { drop_object(kNoObject); ImGui::EndDragDropTarget(); }
        draw_icon(dl, Icon::Layers, ImVec2(p.x + 4, p.y + (row_h - fs) * 0.5f), fs, pal.text_dim);
        if (font_medium) ImGui::PushFont(font_medium, 0.0f);
        dl->AddText(ImVec2(p.x + fs + 11, p.y + (row_h - fs) * 0.5f), pal.text, "Scene");
        if (font_medium) ImGui::PopFont();
        char cnt[16];
        snprintf(cnt, sizeof(cnt), "%d", scene.count());
        float cw = ImGui::CalcTextSize(cnt).x;
        dl->AddText(ImVec2(p.x + ImGui::GetContentRegionAvail().x - cw - 6, p.y + (row_h - fs) * 0.5f), pal.text_faint, cnt);
    }

    // The rows to show: the tree (children under their parent, folded ones
    // hidden), or with a filter the matching objects as a flat list.
    struct Row { ObjectId id; int depth; };
    static Row* rows = nullptr;
    static int rows_cap = 0;
    if (rows_cap < scene.count()) {
        rows_cap = scene.count() + 64;
        rows = (Row*)realloc(rows, sizeof(Row) * rows_cap);
    }
    int nrows = 0;
    if (filter[0]) {
        for (int i = 0; i < scene.count(); i++)
            if (ImStristr(scene.at(i).name, nullptr, filter, nullptr)) rows[nrows++] = { scene.at(i).id, 0 };
    } else {
        // Depth-first with an explicit stack; children keep the scene's order.
        struct Frame { ObjectId parent; int next; int depth; };
        Frame stack[64];
        int sp = 0;
        stack[sp++] = { kNoObject, 0, 0 };
        while (sp > 0 && nrows < rows_cap) {
            Frame& f = stack[sp - 1];
            bool pushed = false;
            for (; f.next < scene.count(); f.next++) {
                Object& o = scene.at(f.next);
                ObjectId parent = o.parent;
                if (parent != kNoObject && !scene.get(parent)) parent = kNoObject;     // a lost parent: show at the top
                if (parent != f.parent) continue;
                rows[nrows++] = { o.id, f.depth };
                f.next++;
                if (o.expanded && sp < 64 && scene.has_children(o.id)) {
                    stack[sp++] = { o.id, 0, f.depth + 1 };
                    pushed = true;
                }
                break;
            }
            if (!pushed && f.next >= scene.count()) sp--;
        }
    }

    ObjectId clicked = kNoObject;
    bool shift = ImGui::GetIO().KeyShift;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
    for (int r = 0; r < nrows; r++) {
        Object* op = scene.get(rows[r].id);
        if (!op) continue;
        Object& o = *op;
        int depth = rows[r].depth;
        ImGui::PushID((int)o.id);
        bool sel = scene.is_selected(o.id);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, row_h)))
            clicked = o.id;
        bool hov = ImGui::IsItemHovered();
        char key[32];
        snprintf(key, sizeof(key), "hier:%u", o.id);
        track(key);
        if (hov && ImGui::IsMouseDoubleClicked(0)) frame_selection();
        // Drag a row onto another to make it a child, onto "Scene" to take it out.
        if (ImGui::BeginDragDropSource()) {
            ObjectId id = o.id;
            ImGui::SetDragDropPayload(kObjectPayload, &id, sizeof(id));
            draw_icon(ImGui::GetWindowDrawList(), object_icon(o), ImGui::GetCursorScreenPos(), fs, pal.accent_text);
            ImGui::Dummy(ImVec2(fs + 4, fs));
            ImGui::SameLine();
            ImGui::TextUnformatted(o.name);
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kFilePayload)) apply_file_to_object((const char*)pl->Data, o.id);
            drop_object(o.id);
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("##ctx")) {
            if (!sel) scene.select_only(o.id);
            if (menu_item("Frame", "F")) frame_selection();
            if (menu_item("Duplicate", "Ctrl+D")) duplicate_selection();
            if (menu_item(o.visible ? "Hide" : "Show")) o.visible = !o.visible;
            if (ImGui::BeginMenu("Add child")) {
                create_menu_items(o.id, Vec3(0, 0, 0));
                ImGui::EndMenu();
            }
            if (o.parent && menu_item("Move to the top")) scene.set_parent(o.id, kNoObject);
            ImGui::Separator();
            if (menu_item("Delete", "Del")) delete_selection();
            ImGui::EndPopup();
        }
        // The row: a rounded highlight, the active object marked at the left.
        ImVec2 rmax(pos.x + w, pos.y + row_h);
        if (sel) dl->AddRectFilled(pos, rmax, pal.accent_soft, 4.0f);
        else if (hov) dl->AddRectFilled(pos, rmax, pal.hover, 4.0f);
        if (o.id == scene.active()) dl->AddRectFilled(ImVec2(pos.x, pos.y + 4), ImVec2(pos.x + 2.5f, rmax.y - 4), pal.accent, 1.0f);
        float x0 = pos.x + 6.0f + depth * 16.0f;
        float ty = pos.y + (row_h - fs) * 0.5f;
        // Guide lines for nested rows.
        for (int d = 0; d < depth; d++) {
            float gx = pos.x + 6.0f + d * 16.0f + fs * 0.5f;
            dl->AddLine(ImVec2(gx, pos.y), ImVec2(gx, rmax.y + 1), pal.border, 1.0f);
        }
        bool kids = !filter[0] && scene.has_children(o.id);
        float icon_x = x0 + fs + 2;
        bool shown = scene.visible_in_tree(o);
        ImU32 icol = sel ? pal.accent_text : (shown ? pal.text_dim : pal.text_faint);
        draw_icon(dl, object_icon(o), ImVec2(icon_x, ty), fs, icol);
        dl->AddText(ImVec2(icon_x + fs + 7, ty), shown ? pal.text : pal.text_faint, o.name);
        // A star after the name of a current camera, sky, listener ...
        {
            bool cur = false;
            for (int k = 0; k < kModuleKindCount && !cur; k++) cur = scene.is_current(o.id, (ModuleKind)k);
            if (cur) {
                float nx = icon_x + fs + 7 + ImGui::CalcTextSize(o.name).x + 6;
                draw_icon(dl, Icon::Star, ImVec2(nx, ty + fs * 0.1f), fs * 0.8f, pal.warning);
                snprintf(key, sizeof(key), "current:%u", o.id);
                track_rect(key, nx, ty, fs * 0.8f, fs * 0.8f);
            }
        }
        // Fold arrow for objects with children.
        if (kids) {
            ImGui::SameLine(0, 0);
            ImGui::SetCursorScreenPos(ImVec2(x0 - 2, pos.y));
            if (ImGui::InvisibleButton("##fold", ImVec2(fs + 4, row_h))) o.expanded = !o.expanded;
            snprintf(key, sizeof(key), "fold:%u", o.id);
            track(key);
            bool fh = ImGui::IsItemHovered();
            draw_icon(dl, o.expanded ? Icon::ChevronDown : Icon::ChevronRight, ImVec2(x0, ty), fs, fh ? pal.text : pal.text_faint);
        }
        // Visibility, at the right end of the row.
        float eb = row_h - 2;
        ImGui::SameLine(0, 0);
        ImGui::SetCursorScreenPos(ImVec2(rmax.x - eb - 2, pos.y + 1));
        // Shown on the hovered and selected rows, and always for hidden objects.
        bool eye_hover = ImGui::IsMouseHoveringRect(ImVec2(rmax.x - eb - 2, pos.y), rmax);
        ImU32 eye_col = !o.visible ? pal.text_dim : ((hov || sel || eye_hover) ? pal.text_dim : 0x00000000u);
        if (icon_button(o.visible ? Icon::Eye : Icon::EyeOff, o.visible ? "Hide" : "Show", false, eb, eye_col ? eye_col : 0x00FFFFFFu))
            o.visible = !o.visible;
        snprintf(key, sizeof(key), "eye:%u", o.id);
        track(key);
        ImGui::SetCursorScreenPos(ImVec2(pos.x, rmax.y + 1));
        ImGui::PopID();
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    if (clicked != kNoObject) {
        if (shift) scene.toggle(clicked);
        else scene.select_only(clicked);
    }
    // Empty area: click clears, right-click creates.
    ImGui::InvisibleButton("##empty", ImVec2(-FLT_MIN, fmaxf_(ImGui::GetContentRegionAvail().y, 40)));
    if (ImGui::IsItemClicked(0) && !shift) scene.clear_selection();
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kFilePayload)) {
            const char* path = (const char*)pl->Data;
            if (path_is_model(path)) {
                ObjectId id = create_object(path_filename(path), MeshKind::Model, camera.target);
                apply_file_to_object(path, id);
            }
        }
        drop_object(kNoObject);
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("##create")) {
        ImGui::TextDisabled("Add");
        create_menu_items(kNoObject, Vec3(0, 0, 0));
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------- Inspector

// X/Y/Z drag fields, each with its axis letter in the axis color.
static bool vec3_field(const char* label, Vec3* v, float speed, const char* fmt) {
    const Palette& pal = ui_palette();
    bool changed = false;
    ImGui::PushID(label);
    prop_label(label);
    float w = (ImGui::GetContentRegionAvail().x - 8) / 3.0f;
    static const char* letters[3] = { "X", "Y", "Z" };
    float* f = &v->x;
    float fs = ImGui::GetFontSize();
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine(0, 4);
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        // Narrow fields (small screens) keep only the colored bar: the
        // value is centered and would run into the letter.
        bool letter = w >= fs * 3.6f;
        ImGui::SetNextItemWidth(w);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(letter ? fs * 0.9f : 4.0f, ImGui::GetStyle().FramePadding.y));
        changed |= ImGui::DragFloat("##v", &f[i], speed, 0, 0, fmt);
        ImGui::PopStyleVar();
        float h = ImGui::GetFrameHeight();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + 2.0f, p.y + h), pal.axis[i], 1.0f);
        if (letter) dl->AddText(ImVec2(p.x + 6, p.y + (h - fs) * 0.5f), pal.axis[i], letters[i]);
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

// A file slot: shows the file (or "None"), takes a file dropped on it.
static void slot_field(const char* label, char* path, size_t cap, Editor* ed, bool (*accepts)(const char*), Icon icon) {
    const Palette& pal = ui_palette();
    ImGui::PushID(label);
    prop_label(label);
    float bs = ImGui::GetFrameHeight();
    float w = ImGui::GetContentRegionAvail().x - (path[0] ? bs + 4 : 0);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##slot", ImVec2(w, bs));
    bool hov = ImGui::IsItemHovered();
    bool target = ImGui::GetDragDropPayload() != nullptr;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + bs), hov ? pal.hover : pal.frame, 4.0f);
    if (target) dl->AddRect(p, ImVec2(p.x + w, p.y + bs), pal.accent, 4.0f, 0, 1.0f);
    float fs = ImGui::GetFontSize();
    draw_icon(dl, icon, ImVec2(p.x + 7, p.y + (bs - fs) * 0.5f), fs, path[0] ? pal.accent_text : pal.text_faint);
    dl->PushClipRect(p, ImVec2(p.x + w - 4, p.y + bs), true);
    dl->AddText(ImVec2(p.x + fs + 13, p.y + (bs - fs) * 0.5f), path[0] ? pal.text : pal.text_faint,
                path[0] ? path_filename(path) : "None (drop a file)");
    dl->PopClipRect();
    if (path[0] && hov) ImGui::SetTooltip("%s", path);
    if (const char* d = accept_file_drop()) {
        if (accepts(d)) snprintf(path, cap, "%s", d);
        else ed->log(LogLevel::Warning, "%s does not fit the %s slot", path_filename(d), label);
    }
    if (path[0]) {
        ImGui::SameLine(0, 4);
        if (icon_button(Icon::Close, "Clear", false, bs)) path[0] = 0;
    }
    ImGui::PopID();
}

// Whether a field means something with the other settings as they are
// (a spot angle only for spot lights, a radius only for round colliders).
static bool g_playing_inspector = false;     // live (read-only) fields show only while playing

static bool field_relevant(ModuleKind kind, const void* data, const FieldInfo& f) {
    const char* n = f.name;
    switch (kind) {
        case ModuleKind::ParticleEmitter: {
            const ParticleEmitter& e = *(const ParticleEmitter*)data;
            if (!strcmp(n, "particle_count") || !strcmp(n, "playing")) return g_playing_inspector;
            if (!strcmp(n, "shape_radius")) return e.shape == EmitShape::Sphere || e.shape == EmitShape::Cone;
            if (!strcmp(n, "box_size")) return e.shape == EmitShape::Box;
            if (!strcmp(n, "cone_angle")) return e.shape == EmitShape::Cone;
            if (!strcmp(n, "burst_interval")) return e.burst_count > 0;
            if (!strcmp(n, "duration")) return !e.looping;
            if (!strcmp(n, "prewarm")) return e.looping;
            return true;
        }
        case ModuleKind::Joint3D: {
            const Joint3D& j = *(const Joint3D*)data;
            bool hinge = j.type == JointType::Hinge;
            if (!strcmp(n, "axis") || !strcmp(n, "use_limits") || !strcmp(n, "use_motor")) return hinge;
            if (!strcmp(n, "limit_min") || !strcmp(n, "limit_max")) return hinge && j.use_limits;
            if (!strcmp(n, "motor_speed") || !strcmp(n, "motor_torque")) return hinge && j.use_motor;
            if (!strcmp(n, "rest_length") || !strcmp(n, "stiffness") || !strcmp(n, "damping")) return j.type == JointType::Spring;
            if (!strcmp(n, "angle")) return g_playing_inspector && hinge;
            if (!strcmp(n, "force") || !strcmp(n, "broken")) return g_playing_inspector;
            return true;
        }
        case ModuleKind::Animator: {
            if (!strcmp(n, "playing") || !strcmp(n, "time") || !strcmp(n, "duration")) return g_playing_inspector;
            return true;
        }
        case ModuleKind::ForceField: {
            const ForceField& ff = *(const ForceField*)data;
            if (!strcmp(n, "size")) return ff.shape == FieldShape::Box;
            if (!strcmp(n, "radius")) return ff.shape == FieldShape::Sphere;
            if (!strcmp(n, "direction")) return ff.mode == FieldMode::Directional;
            if (!strcmp(n, "turbulence_scale") || !strcmp(n, "turbulence_speed")) return ff.turbulence > 0;
            return true;
        }
        case ModuleKind::Spawner: {
            if (!strcmp(n, "spawned") || !strcmp(n, "alive")) return g_playing_inspector;
            return true;
        }
        case ModuleKind::ReverbZone: {
            const ReverbZone& r = *(const ReverbZone*)data;
            if (!strcmp(n, "size")) return r.shape == FieldShape::Box;
            if (!strcmp(n, "radius")) return r.shape == FieldShape::Sphere;
            return true;
        }
        case ModuleKind::Light: {
            const Light& l = *(const Light*)data;
            if (!strcmp(n, "range")) return l.type != LightType::Directional;
            if (!strcmp(n, "spot_angle") || !strcmp(n, "spot_blend")) return l.type == LightType::Spot;
            return true;
        }
        case ModuleKind::Collider3D: {
            const Collider3D& c = *(const Collider3D*)data;
            if (!strcmp(n, "size")) return c.shape == ColliderShape::Box;
            bool round = c.shape == ColliderShape::Capsule || c.shape == ColliderShape::Cylinder;
            if (!strcmp(n, "radius")) return c.shape == ColliderShape::Sphere || round;
            if (!strcmp(n, "height")) return round;
            if (!strcmp(n, "friction") || !strcmp(n, "bounciness")) return c.mode == ColliderMode::Collision;
            return true;
        }
        case ModuleKind::RigidBody3D: {
            const RigidBody3D& b = *(const RigidBody3D*)data;
            if (b.type == BodyType::Static) return !strcmp(n, "type");
            if (!strcmp(n, "gravity_scale")) return b.use_gravity && b.type == BodyType::Dynamic;
            return true;
        }
        case ModuleKind::Camera: {
            const Camera& c = *(const Camera*)data;
            if (!strcmp(n, "fov")) return c.projection == Projection::Perspective;
            if (!strcmp(n, "ortho_size")) return c.projection == Projection::Orthographic;
            return true;
        }
        case ModuleKind::Sky: {
            const Sky& sk = *(const Sky*)data;
            if (sk.mode == SkyMode::Color) return strcmp(n, "horizon") && strcmp(n, "ground") && strcmp(n, "sun_size");
            return true;
        }
        case ModuleKind::AudioStream:
        case ModuleKind::AudioSampler: {
            bool spatial = kind == ModuleKind::AudioStream ? ((const AudioStream*)data)->spatial : ((const AudioSampler*)data)->spatial;
            if (!strcmp(n, "min_distance") || !strcmp(n, "max_distance")) return spatial;
            return true;
        }
        default: return true;
    }
}

static bool path_any(const char*) { return true; }

// One module (or a special object's settings): its fields, drawn from the
// reflection table, and a remove button for modules.
bool Editor::module_section(Object* o, ModuleKind kind) {
    const Palette& pal = ui_palette();
    const ModuleInfo& mi = module_info(kind);
    void* data = o->modules.data(kind);
    char title[96];
    snprintf(title, sizeof(title), "%s##module%d", mi.label, (int)kind);
    char key[64];
    snprintf(key, sizeof(key), "module:%s", mi.name);
    bool open = section_header(title, module_icon(kind));
    track(key);
    if (!open) return true;
    g_playing_inspector = playing();
    ImGui::PushID(title);
    float bs = ImGui::GetFrameHeight();
    if (is_module(kind)) {
        // Remove, at the right of a short description line.
        static const char* about[] = {
            "Physics: gravity, velocity, collisions.", "A shape that collides or triggers.", "An upright capsule driven by velocity.",
            "Where the game hears from.", "A long sound streamed from a file.", "Short sounds played on demand.", "Lights the scene.",
            "Particles: sparks, smoke, fire, rain.", "Ties this body to another body or the world.", "Plays a clip from an animation file.",
            "Wind, fans and vortices that push things.", "Makes copies of another object.", "Echo and muffling inside a volume.",
            "A picture that always faces the camera.", "A text label in the world.",
        };
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.text_faint));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(about[(int)kind]);
        ImGui::PopStyleColor();
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - bs);
        if (icon_button(Icon::Trash, "Remove the module", false, bs)) {
            log(LogLevel::Info, "%s: removed %s", o->name, mi.label);
            scene.remove_module(o->id, kind);
            ImGui::PopID();
            return false;
        }
        snprintf(key, sizeof(key), "module:%s:remove", mi.name);
        track(key);
        if (kind == ModuleKind::Collider3D) {
            // Shape the collider in the viewport by dragging handles.
            prop_label("Edit shape");
            toggle_switch("##edit_shape", &edit_collider);
            track("module:collider3d:edit");
            if (edit_collider) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.text_faint));
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("drag the dots");
                ImGui::PopStyleColor();
            }
        }
        if (kind == ModuleKind::CharacterController3D && o->modules.has(ModuleKind::RigidBody3D)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.warning));
            ImGui::TextWrapped("This controller drives the body; Rigidbody settings are ignored while it is attached.");
            ImGui::PopStyleColor();
        }
        if (kind == ModuleKind::CharacterController3D && o->modules.has(ModuleKind::Collider3D)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.text_faint));
            ImGui::TextWrapped("The controller supplies a solid capsule; Collider shape and mode are ignored, but friction, bounciness and layers apply.");
            ImGui::PopStyleColor();
        }
    }
    for (int i = 0; i < mi.field_count; i++) {
        const FieldInfo& f = mi.fields[i];
        if (!field_relevant(kind, data, f)) continue;
        void* fp = (char*)data + f.offset;
        ImGui::PushID(i);
        snprintf(key, sizeof(key), "module:%s:%s", mi.name, f.name);
        bool tracked = false;
        switch (f.type) {
            case FieldType::Float: {
                prop_label(f.label);
                float* v = (float*)fp;
                if (f.min < f.max && f.max - f.min <= 20.0f) ImGui::SliderFloat("##v", v, f.min, f.max, "%.2f");
                else if (ImGui::DragFloat("##v", v, 0.05f, f.min, f.max, "%.2f", f.min < f.max ? ImGuiSliderFlags_AlwaysClamp : 0)) {}
                break;
            }
            case FieldType::Int: {
                prop_label(f.label);
                int* v = (int*)fp;
                if (f.min < f.max) ImGui::SliderInt("##v", v, (int)f.min, (int)f.max);
                else ImGui::DragInt("##v", v);
                break;
            }
            case FieldType::Bool: {
                prop_label(f.label);
                bool* v = (bool*)fp;
                if (!strcmp(f.name, "current")) {
                    // Only one current per kind: turning it on takes it from the others.
                    bool on = *v;
                    bool flipped = toggle_switch("##v", &on);
                    track(key);
                    tracked = true;
                    if (flipped) {
                        if (on) {
                            ObjectId was = scene.current(kind);
                            scene.set_current(o->id, kind);
                            Object* w = scene.get(was);
                            if (w && was != o->id) log(LogLevel::Info, "%s is now the current %s (was %s)", o->name, mi.label, w->name);
                        } else {
                            scene.clear_current(o->id, kind);
                        }
                    }
                    if (!on) {
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.text_faint));
                        ImGui::AlignTextToFramePadding();
                        Object* c = scene.get(scene.current(kind));
                        if (c) ImGui::Text("(now: %s)", c->name);
                        else ImGui::TextUnformatted("(none)");
                        ImGui::PopStyleColor();
                    }
                } else {
                    toggle_switch("##v", v);
                }
                break;
            }
            case FieldType::Enum: {
                prop_label(f.label);
                uint8_t* v = (uint8_t*)fp;
                int count = 0;
                while (f.enum_names[count]) count++;
                int cur = *v < count ? *v : 0;
                if (ImGui::Combo("##v", &cur, f.enum_names, count)) *v = (uint8_t)cur;
                break;
            }
            case FieldType::Color:
                prop_label(f.label);
                ImGui::ColorEdit3("##v", (float*)fp, ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_DisplayHex);
                break;
            case FieldType::Vec3:
                vec3_field(f.label, (Vec3*)fp, 0.05f, "%.2f");
                break;
            case FieldType::Path:
                slot_field(f.label, (char*)fp, kPathMax, this, path_any, Icon::Wave);
                break;
            case FieldType::Bits: {
                prop_label(f.label);
                ImGui::InputScalar("##v", ImGuiDataType_U32, fp, nullptr, nullptr, "%08X", ImGuiInputTextFlags_CharsHexadecimal);
                break;
            }
            case FieldType::Text:
                prop_label(f.label);
                ImGui::InputText("##v", (char*)fp, kPathMax);
                break;
            case FieldType::Object: {
                // A link to another object: pick from the list or drop a hierarchy row.
                prop_label(f.label);
                ObjectId* link = (ObjectId*)fp;
                Object* t = scene.get(*link);
                if (t && t == o) { *link = kNoObject; t = nullptr; }
                float lw = ImGui::GetContentRegionAvail().x - (t ? bs + 4 : 0);
                ImGui::SetNextItemWidth(lw);
                const char* none = !strcmp(f.name, "connected") ? "The world (drop an object)" : "None (drop an object)";
                if (!t) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.text_faint));
                bool combo = ImGui::BeginCombo("##v", t ? t->name : none);
                if (!t) ImGui::PopStyleColor();
                if (combo) {
                    if (ImGui::Selectable("None", !t)) *link = kNoObject;
                    for (int oi = 0; oi < scene.count(); oi++) {
                        Object& c = scene.at(oi);
                        if (c.id == o->id) continue;
                        ImGui::PushID((int)c.id);
                        if (ImGui::Selectable(c.name, c.id == *link)) *link = c.id;
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("FE_OBJECT")) {
                        ObjectId dropped = *(const ObjectId*)pl->Data;
                        if (dropped != o->id) *link = dropped;
                    }
                    ImGui::EndDragDropTarget();
                }
                if (t) {
                    ImGui::SameLine(0, 4);
                    if (icon_button(Icon::Pointer, "Select it", false, bs)) {
                        ObjectId target = t->id;
                        ImGui::PopID();
                        ImGui::PopID();
                        scene.select_only(target);
                        return false;
                    }
                }
                break;
            }
        }
        if (!tracked) track(key);
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PopID();
    return true;
}

// "Add module": a menu of the modules the object does not have yet.
void Editor::add_module_button(Object* o) {
    const Palette& pal = ui_palette();
    float bs = ImGui::GetFrameHeight();
    ImGui::Dummy(ImVec2(0, 2));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    bool clicked = ImGui::InvisibleButton("##add_module", ImVec2(w, bs + 4));
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + bs + 4), pal.hover, 4.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + bs + 4), hov ? pal.text_faint : pal.border, 4.0f, 0, 1.0f);
    float fs = ImGui::GetFontSize();
    const char* label = "Add module";
    float tw = ImGui::CalcTextSize(label).x;
    float x = p.x + (w - tw - fs - 6) * 0.5f;
    draw_icon(dl, Icon::Plus, ImVec2(x, p.y + (bs + 4 - fs) * 0.5f), fs, pal.accent_text);
    dl->AddText(ImVec2(x + fs + 6, p.y + (bs + 4 - fs) * 0.5f), pal.text_dim, label);
    track("inspector:add_module");
    if (clicked) ImGui::OpenPopup("##add_module_menu");
    if (ImGui::BeginPopup("##add_module_menu")) {
        static const struct { const char* group; ModuleKind kind; } menu[] = {
            { "Physics", ModuleKind::RigidBody3D }, { nullptr, ModuleKind::Collider3D }, { nullptr, ModuleKind::CharacterController3D },
            { nullptr, ModuleKind::Joint3D }, { nullptr, ModuleKind::ForceField },
            { "Audio", ModuleKind::AudioListener }, { nullptr, ModuleKind::AudioStream }, { nullptr, ModuleKind::AudioSampler },
            { nullptr, ModuleKind::ReverbZone },
            { "Rendering", ModuleKind::Light }, { nullptr, ModuleKind::ParticleEmitter }, { nullptr, ModuleKind::Billboard },
            { nullptr, ModuleKind::Text3D },
            { "Gameplay", ModuleKind::Animator }, { nullptr, ModuleKind::Spawner },
        };
        bool first_group = true;
        for (auto& it : menu) {
            ModuleKind mk = it.kind;
            if (it.group) { if (!first_group) ImGui::Separator(); first_group = false; ImGui::TextDisabled("%s", it.group); }
            const ModuleInfo& mi = module_info(mk);
            bool have = o->modules.has(mk);
            ImVec2 ip = ImGui::GetCursorScreenPos();
            ImGui::BeginDisabled(have);
            char item[64];
            snprintf(item, sizeof(item), "      %s%s", mi.label, have ? "  (added)" : "");
            if (ImGui::Selectable(item)) {
                scene.add_module(o->id, mk);
                log(LogLevel::Info, "%s: added %s", o->name, mi.label);
            }
            ImGui::EndDisabled();
            draw_icon(ImGui::GetWindowDrawList(), module_icon(mk), ImVec2(ip.x + 2, ip.y + 1), fs * 0.95f, have ? pal.text_faint : pal.accent_text);
            char k2[64];
            snprintf(k2, sizeof(k2), "addmodule:%s", mi.name);
            track(k2);
        }
        ImGui::EndPopup();
    }
}

// One script on the object: its file, buttons, and the inputs it declares.
void Editor::script_section(Object* o, int si) {
    const Palette& pal = ui_palette();
    ScriptSlot& sl = o->scripts[si];
    char title[300];
    snprintf(title, sizeof(title), "%s##script%d", sl.name(), si);
    char key[48];
    snprintf(key, sizeof(key), "script:%d", si);
    bool open = section_header(title, Icon::Script);
    track(key);
    if (!open) return;
    ImGui::PushID(si);
    float bs = ImGui::GetFrameHeight();
    // Buttons: open, read again, move up / down, remove.
    {
        float w = ImGui::GetContentRegionAvail().x - (bs + 4) * 4;
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(sl.missing ? pal.warning : pal.text_dim));
        ImGui::AlignTextToFramePadding();
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushClipRect(p, ImVec2(p.x + w, p.y + bs), true);
        ImGui::TextUnformatted(sl.missing ? "missing file" : sl.path);
        ImGui::PopClipRect();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sl.path);
        ImGui::SameLine(0, 0);
        ImGui::SetCursorScreenPos(ImVec2(p.x + w + 4, p.y));
    }
    if (icon_button(Icon::File, "Open in editor", false, bs)) open_file(sl.path);
    ImGui::SameLine(0, 4);
    if (icon_button(Icon::Refresh, "Read the inputs again", false, bs)) reload_script(o->id, si);
    snprintf(key, sizeof(key), "script:%d:reload", si);
    track(key);
    ImGui::SameLine(0, 4);
    ImGui::BeginDisabled(si == 0);
    if (icon_button(Icon::Up, "Run earlier", false, bs)) {
        scene.move_script(o->id, si, si - 1);
        ImGui::EndDisabled();
        ImGui::PopID();
        return;
    }
    snprintf(key, sizeof(key), "script:%d:up", si);
    track(key);
    ImGui::EndDisabled();
    ImGui::SameLine(0, 4);
    if (icon_button(Icon::Trash, "Remove the script", false, bs)) {
        log(LogLevel::Info, "%s: removed script %s", o->name, sl.name());
        scene.remove_script(o->id, si);
        ImGui::PopID();
        return;
    }
    snprintf(key, sizeof(key), "script:%d:remove", si);
    track(key);

    if (sl.prop_count == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.text_faint));
        ImGui::TextWrapped("No inputs. A script declares them with lines like  -- @prop speed number = 5");
        ImGui::PopStyleColor();
    }
    for (int k = 0; k < sl.prop_count; k++) {
        ScriptProp& p = sl.props[k];
        ImGui::PushID(k);
        prop_label(p.name);
        switch (p.type) {
            case PropType::Number:
                if (p.min < p.max) ImGui::SliderFloat("##v", &p.number, p.min, p.max, "%.2f");
                else ImGui::DragFloat("##v", &p.number, 0.05f, 0, 0, "%.2f");
                break;
            case PropType::Text:
                ImGui::InputText("##v", p.text, sizeof(p.text));
                break;
            case PropType::Toggle:
                toggle_switch("##v", &p.toggle);
                break;
            case PropType::List:
                if (ImGui::BeginCombo("##v", p.choice < p.choice_count ? p.choices[p.choice] : "")) {
                    for (int c = 0; c < p.choice_count; c++)
                        if (ImGui::Selectable(p.choices[c], c == p.choice)) p.choice = c;
                    ImGui::EndCombo();
                }
                break;
            case PropType::Object: {
                // A link to another object: pick it from the list or drop a
                // hierarchy row here. Its scripts are what this script talks to.
                Object* t = scene.get(p.object);
                float w = ImGui::GetContentRegionAvail().x - (t ? bs + 4 : 0);
                ImGui::SetNextItemWidth(w);
                if (!t) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.text_faint));
                bool combo = ImGui::BeginCombo("##v", t ? t->name : "None (drop an object)");
                if (!t) ImGui::PopStyleColor();
                if (combo) {
                    if (ImGui::Selectable("None", !t)) p.object = kNoObject;
                    for (int i = 0; i < scene.count(); i++) {
                        Object& c = scene.at(i);
                        ImGui::PushID((int)c.id);
                        char label[96];
                        if (c.script_count) snprintf(label, sizeof(label), "%s   (%d script%s)", c.name, c.script_count, c.script_count == 1 ? "" : "s");
                        else snprintf(label, sizeof(label), "%s", c.name);
                        if (ImGui::Selectable(label, c.id == p.object)) p.object = c.id;
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("FE_OBJECT")) p.object = *(const ObjectId*)pl->Data;
                    ImGui::EndDragDropTarget();
                }
                if (t && ImGui::IsItemHovered() && t->script_count) {
                    char tip[400];
                    int n = snprintf(tip, sizeof(tip), "Scripts on %s:", t->name);
                    for (int s2 = 0; s2 < t->script_count && n < (int)sizeof(tip) - 2; s2++)
                        n += snprintf(tip + n, sizeof(tip) - n, "\n  %s", t->scripts[s2].name());
                    ImGui::SetTooltip("%s", tip);
                }
                if (t) {
                    ImGui::SameLine(0, 4);
                    if (icon_button(Icon::Pointer, "Select it", false, bs)) {
                        ObjectId target = t->id;
                        ImGui::PopID();
                        ImGui::PopID();
                        scene.select_only(target);
                        return;
                    }
                }
                break;
            }
            case PropType::File:
                slot_field(p.name, p.file, sizeof(p.file), this, path_any, Icon::Wave);
                break;
        }
        char pk[48];
        snprintf(pk, sizeof(pk), "script:%d:%s", si, p.name);
        track(pk);
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PopID();
}

namespace {
struct ScriptList {
    char paths[64][256];
    int count = 0;
};
void collect_scripts(const platform::DirEntry& e, void* user) {
    ScriptList* l = (ScriptList*)user;
    if (!e.is_dir && path_is_script(e.name) && l->count < 64) snprintf(l->paths[l->count++], 256, "%s", e.name);
}
}  // namespace

// "Add script": a button that lists the project's scripts and takes a
// script dropped from Files.
void Editor::add_script_button(Object* o) {
    const Palette& pal = ui_palette();
    float bs = ImGui::GetFrameHeight();
    ImGui::Dummy(ImVec2(0, 2));
    bool full = o->script_count >= kMaxScripts;
    ImGui::BeginDisabled(full);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    bool clicked = ImGui::InvisibleButton("##add_script", ImVec2(w, bs + 4));
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool target = ImGui::GetDragDropPayload() != nullptr;
    dl->AddRect(p, ImVec2(p.x + w, p.y + bs + 4), target ? pal.accent : (hov ? pal.text_faint : pal.border), 4.0f, 0, 1.0f);
    if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + bs + 4), pal.hover, 4.0f);
    float fs = ImGui::GetFontSize();
    const char* label = full ? "Script slots full" : "Add script";
    float tw = ImGui::CalcTextSize(label).x;
    float x = p.x + (w - tw - fs - 6) * 0.5f;
    draw_icon(dl, Icon::Plus, ImVec2(x, p.y + (bs + 4 - fs) * 0.5f), fs, pal.accent_text);
    dl->AddText(ImVec2(x + fs + 6, p.y + (bs + 4 - fs) * 0.5f), pal.text_dim, label);
    track("inspector:add_script");
    if (const char* d = accept_file_drop()) {
        if (path_is_script(d)) apply_file_to_object(d, o->id);
        else log(LogLevel::Warning, "%s is not a script", path_filename(d));
    }
    ImGui::EndDisabled();
    if (clicked) ImGui::OpenPopup("##add_script_menu");
    if (ImGui::BeginPopup("##add_script_menu")) {
        // Scripts in the project folder and its scripts/ folder.
        static ScriptList list;
        if (ImGui::IsWindowAppearing()) {
            list.count = 0;
            const char* dirs[2] = { project_dir, nullptr };
            char sub[300];
            snprintf(sub, sizeof(sub), "%s/scripts", project_dir);
            dirs[1] = sub;
            for (int d = 0; d < 2; d++) {
                int before = list.count;
                if (!dirs[d][0]) continue;
                platform::list_dir(dirs[d], collect_scripts, &list);
                for (int i = before; i < list.count; i++) {
                    char full[256];
                    snprintf(full, sizeof(full), "%s/%s", dirs[d], list.paths[i]);
                    memcpy(list.paths[i], full, sizeof(full));
                }
            }
        }
        if (!list.count) ImGui::TextDisabled("No scripts in the project (drop one from Files)");
        for (int i = 0; i < list.count; i++) {
            ImGui::PushID(i);
            if (ImGui::Selectable(path_filename(list.paths[i]))) apply_file_to_object(list.paths[i], o->id);
            char k[48];
            snprintf(k, sizeof(k), "addscript:%s", path_filename(list.paths[i]));
            track(k);
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
}

// Settings > Graphics: FarcadeGL quality. The GPU effects run only when
// FarcadeGL is on a graphics card, unless forced on the CPU.
void Editor::settings_graphics() {
    const Palette& pal = ui_palette();
    gfx::GraphicsSettings& g = settings.graphics;
    bool gpu = fgl_features.gpu;
    ImGui::Dummy(ImVec2(0, 4));
    auto faint = [&](const char* text) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.text_faint));
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    };
    if (section_header("Device", Icon::Monitor)) {
        prop_label("FarcadeGL on");
        ImGui::AlignTextToFramePadding();
        if (fgl_backend[0]) ImGui::Text("%s  (%s)", fgl_backend, gpu ? "graphics card" : "CPU");
        else ImGui::TextDisabled("not started yet: switch the viewport to FarcadeGL");
        static const char* levels[] = { "Auto", "FL0", "FL1-ready", "GPU tier" };
        int lm = (int)g.feature_level;
        prop_label("Feature level");
        if (ImGui::Combo("##flevel", &lm, levels, 4)) g.feature_level = (gfx::FeatureLevelMode)lm;
        track("gfx:level");
        if (fgl_backend[0]) {
            prop_label("  Running");
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s%s%s", gfx::feature_level_name(fgl_features.level), fgl_features.level_forced ? " (forced)" : "",
                        fgl_features.level_clamped ? " (the device lacks what the request needs)" : "");
            const gfx::PassCaps& pc = fgl_features.pass_caps;
            char caps[96];
            snprintf(caps, sizeof(caps), "Device: scissor %s, mipmaps %s, 16 texture units %s, multiple targets %s",
                     pc.scissor ? "yes" : "no", pc.mipmaps ? "yes" : "no", pc.tex16 ? "yes" : "no", pc.mrt ? "yes" : "no");
            faint(caps);
        }
        ImGui::Dummy(ImVec2(0, 4));
    }
    if (section_header("Effects", Icon::Sparks)) {
        prop_label("Particles");
        toggle_switch("##fxp", &g.fx_particles);
        track("gfx:particles");
        prop_label("Density");
        ImGui::SliderFloat("##fxd", &g.fx_particle_density, 0.1f, 1.0f, "%.2f");
        track("gfx:particle_density");
        prop_label("Hidden by meshes");
        toggle_switch("##fxs", &g.fx_depth_test);
        prop_label("Billboards and text");
        toggle_switch("##fxb", &g.fx_sprites);
        prop_label("Audio reverb zones");
        toggle_switch("##fxr", &g.fx_reverb);
        faint("Density scales every emitter's rate and max particles; the depth test is used by the FarcadeGL renderer and costs a coarse depth map.");
        ImGui::Dummy(ImVec2(0, 4));
    }
    if (section_header("Quality", Icon::Sun)) {
        static const char* presets[] = { "Low", "Medium", "High", "Ultra", "Custom" };
        int p = (int)g.preset;
        prop_label("Preset");
        if (ImGui::Combo("##preset", &p, presets, 5)) g.apply_preset((gfx::GraphicsPreset)p);
        track("gfx:preset");
        prop_label("Render scale");
        ImGui::SliderFloat("##rscale", &g.render_scale, 0.25f, 1.0f, "%.2f");
        track("gfx:rscale");
        prop_label("Dynamic resolution");
        toggle_switch("##dynres", &g.dynamic_resolution);
        track("gfx:dynres");
        if (g.dynamic_resolution) {
            prop_label("  Target");
            ImGui::SliderFloat("##dynms", &g.target_frame_ms, 8.0f, 100.0f, "%.0f ms");
            prop_label("  Lowest scale");
            ImGui::SliderFloat("##dynmin", &g.min_render_scale, 0.25f, 1.0f, "%.2f");
        }
        {
            using namespace gfx;
            static const char* gi_modes[] = { "Hemisphere ambient", "Probe GI" };
            static const char* gi_density[] = { "Coarse (2 m)", "Normal (1 m)", "Fine (0.5 m)" };
            static const char* gi_update[] = { "Slow", "Normal", "Fast" };
            int gm = (int)g.gi_mode, gd = (int)g.gi_density, gu = (int)g.gi_update;
            bool gchanged = false;
            prop_label("Global illumination");
            if (ImGui::Combo("##gimode", &gm, gi_modes, 2)) { g.gi_mode = (GiMode)gm; gchanged = true; }
            track("gfx:gi");
            if (g.gi_mode == GiMode::Probes) {
                prop_label("  Probe density");
                if (ImGui::Combo("##gidens", &gd, gi_density, 3)) { g.gi_density = (GiDensity)gd; gchanged = true; }
                prop_label("  Update rate");
                if (ImGui::Combo("##giupd", &gu, gi_update, 3)) { g.gi_update = (GiUpdate)gu; gchanged = true; }
                faint("Voxels built from the scene and light probes updated on the CPU: bounce light and colour "
                      "bleeding on every feature level, a few ms of one core. Strength and bounce are on the Sky Light.");
            }
            if (gchanged) g.preset = GraphicsPreset::Custom;
        }
        faint("Fog is set on the Sky object.");
        ImGui::Dummy(ImVec2(0, 4));
    }
    if (section_header("GPU effects", Icon::Wand)) {
        bool active = gpu || g.allow_gpu_effects_on_cpu;
        if (!gpu) {
            faint("These run when FarcadeGL is on a graphics card (the kernel's NVIDIA backend, RTX 20 / GTX 16). "
                  "This machine draws on the CPU, so they are off unless forced (slow).");
            prop_label("Use on the CPU");
            toggle_switch("##force", &g.allow_gpu_effects_on_cpu);
            track("gfx:force");
        }
        ImGui::BeginDisabled(!active);
        bool changed = false;
        // A combo over the named choices of a graphics property (graphics_settings.h); the older, cheaper
        // technique stays in the list next to the newer ones.
        auto choice = [&](const char* label, const char* id, const char* prop, const char* const* shown) {
            using namespace gfx;
            int pi = graphics_property_find(prop);
            const GraphicsProperty& gp = graphics_property(pi);
            char cur[40];
            graphics_get_text(g, prop, cur, sizeof cur);
            int ci = 0, n = 0;
            for (; gp.choices[n]; n++) if (!strcmp(gp.choices[n], cur)) ci = n;
            prop_label(label);
            if (ImGui::Combo(id, &ci, shown, n)) { graphics_set_text(&g, prop, gp.choices[ci]); changed = true; }
            track(id);
        };
        static const char* shadow_modes[] = { "Off", "Single map", "Cascades" };
        choice("Shadows", "##shadowmode", "shadows", shadow_modes);
        if (g.shadows) {
            if (g.shadow_cascades >= 2) {
                prop_label("  Cascades");
                changed |= ImGui::SliderInt("##casc", &g.shadow_cascades, 2, 3);
                prop_label("  Cache far");
                changed |= toggle_switch("##casccache", &g.cascade_cache);
            }
            static const int sizes[] = { 512, 1024, 2048, 4096 };
            static const char* size_names[] = { "512", "1024", "2048", "4096" };
            int si = 0;
            for (int i = 0; i < 4; i++) if (sizes[i] == g.shadow_resolution) si = i;
            prop_label("  Resolution");
            if (ImGui::Combo("##sres", &si, size_names, 4)) { g.shadow_resolution = sizes[si]; changed = true; }
            prop_label("  Distance");
            changed |= ImGui::SliderFloat("##sdist", &g.shadow_distance, 5.0f, 200.0f, "%.0f m");
            prop_label("  Soft");
            changed |= toggle_switch("##soft", &g.soft_shadows);
        }
        prop_label("Point / spot shadows");
        changed |= toggle_switch("##lshadows", &g.local_shadows);
        track("gfx:local_shadows");
        if (g.local_shadows) {
            prop_label("  Lights");
            changed |= ImGui::SliderInt("##lsn", &g.local_shadow_lights, 1, 4);
            track("gfx:local_lights");
            static const int asizes[] = { 512, 1024, 2048, 4096 };
            static const char* asize_names[] = { "512", "1024", "2048", "4096" };
            int ai = 1;
            for (int i = 0; i < 4; i++) if (asizes[i] == g.local_shadow_resolution) ai = i;
            prop_label("  Atlas");
            if (ImGui::Combo("##lsres", &ai, asize_names, 4)) { g.local_shadow_resolution = asizes[ai]; changed = true; }
            faint("A spot light takes one of 16 atlas tiles, a point light six.");
        }
        static const char* ao_modes[] = { "Off", "SSAO", "GTAO" };
        choice("Ambient occlusion", "##aomode", "ao", ao_modes);
        if (g.ambient_occlusion) {
            prop_label("  Radius");
            changed |= ImGui::SliderFloat("##aor", &g.ao_radius, 0.1f, 2.0f, "%.2f m");
            prop_label("  Strength");
            changed |= ImGui::SliderFloat("##aos", &g.ao_strength, 0.0f, 2.0f, "%.2f");
        }
        static const char* bloom_modes[] = { "Off", "Simple", "Chain" };
        choice("Bloom", "##bloommode", "bloom", bloom_modes);
        static const char* aa_modes[] = { "Off", "FXAA", "SMAA", "TAA" };
        choice("Antialiasing", "##aamode", "aa", aa_modes);
        if (g.antialiasing && g.aa_mode == gfx::AAMode::TAA) faint("TAA jitters the camera and keeps a history; with Render scale below 1 it also upscales. Moving objects can ghost a little. FXAA and SMAA stay available.");
        static const char* ssr_modes[] = { "Off", "On" };
        choice("Reflections (SSR)", "##ssrmode", "ssr", ssr_modes);
        prop_label("PBR shading");
        changed |= toggle_switch("##pbr", &g.pbr);
        track("gfx:pbr");
        if (changed) g.preset = gfx::GraphicsPreset::Custom;
        ImGui::EndDisabled();
        if (fgl_backend[0]) {
            char on[128];
            snprintf(on, sizeof(on), "Running now: %s%s%s%s%s%s", fgl_features.shadows ? "shadows  " : "",
                     fgl_features.local_shadows ? "point/spot shadows  " : "", fgl_features.ambient_occlusion ? "AO  " : "",
                     fgl_features.bloom ? "bloom  " : "", fgl_features.antialiasing ? "AA  " : "", fgl_features.pbr ? "PBR" : "");
            faint(fgl_features.gpu_tier ? on : "Running now: base tier only");
        }
        ImGui::Dummy(ImVec2(0, 4));
    }
}

void Editor::panel_inspector() {
    ImGui::Begin(FE_ICON_INSPECTOR "  Inspector###Inspector");
    int n = scene.selected_count();
    Object* o = scene.get(scene.active());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float fs = ImGui::GetFontSize();
    if (!o) {
        // Empty state, centered.
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float cy = p.y + avail.y * 0.38f;
        draw_icon(dl, Icon::Cube, ImVec2(p.x + avail.x * 0.5f - 18, cy - 44), 36, pal.text_faint);
        const char* l1 = "Nothing selected";
        // Centered lines, wrapped to the panel so narrow panels do not cut them.
        const char* lines[] = { "Click an object in the viewport or the hierarchy.", "Shift-click adds to the selection." };
        if (font_medium) ImGui::PushFont(font_medium, 0.0f);
        dl->AddText(ImVec2(p.x + (avail.x - ImGui::CalcTextSize(l1).x) * 0.5f, cy), pal.text_dim, l1);
        if (font_medium) ImGui::PopFont();
        float y = cy + fs + 8, wrap = avail.x - 24.0f;
        for (const char* text : lines) {
            const char* t = text;
            while (*t) {
                // the longest run of words that fits
                const char* end = t;
                const char* fit = nullptr;
                for (;;) {
                    while (*end && *end != ' ') end++;
                    if (ImGui::CalcTextSize(t, end).x > wrap && fit) break;
                    fit = end;
                    if (!*end) break;
                    end++;
                }
                float lw = ImGui::CalcTextSize(t, fit).x;
                dl->AddText(ImVec2(p.x + (avail.x - lw) * 0.5f, y), pal.text_faint, t, fit);
                y += fs + 4;
                t = *fit ? fit + 1 : fit;
            }
        }
        ImGui::End();
        return;
    }

    // Header: the object's icon on its color, the name, what it is.
    {
        float big = ImGui::GetFrameHeight() * 2 + 4;
        ImVec2 p = ImGui::GetCursorScreenPos();
        auto ch = [](float v) { return (int)(fclamp(v, 0.0f, 1.0f) * 255.0f); };
        ImU32 tint = IM_COL32(ch(o->color.x), ch(o->color.y), ch(o->color.z), 60);
        dl->AddRectFilled(p, ImVec2(p.x + big, p.y + big), pal.frame, 6.0f);
        dl->AddRectFilled(p, ImVec2(p.x + big, p.y + big), tint, 6.0f);
        // The icon in the object's color, lifted on dark and deepened on light.
        float lift = settings.dark_theme ? 0.4f : 0.0f, keep = settings.dark_theme ? 0.6f : 0.75f;
        draw_icon(dl, object_icon(*o), ImVec2(p.x + big * 0.22f, p.y + big * 0.22f), big * 0.56f,
                  IM_COL32(ch(o->color.x * keep + lift), ch(o->color.y * keep + lift), ch(o->color.z * keep + lift), 255));
        ImGui::Dummy(ImVec2(big, big));
        ImGui::SameLine(0, 8);
        ImGui::BeginGroup();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (font_medium) ImGui::PushFont(font_medium, 0.0f);
        ImGui::InputText("##name", o->name, sizeof(o->name));
        if (font_medium) ImGui::PopFont();
        static const char* kinds[] = { "Cube", "Plane", "Sphere", "Cylinder", "Model" };
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.text_faint));
        const char* kind_text = o->kind == ObjectKind::Mesh ? kinds[(int)o->mesh] : object_kind_name(o->kind);
        if (n > 1) ImGui::Text("%s   #%u   +%d more selected", kind_text, o->id, n - 1);
        else ImGui::Text("%s   #%u", kind_text, o->id);
        ImGui::PopStyleColor();
        ImGui::EndGroup();
    }
    ImGui::Dummy(ImVec2(0, 4));

    if (section_header("Transform", Icon::Move)) {
        // The parent: position, rotation and scale below are relative to it.
        {
            prop_label("Parent");
            Object* par = scene.get(o->parent);
            if (ImGui::BeginCombo("##parent", par ? par->name : "None (scene)")) {
                if (ImGui::Selectable("None (scene)", !par)) scene.set_parent(o->id, kNoObject);
                for (int i = 0; i < scene.count(); i++) {
                    Object& c = scene.at(i);
                    if (c.id == o->id || scene.is_ancestor(o->id, c.id)) continue;     // not itself or its children
                    ImGui::PushID((int)c.id);
                    if (ImGui::Selectable(c.name, c.id == o->parent)) {
                        scene.set_parent(o->id, c.id);
                        c.expanded = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            track("inspector:parent");
        }
        Vec3 pos = o->position, scl = o->scale;
        Vec3 rot = o->rotation.to_euler_deg();
        Vec3 old_pos = pos;
        if (vec3_field("Position", &pos, 0.05f, "%.2f")) {
            // With several objects selected, move them all by the same amount.
            Vec3 d = pos - old_pos;
            for (int i = 0; i < n; i++) {
                ObjectId sid = scene.selected(i);
                if (sid != o->id && scene.has_selected_ancestor(sid)) continue;     // moves with its parent
                if (Object* s = scene.get(sid)) s->position += d;
            }
        }
        if (vec3_field("Rotation", &rot, 0.5f, "%.1f")) o->rotation = Quat::euler_deg(rot);
        if (vec3_field("Scale", &scl, 0.02f, "%.2f")) o->scale = scl;
        ImGui::Dummy(ImVec2(0, 4));
    }
    // A special object's own settings (Camera, Sky, ...) come right after
    // the transform.
    if (settings_of(o->kind) != ModuleKind::Count) module_section(o, settings_of(o->kind));
    if (o->kind == ObjectKind::Mesh && section_header("Mesh", Icon::Cube)) {
        static const char* kinds[] = { "Cube", "Plane", "Sphere", "Cylinder", "Model (.mof)" };
        int k = (int)o->mesh;
        prop_label("Shape");
        if (ImGui::Combo("##shape", &k, kinds, 5)) o->mesh = (MeshKind)k;
        if (o->mesh == MeshKind::Model) slot_field("Model", o->model_path, sizeof(o->model_path), this, path_is_model, Icon::Model);
        prop_label("Visible");
        toggle_switch("##visible", &o->visible);
        track("inspector:visible");
        ImGui::Dummy(ImVec2(0, 4));
    }
    if (o->kind == ObjectKind::Mesh && section_header("Look", Icon::Sun)) {
        prop_label("Color");
        ImGui::ColorEdit3("##color", &o->color.x, ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_DisplayHex);
        slot_field("Texture", o->texture_path, sizeof(o->texture_path), this, path_is_image, Icon::Image);
        slot_field("Shader", o->shader_path, sizeof(o->shader_path), this, path_is_shader, Icon::File);
        prop_label("Roughness");
        ImGui::SliderFloat("##roughness", &o->roughness, 0.0f, 1.0f, "%.2f");
        track("inspector:roughness");
        prop_label("Metal");
        ImGui::SliderFloat("##metallic", &o->metallic, 0.0f, 1.0f, "%.2f");
        track("inspector:metallic");
        prop_label("Unlit");
        toggle_switch("##unlit", &o->unlit);
        track("inspector:unlit");
        prop_label("Shadows");
        toggle_switch("##cast_shadows", &o->cast_shadows);
        track("inspector:cast_shadows");
        if (!fgl_features.pbr && !o->unlit) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.text_faint));
            ImGui::TextWrapped("Roughness, metal and shadows show with FarcadeGL's GPU tier (Settings > Graphics).");
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, 4));
    }
    // ---- modules (Rigidbody 3D, Light, ...), then "Add module".
    for (int k = 0; k < kFirstObjectSettings; k++)
        if (o->modules.has((ModuleKind)k)) module_section(o, (ModuleKind)k);
    add_module_button(o);
    // ---- scripts: one section each, in the order they run, then "Add script".
    for (int si = 0; si < o->script_count; si++) script_section(o, si);
    add_script_button(o);
    if (int links = scene.links_to(o->id)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.text_faint));
        ImGui::TextWrapped("%d script input%s link%s to this object.", links, links == 1 ? "" : "s", links == 1 ? "s" : "");
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

// ---------------------------------------------------------------- Files

namespace {
struct Listing {
    platform::DirEntry* items = nullptr;
    int count = 0, cap = 0;
    char dir[256] = {};
    bool valid = false;
};
Listing g_listing;

void add_entry(const platform::DirEntry& e, void* user) {
    Listing* l = (Listing*)user;
    if (e.name[0] == '.') return;
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->items = (platform::DirEntry*)realloc(l->items, sizeof(platform::DirEntry) * l->cap);
    }
    l->items[l->count++] = e;
}

int compare_entries(const void* a, const void* b) {
    const platform::DirEntry* x = (const platform::DirEntry*)a;
    const platform::DirEntry* y = (const platform::DirEntry*)b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return ImStricmp(x->name, y->name);
}

void refresh(const char* dir) {
    g_listing.count = 0;
    snprintf(g_listing.dir, sizeof(g_listing.dir), "%s", dir);
    g_listing.valid = platform::list_dir(dir, add_entry, &g_listing);
    if (g_listing.count > 1) qsort(g_listing.items, g_listing.count, sizeof(platform::DirEntry), compare_entries);
}

void join(char* out, size_t cap, const char* dir, const char* name) {
    size_t n = strlen(dir);
    bool sep = n && (dir[n - 1] == '/' || dir[n - 1] == ':');
    snprintf(out, cap, sep ? "%s%s" : "%s/%s", dir, name);
}

Icon file_icon(const platform::DirEntry& e) {
    if (e.is_dir) return Icon::Folder;
    if (path_is_image(e.name)) return Icon::Image;
    if (path_is_model(e.name)) return Icon::Model;
    if (path_is_script(e.name)) return Icon::Script;
    if (path_is_scene(e.name)) return Icon::Layers;
    return Icon::File;
}

ImU32 file_color(const platform::DirEntry& e) {
    if (e.is_dir) return IM_COL32(230, 180, 80, 255);
    if (path_is_image(e.name)) return IM_COL32(120, 200, 140, 255);
    if (path_is_model(e.name)) return IM_COL32(110, 160, 255, 255);
    if (path_is_script(e.name)) return IM_COL32(200, 140, 230, 255);
    if (path_is_scene(e.name)) return IM_COL32(240, 150, 90, 255);
    return ImGui::GetColorU32(ImGuiCol_TextDisabled);
}
}  // namespace

void Editor::panel_files() {
    ImGui::Begin(FE_ICON_FILES "  Files###Files");
    if (!g_listing.valid || strcmp(g_listing.dir, files_dir)) refresh(files_dir);
    static char selected_file[512];
    static char filter[64];

    // Toolbar: up, refresh, the folder as clickable parts, a filter.
    float bs = ImGui::GetFrameHeight();
    bool at_root = !strcmp(files_dir, project_dir);
    ImGui::BeginDisabled(at_root);
    if (icon_button(Icon::Up, "Up one folder", false, bs)) {
        char* slash = strrchr(files_dir, '/');
        if (slash && slash != files_dir) *slash = 0;
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, 2);
    if (icon_button(Icon::Refresh, "Refresh", false, bs)) refresh(files_dir);
    ImGui::SameLine(0, 8);
    char go_to[256] = {};
    size_t pl = strlen(project_dir);
    auto crumb = [&](const char* label, int len, const char* target_end) {
        char text[96];
        snprintf(text, sizeof(text), "%.*s", len, label);
        ImGui::PushID(target_end);
        float tw = ImGui::CalcTextSize(text).x + 12;
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##crumb", ImVec2(tw, bs))) {
            size_t n = (size_t)(target_end - files_dir);
            snprintf(go_to, sizeof(go_to), "%.*s", (int)n, files_dir);
        }
        bool last = *target_end == 0;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (ImGui::IsItemHovered()) dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + bs), pal.hover, 4.0f);
        float fs = ImGui::GetFontSize();
        if (last && font_medium) ImGui::PushFont(font_medium, 0.0f);
        dl->AddText(ImVec2(p.x + 6, p.y + (bs - fs) * 0.5f), last ? pal.text : pal.text_dim, text);
        if (last && font_medium) ImGui::PopFont();
        ImGui::PopID();
    };
    if (!strncmp(files_dir, project_dir, pl)) {
        const char* pname = path_filename(project_dir);
        crumb(pname, (int)strlen(pname), files_dir + pl);
        for (const char* p = files_dir + pl; *p;) {
            while (*p == '/') p++;
            if (!*p) break;
            const char* e = p;
            while (*e && *e != '/') e++;
            ImGui::SameLine(0, 0);
            ImVec2 cp = ImGui::GetCursorScreenPos();
            float fs = ImGui::GetFontSize();
            draw_icon(ImGui::GetWindowDrawList(), Icon::ChevronRight, ImVec2(cp.x, cp.y + (bs - fs * 0.8f) * 0.5f), fs * 0.8f, pal.text_faint);
            ImGui::Dummy(ImVec2(fs * 0.8f, bs));
            ImGui::SameLine(0, 0);
            crumb(p, (int)(e - p), e);
            p = e;
        }
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", files_dir);
    }
    float fw = 180 * settings.ui_scale;
    ImGui::SameLine(0, 0);
    float right = ImGui::GetWindowContentRegionMax().x - fw;
    if (right > ImGui::GetCursorPosX() + 10) {
        ImGui::SameLine(right);
        search_field("##filefilter", "Filter files", filter, sizeof(filter), fw);
    } else {
        ImGui::NewLine();
    }
    if (go_to[0]) snprintf(files_dir, sizeof(files_dir), "%s", go_to);
    ImGui::Dummy(ImVec2(0, 2));

    ImGui::BeginChild("##grid");
    if (!g_listing.valid) ImGui::TextDisabled("This folder cannot be read.");
    float tile = 80.0f * settings.ui_scale, icon = 32.0f * settings.ui_scale;
    float gap = 6.0f;
    int cols = (int)((ImGui::GetContentRegionAvail().x + gap) / (tile + gap));
    if (cols < 1) cols = 1;
    char open_dir[256] = {};
    file_tile_count = 0;
    int shown = 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    for (int i = 0; i < g_listing.count; i++) {
        const platform::DirEntry& e = g_listing.items[i];
        if (filter[0] && !ImStristr(e.name, nullptr, filter, nullptr)) continue;
        char full[512];
        join(full, sizeof(full), files_dir, e.name);
        ImGui::PushID(i);
        if (shown++ % cols) ImGui::SameLine();
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool sel = !strcmp(selected_file, full);
        if (ImGui::Selectable("##tile", sel, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(tile, tile)))
            snprintf(selected_file, sizeof(selected_file), "%s", full);
        bool hov = ImGui::IsItemHovered();
        if (file_tile_count < kMaxTiles) {
            TileRect& tr = file_tiles[file_tile_count++];
            snprintf(tr.path, sizeof(tr.path), "%s", full);
            tr.x = p.x; tr.y = p.y; tr.w = tile; tr.h = tile;
        }
        if (hov && ImGui::IsMouseDoubleClicked(0)) {
            if (e.is_dir) snprintf(open_dir, sizeof(open_dir), "%s", full);
            else open_file(full);
        }
        if (!e.is_dir && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kFilePayload, full, strlen(full) + 1);
            draw_icon(ImGui::GetWindowDrawList(), file_icon(e), ImGui::GetCursorScreenPos(), 20, file_color(e));
            ImGui::Dummy(ImVec2(22, 20));
            ImGui::SameLine();
            ImGui::TextUnformatted(e.name);
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginPopupContextItem("##ctx")) {
            snprintf(selected_file, sizeof(selected_file), "%s", full);
            if (menu_item(e.is_dir ? "Open folder" : "Open")) {
                if (e.is_dir) snprintf(open_dir, sizeof(open_dir), "%s", full);
                else open_file(full);
            }
            const char* why = platform::reveal_unavailable_reason();
            if (menu_item("Open in file manager", nullptr, false, why == nullptr)) {
                if (!platform::reveal_in_file_manager(full)) log(LogLevel::Error, "Could not open the file manager");
            }
            if (why && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why);
            if (!e.is_dir && path_is_importable_model(e.name) && menu_item("Import as model")) import_model(full);
            if (menu_item("Copy path")) ImGui::SetClipboardText(full);
            ImGui::EndPopup();
        }
        ImVec2 q(p.x + tile, p.y + tile);
        if (sel) dl->AddRectFilled(p, q, pal.accent_soft, 6.0f);
        else if (hov) dl->AddRectFilled(p, q, pal.hover, 6.0f);
        draw_icon(dl, file_icon(e), ImVec2(p.x + (tile - icon) * 0.5f, p.y + 10), icon, file_color(e));
        // Name, cut to the tile width.
        char label[40];
        snprintf(label, sizeof(label), "%s", e.name);
        ImFont* f = ImGui::GetFont();
        float fs = ImGui::GetFontSize();
        while (strlen(label) > 4 && f->CalcTextSizeA(fs, FLT_MAX, 0, label).x > tile - 8) {
            size_t n = strlen(label);
            label[n - 4] = '.'; label[n - 3] = '.'; label[n - 2] = 0;
        }
        float tw = f->CalcTextSizeA(fs, FLT_MAX, 0, label).x;
        dl->AddText(ImVec2(p.x + (tile - tw) * 0.5f, p.y + tile - fs - 10), sel ? pal.text : pal.text_dim, label);
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if (open_dir[0]) snprintf(files_dir, sizeof(files_dir), "%s", open_dir);
    if (g_listing.count == 0 && g_listing.valid)
        ImGui::TextDisabled("Empty folder. Files dropped here from the file manager will appear here.");
    // Empty space: context menu for the folder itself.
    ImGui::Dummy(ImVec2(-FLT_MIN, fmaxf_(ImGui::GetContentRegionAvail().y, 20)));
    if (ImGui::IsItemClicked(0)) selected_file[0] = 0;
    if (ImGui::BeginPopupContextItem("##folderctx")) {
        const char* why = platform::reveal_unavailable_reason();
        if (menu_item("Open folder in file manager", nullptr, false, why == nullptr)) platform::reveal_in_file_manager(files_dir);
        if (why && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why);
        if (menu_item("Refresh")) refresh(files_dir);
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------- Console

void Editor::panel_console() {
    ImGui::Begin(FE_ICON_CONSOLE "  Console###Console");
    static bool show[3] = { true, true, true };
    static char filter[64];
    int counts[3] = { 0, 0, 0 };
    for (int i = 0; i < log_count; i++) counts[(int)log_lines[(log_head + i) % kLogLines].level]++;
    static const Icon icons[3] = { Icon::Info, Icon::Warning, Icon::Error };
    ImU32 colors[3] = { pal.text_dim, pal.warning, pal.error };
    static const char* names[3] = { "info", "warnings", "errors" };
    for (int k = 0; k < 3; k++) {
        if (k) ImGui::SameLine(0, 4);
        char label[32], id[16];
        snprintf(label, sizeof(label), "%d", counts[k]);
        snprintf(id, sizeof(id), "##chip%d", k);
        if (chip(id, icons[k], colors[k], label, show[k])) show[k] = !show[k];
        ImGui::SetItemTooltip("Show %s", names[k]);
        char key[24];
        snprintf(key, sizeof(key), "console:%s", names[k]);
        track(key);
    }
    ImGui::SameLine(0, 10);
    float bs = ImGui::GetFrameHeight();
    float fw = ImGui::GetContentRegionAvail().x - bs - 6;
    if (fw > 260) fw = 260;
    search_field("##logfilter", "Filter messages", filter, sizeof(filter), fw);
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - bs);
    if (icon_button(Icon::Trash, "Clear the console", false, bs)) { log_count = 0; log_head = 0; }
    track("console:clear");
    ImGui::Dummy(ImVec2(0, 1));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(pal.app_bg & 0x60FFFFFF));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PushFont(font_mono, 0.0f);
    float lh = ImGui::GetTextLineHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    double t0 = log_count ? log_lines[log_head].time : 0.0;
    for (int i = 0; i < log_count; i++) {
        const LogLine& l = log_lines[(log_head + i) % kLogLines];
        if (!show[(int)l.level]) continue;
        if (filter[0] && !ImStristr(l.text, nullptr, filter, nullptr)) continue;
        ImVec2 p = ImGui::GetCursorScreenPos();
        char ts[16];
        double t = l.time - t0;
        if (t < 0) t = 0;
        snprintf(ts, sizeof(ts), "%02d:%04.1f", (int)(t / 60), t - 60 * (int)(t / 60));
        dl->AddText(p, pal.text_faint, ts);
        float tx = p.x + ImGui::CalcTextSize("00:00.0").x + 10;
        draw_icon(dl, icons[(int)l.level], ImVec2(tx, p.y + 1), lh - 2, colors[(int)l.level]);
        ImGui::SetCursorScreenPos(ImVec2(tx + lh + 6, p.y));
        ImU32 tc = l.level == LogLevel::Info ? pal.text : colors[(int)l.level];
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(tc));
        ImGui::TextUnformatted(l.text);
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();
    if (log_scroll_to_end) { ImGui::SetScrollHereY(1.0f); log_scroll_to_end = false; }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------- Settings

void Editor::window_settings() {
    ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(520, 320), ImVec2(FLT_MAX, FLT_MAX));
    if (!begin_floating(FE_ICON_GEAR "  Settings###Settings", &show_settings, ImGuiWindowFlags_NoCollapse)) {
        end_floating();
        return;
    }
    // A navigation column on the left, the page on the right.
    struct Page { Icon icon; const char* name; const char* about; };
    static const Page pages[] = {
        { Icon::Gear, "General", "How the editor behaves." },
        { Icon::Sun, "Appearance", "Theme and colors." },
        { Icon::Grid, "Viewport", "Grid, snapping and the CPU preview." },
        { Icon::Monitor, "Graphics", "FarcadeGL: presets, every technique as a choice, effects. Exported with games." },
        { Icon::File, "File editors", "Programs that open files from the Files panel." },
    };
    const int page_count = (int)(sizeof(pages) / sizeof(pages[0]));
    if (settings_page < 0 || settings_page >= page_count) settings_page = 0;
    float nav_w = 184.0f * settings.ui_scale;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(pal.sidebar));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::BeginChild("##settings_nav", ImVec2(nav_w, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    for (int i = 0; i < page_count; i++) {
        char id[32];
        snprintf(id, sizeof(id), "##page%d", i);
        if (nav_item(id, pages[i].icon, pages[i].name, settings_page == i)) settings_page = i;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 14.0f);
    ImGui::BeginChild("##settings_page", ImVec2(0, 0));
    {
        ImGui::PushFont(font_medium);
        ImGui::SetWindowFontScale(1.25f);
        ImGui::TextUnformatted(pages[settings_page].name);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.text_dim));
        ImGui::TextWrapped("%s", pages[settings_page].about);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 6));
    }
    {
        if (settings_page == 0) {
            ImGui::Dummy(ImVec2(0, 4));
            prop_label("Invert scrolling");
            toggle_switch("##invert_scrolling", &settings.invert_scrolling);
            track("settings:invert_scrolling");
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.text_faint));
            ImGui::TextWrapped("Reverse the mouse wheel direction for scrolling and viewport zoom.");
            ImGui::PopStyleColor();
        }
        if (settings_page == 1) {
            ImGui::Dummy(ImVec2(0, 4));
            prop_label("Dark theme");
            if (toggle_switch("##dark", &settings.dark_theme)) apply_theme();
        }
        if (settings_page == 2) {
            ImGui::Dummy(ImVec2(0, 4));
            if (section_header("Grid and snap", Icon::Grid)) {
                prop_label("Show grid");
                toggle_switch("##grid", &settings.show_grid);
                prop_label("Grid size");
                ImGui::DragFloat("##gs", &settings.grid_size, 0.05f, 0.1f, 100.0f, "%.2f m");
                prop_label("Snap");
                toggle_switch("##snap", &settings.snap);
                prop_label("Move step");
                ImGui::DragFloat("##ms", &settings.snap_move, 0.05f, 0.01f, 100.0f, "%.2f m");
                prop_label("Rotate step");
                ImGui::DragFloat("##rs", &settings.snap_rotate, 1.0f, 1.0f, 180.0f, "%.0f deg");
                prop_label("Scale step");
                ImGui::DragFloat("##ss", &settings.snap_scale, 0.01f, 0.01f, 10.0f, "%.2f");
                ImGui::Dummy(ImVec2(0, 4));
            }
            if (section_header("CPU preview", Icon::Monitor)) {
                prop_label("Smooth edges");
                toggle_switch("##ssaa", &settings.view_supersample);
                prop_label("Ground shadows");
                toggle_switch("##shadows", &settings.view_shadows);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.text_faint));
                ImGui::TextWrapped("The CPU preview is the editor's own quick renderer (renderer menu, top right). While you drag "
                                   "the camera or an object it draws without smoothing and shadows, and redraws in full when you let go.");
                ImGui::PopStyleColor();
            }
        }
        if (settings_page == 3) {
            settings_graphics();
        }
        if (settings_page == 4) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.text_dim));
            ImGui::TextWrapped("Double-clicking a file in Files opens it with the program set for its extension. "
                               "\"*\" is used for every other extension.");
            ImGui::PopStyleColor();
            ImGui::Spacing();
            int remove = -1;
            if (ImGui::BeginTable("##eds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Extension", ImGuiTableColumnFlags_WidthFixed, 90);
                ImGui::TableSetupColumn("Program");
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
                ImGui::TableHeadersRow();
                for (int i = 0; i < settings.editor_count; i++) {
                    FileEditor& e = settings.editors[i];
                    ImGui::PushID(i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::InputText("##ext", e.extension, sizeof(e.extension));
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::InputText("##prog", e.program, sizeof(e.program));
                    ImGui::TableNextColumn();
                    if (icon_button(Icon::Close, "Remove", false, ImGui::GetFrameHeight())) remove = i;
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (remove >= 0) {
                memmove(&settings.editors[remove], &settings.editors[remove + 1], sizeof(FileEditor) * (settings.editor_count - remove - 1));
                settings.editor_count--;
            }
            if (settings.editor_count < 16 && ImGui::Button("Add an extension")) {
                FileEditor& e = settings.editors[settings.editor_count++];
                snprintf(e.extension, sizeof(e.extension), ".ext");
                snprintf(e.program, sizeof(e.program), "0:/programs/");
            }
        }
    }
    ImGui::EndChild();
    end_floating();
}

}  // namespace fe
