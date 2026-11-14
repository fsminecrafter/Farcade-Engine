// Hierarchy, Inspector, Files, Console and Settings.
#include "editor.h"
#include "icons.h"
#include "platform/platform.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe {

static const char* kFilePayload = "FE_FILE";   // drag-and-drop payload: a NUL-terminated path

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

// ---------------------------------------------------------------- Hierarchy

void Editor::panel_hierarchy() {
    ImGui::Begin("Hierarchy");
    static char filter[64];
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "Search", filter, sizeof(filter));
    ImGui::Spacing();

    ImGui::BeginChild("##objects");
    float row_h = ImGui::GetTextLineHeight() + 8;
    ObjectId clicked = kNoObject;
    bool shift = ImGui::GetIO().KeyShift;
    for (int i = 0; i < scene.count(); i++) {
        Object& o = scene.at(i);
        if (filter[0] && !ImStristr(o.name, nullptr, filter, nullptr)) continue;
        ImGui::PushID((int)o.id);
        bool sel = scene.is_selected(o.id);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, row_h))) clicked = o.id;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) frame_selection();
        if (const char* path = accept_file_drop()) apply_file_to_object(path, o.id);
        if (ImGui::BeginPopupContextItem("##ctx")) {
            if (!sel) scene.select_only(o.id);
            if (ImGui::MenuItem("Frame", "F")) frame_selection();
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicate_selection();
            if (ImGui::MenuItem("Delete", "Del")) delete_selection();
            ImGui::EndPopup();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float ih = ImGui::GetTextLineHeight();
        ImU32 icol = sel ? ImGui::GetColorU32(ImVec4(0.45f, 0.65f, 1.0f, 1.0f)) : dim_text();
        draw_icon(dl, mesh_icon(o.mesh), ImVec2(pos.x + 4, pos.y + (row_h - ih) * 0.5f), ih, icol);
        dl->AddText(ImVec2(pos.x + ih + 12, pos.y + (row_h - ih) * 0.5f),
                    ImGui::GetColorU32(o.visible ? ImGuiCol_Text : ImGuiCol_TextDisabled), o.name);
        ImGui::PopID();
    }
    if (clicked != kNoObject) {
        if (shift) scene.toggle(clicked);
        else scene.select_only(clicked);
    }
    // Empty area: click clears, right-click creates.
    ImGui::InvisibleButton("##empty", ImVec2(-FLT_MIN, fmaxf_(ImGui::GetContentRegionAvail().y, 40)));
    if (ImGui::IsItemClicked(0) && !shift) scene.clear_selection();
    if (const char* path = accept_file_drop()) {
        if (path_is_model(path)) {
            ObjectId id = create_object(path_filename(path), MeshKind::Model, camera.target);
            apply_file_to_object(path, id);
        }
    }
    if (ImGui::BeginPopupContextItem("##create")) {
        if (ImGui::MenuItem("Cube")) create_object("Cube", MeshKind::Cube, Vec3(0, 0.5f, 0));
        if (ImGui::MenuItem("Sphere")) create_object("Sphere", MeshKind::Sphere, Vec3(0, 0.5f, 0));
        if (ImGui::MenuItem("Cylinder")) create_object("Cylinder", MeshKind::Cylinder, Vec3(0, 0.5f, 0));
        if (ImGui::MenuItem("Plane")) create_object("Plane", MeshKind::Plane, Vec3(0, 0, 0));
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------- Inspector

// X/Y/Z drag fields with a colored bar per axis.
static bool vec3_field(const char* label, Vec3* v, float speed, const char* fmt) {
    bool changed = false;
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(84);
    float w = (ImGui::GetContentRegionAvail().x - 8) / 3.0f;
    static const ImU32 axis[3] = { IM_COL32(230, 80, 80, 255), IM_COL32(110, 200, 90, 255), IM_COL32(80, 140, 240, 255) };
    float* f = &v->x;
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine(0, 4);
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemWidth(w);
        changed |= ImGui::DragFloat("##v", &f[i], speed, 0, 0, fmt);
        float h = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 4), ImVec2(p.x + 3, p.y + h - 4), axis[i], 1.5f);
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

static void slot_field(const char* label, char* path, size_t cap, Editor* ed, bool (*accepts)(const char*)) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(84);
    float w = ImGui::GetContentRegionAvail().x - (path[0] ? ImGui::GetFrameHeight() + 4 : 0);
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::Button(path[0] ? path_filename(path) : "Drop a file here", ImVec2(w, 0));
    ImGui::PopStyleColor();
    if (path[0] && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path);
    if (const char* p = accept_file_drop()) {
        if (accepts(p)) snprintf(path, cap, "%s", p);
        else ed->log(LogLevel::Warning, "%s does not fit the %s slot", path_filename(p), label);
    }
    if (path[0]) {
        ImGui::SameLine(0, 4);
        if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) path[0] = 0;
    }
    ImGui::PopID();
}

void Editor::panel_inspector() {
    ImGui::Begin("Inspector");
    int n = scene.selected_count();
    Object* o = scene.get(scene.active());
    if (!o) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Select an object in the viewport or the hierarchy to edit it here.");
        ImGui::TextWrapped("Shift-click adds objects to the selection.");
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }
    if (n > 1) {
        ImGui::TextDisabled("%d objects selected; editing %s", n, o->name);
        ImGui::Spacing();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##name", o->name, sizeof(o->name));
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        Vec3 pos = o->position, scl = o->scale;
        Vec3 rot = o->rotation.to_euler_deg();
        Vec3 old_pos = pos;
        if (vec3_field("Position", &pos, 0.05f, "%.2f")) {
            // With several objects selected, move them all by the same amount.
            Vec3 d = pos - old_pos;
            for (int i = 0; i < n; i++)
                if (Object* s = scene.get(scene.selected(i))) s->position += d;
        }
        if (vec3_field("Rotation", &rot, 0.5f, "%.1f")) o->rotation = Quat::euler_deg(rot);
        if (vec3_field("Scale", &scl, 0.02f, "%.2f")) o->scale = scl;
    }
    if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* kinds[] = { "Cube", "Plane", "Sphere", "Cylinder", "Model (.mof)" };
        int k = (int)o->mesh;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Shape");
        ImGui::SameLine(84);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##shape", &k, kinds, 5)) o->mesh = (MeshKind)k;
        if (o->mesh == MeshKind::Model) slot_field("Model", o->model_path, sizeof(o->model_path), this, path_is_model);
        ImGui::Checkbox("Visible", &o->visible);
    }
    if (ImGui::CollapsingHeader("Look", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Color");
        ImGui::SameLine(84);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::ColorEdit3("##color", &o->color.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        slot_field("Texture", o->texture_path, sizeof(o->texture_path), this, path_is_image);
    }
    if (ImGui::CollapsingHeader("Script", ImGuiTreeNodeFlags_DefaultOpen)) {
        slot_field("Script", o->script_path, sizeof(o->script_path), this, path_is_script);
        if (o->script_path[0] && ImGui::Button("Edit script")) open_file(o->script_path);
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
    return Icon::File;
}

ImU32 file_color(const platform::DirEntry& e) {
    if (e.is_dir) return IM_COL32(230, 180, 80, 255);
    if (path_is_image(e.name)) return IM_COL32(120, 200, 140, 255);
    if (path_is_model(e.name)) return IM_COL32(110, 160, 255, 255);
    if (path_is_script(e.name)) return IM_COL32(200, 140, 230, 255);
    return ImGui::GetColorU32(ImGuiCol_TextDisabled);
}
}  // namespace

void Editor::panel_files() {
    ImGui::Begin("Files");
    if (!g_listing.valid || strcmp(g_listing.dir, files_dir)) refresh(files_dir);

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
    ImGui::SameLine(0, 10);
    ImGui::AlignTextToFramePadding();
    // The folder, relative to the project ("MyGame / assets / textures").
    size_t pl = strlen(project_dir);
    if (!strncmp(files_dir, project_dir, pl)) {
        ImGui::TextUnformatted(path_filename(project_dir));
        for (const char* p = files_dir + pl; *p;) {
            while (*p == '/') p++;
            if (!*p) break;
            const char* e = p;
            while (*e && *e != '/') e++;
            ImGui::SameLine(0, 6);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0, 6);
            ImGui::Text("%.*s", (int)(e - p), p);
            p = e;
        }
    } else {
        ImGui::TextDisabled("%s", files_dir);
    }
    ImGui::Separator();

    ImGui::BeginChild("##grid");
    if (!g_listing.valid) ImGui::TextDisabled("This folder cannot be read.");
    float tile = 84.0f * settings.ui_scale, icon = 34.0f * settings.ui_scale;
    int cols = (int)(ImGui::GetContentRegionAvail().x / (tile + 8));
    if (cols < 1) cols = 1;
    char open_dir[256] = {};
    file_tile_count = 0;
    for (int i = 0; i < g_listing.count; i++) {
        const platform::DirEntry& e = g_listing.items[i];
        char full[512];
        join(full, sizeof(full), files_dir, e.name);
        ImGui::PushID(i);
        if (i % cols) ImGui::SameLine(0, 8);
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Selectable("##tile", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(tile, tile));
        if (file_tile_count < kMaxTiles) {
            TileRect& tr = file_tiles[file_tile_count++];
            snprintf(tr.path, sizeof(tr.path), "%s", full);
            tr.x = p.x; tr.y = p.y; tr.w = tile; tr.h = tile;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
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
            if (ImGui::MenuItem(e.is_dir ? "Open folder" : "Open")) {
                if (e.is_dir) snprintf(open_dir, sizeof(open_dir), "%s", full);
                else open_file(full);
            }
            const char* why = platform::reveal_unavailable_reason();
            if (ImGui::MenuItem("Open in file manager", nullptr, false, why == nullptr)) {
                if (!platform::reveal_in_file_manager(full)) log(LogLevel::Error, "Could not open the file manager");
            }
            if (why && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why);
            if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(full);
            ImGui::EndPopup();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        draw_icon(dl, file_icon(e), ImVec2(p.x + (tile - icon) * 0.5f, p.y + 8), icon, file_color(e));
        // Name, cut to the tile width.
        char label[40];
        snprintf(label, sizeof(label), "%s", e.name);
        ImFont* f = ImGui::GetFont();
        float fs = ImGui::GetFontSize();
        while (strlen(label) > 4 && f->CalcTextSizeA(fs, FLT_MAX, 0, label).x > tile - 6) {
            size_t n = strlen(label);
            label[n - 4] = '.'; label[n - 3] = '.'; label[n - 2] = 0;
        }
        float tw = f->CalcTextSizeA(fs, FLT_MAX, 0, label).x;
        dl->AddText(ImVec2(p.x + (tile - tw) * 0.5f, p.y + tile - fs - 8), ImGui::GetColorU32(ImGuiCol_Text), label);
        ImGui::PopID();
    }
    if (open_dir[0]) snprintf(files_dir, sizeof(files_dir), "%s", open_dir);
    // Empty space: context menu for the folder itself.
    ImGui::Dummy(ImVec2(-FLT_MIN, fmaxf_(ImGui::GetContentRegionAvail().y, 20)));
    if (ImGui::BeginPopupContextItem("##folderctx")) {
        const char* why = platform::reveal_unavailable_reason();
        if (ImGui::MenuItem("Open folder in file manager", nullptr, false, why == nullptr)) platform::reveal_in_file_manager(files_dir);
        if (why && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why);
        if (ImGui::MenuItem("Refresh")) refresh(files_dir);
        ImGui::EndPopup();
    }
    if (g_listing.count == 0 && g_listing.valid)
        ImGui::TextDisabled("Empty folder. Files dropped here from the file manager will appear here.");
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------- Console

void Editor::panel_console() {
    ImGui::Begin("Console");
    static bool show[3] = { true, true, true };
    if (ImGui::Button("Clear")) { log_count = 0; log_head = 0; }
    ImGui::SameLine();
    ImGui::Checkbox("Info", &show[0]);
    ImGui::SameLine();
    ImGui::Checkbox("Warnings", &show[1]);
    ImGui::SameLine();
    ImGui::Checkbox("Errors", &show[2]);
    ImGui::Separator();
    ImGui::BeginChild("##log");
    ImGui::PushFont(font_mono, 0.0f);
    static const ImU32 colors[3] = { IM_COL32(150, 160, 175, 255), IM_COL32(240, 190, 80, 255), IM_COL32(240, 95, 95, 255) };
    static const Icon icons[3] = { Icon::Info, Icon::Warning, Icon::Error };
    float lh = ImGui::GetTextLineHeight();
    for (int i = 0; i < log_count; i++) {
        const LogLine& l = log_lines[(log_head + i) % kLogLines];
        if (!show[(int)l.level]) continue;
        ImVec2 p = ImGui::GetCursorScreenPos();
        draw_icon(ImGui::GetWindowDrawList(), icons[(int)l.level], ImVec2(p.x, p.y + 1), lh - 2, colors[(int)l.level]);
        ImGui::SetCursorScreenPos(ImVec2(p.x + lh + 4, p.y));
        if (l.level == LogLevel::Info) ImGui::TextUnformatted(l.text);
        else ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(colors[(int)l.level]), "%s", l.text);
    }
    ImGui::PopFont();
    if (log_scroll_to_end) { ImGui::SetScrollHereY(1.0f); log_scroll_to_end = false; }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------- Settings

void Editor::window_settings() {
    ImGui::SetNextWindowSize(ImVec2(520, 440), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Settings", &show_settings, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("##settings")) {
        if (ImGui::BeginTabItem("Appearance")) {
            if (ImGui::Checkbox("Dark theme", &settings.dark_theme)) apply_theme();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Grid and snap")) {
            ImGui::DragFloat("Grid size", &settings.grid_size, 0.05f, 0.1f, 100.0f, "%.2f m");
            ImGui::DragInt("Grid lines", &settings.grid_lines, 1, 2, 200);
            ImGui::Checkbox("Snap on", &settings.snap);
            ImGui::DragFloat("Move step", &settings.snap_move, 0.05f, 0.01f, 100.0f, "%.2f m");
            ImGui::DragFloat("Rotate step", &settings.snap_rotate, 1.0f, 1.0f, 180.0f, "%.0f deg");
            ImGui::DragFloat("Scale step", &settings.snap_scale, 0.01f, 0.01f, 10.0f, "%.2f");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("File editors")) {
            ImGui::TextWrapped("Double-clicking a file in Files opens it with the program set for its extension. "
                               "\"*\" is used for every other extension.");
            ImGui::Spacing();
            int remove = -1;
            if (ImGui::BeginTable("##eds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Extension", ImGuiTableColumnFlags_WidthFixed, 90);
                ImGui::TableSetupColumn("Program");
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24);
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
                    if (ImGui::SmallButton("x")) remove = i;
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (remove >= 0) {
                memmove(&settings.editors[remove], &settings.editors[remove + 1], sizeof(FileEditor) * (settings.editor_count - remove - 1));
                settings.editor_count--;
            }
            if (settings.editor_count < 16 && ImGui::Button("Add")) {
                FileEditor& e = settings.editors[settings.editor_count++];
                snprintf(e.extension, sizeof(e.extension), ".ext");
                snprintf(e.program, sizeof(e.program), "0:/programs/");
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

}  // namespace fe
