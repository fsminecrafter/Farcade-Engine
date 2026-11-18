#include "editor.h"
#include "icon_font.h"
#include "fx_bridge.h"
#include "fx/fx.h"
#include "icons.h"
#include "swraster.h"
#include "platform/platform.h"
#include "ecs/ecs.h"
#include "physics/physics.h"
#include "script/runtime.h"
#include "audio/audio.h"
#include "scene_io.h"
#include "model_cache.h"
#include "model_import.h"
#include "core/vfs.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imguizmo/ImGuizmo.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Fonts compiled into the program (build/gen/fonts.cpp, from editor/fonts).
extern const unsigned char fe_font_inter_regular[];
extern const unsigned int fe_font_inter_regular_size;
extern const unsigned char fe_font_inter_medium[];
extern const unsigned int fe_font_inter_medium_size;
extern const unsigned char fe_font_roboto_mono[];
extern const unsigned int fe_font_roboto_mono_size;

namespace fe {

// ---------------------------------------------------------------- helpers

const char* path_filename(const char* path) {
    const char* s = path;
    for (const char* p = path; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') s = p + 1;
    return s;
}

const char* path_extension(const char* path) {
    const char* name = path_filename(path);
    const char* dot = nullptr;
    for (const char* p = name; *p; p++)
        if (*p == '.') dot = p;
    return dot ? dot : "";
}

static bool ext_is(const char* path, const char* const* list) {
    const char* e = path_extension(path);
    for (; *list; list++) {
        const char* a = e;
        const char* b = *list;
        while (*a && *b && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
        if (!*a && !*b) return true;
    }
    return false;
}

bool path_is_image(const char* p) { static const char* l[] = { ".png", ".jpg", ".jpeg", ".bmp", ".tga", nullptr }; return ext_is(p, l); }
bool path_is_shader(const char* p) { static const char* l[] = { ".fshader", nullptr }; return ext_is(p, l); }
bool path_is_model(const char* p) { static const char* l[] = { ".mof", ".gltf", ".glb", ".obj", nullptr }; return ext_is(p, l); }
bool path_is_script(const char* p) { static const char* l[] = { ".lua", ".cpp", ".h", nullptr }; return ext_is(p, l); }
bool path_is_scene(const char* p) { static const char* l[] = { ".fscene", nullptr }; return ext_is(p, l); }

const char* Settings::program_for(const char* path) const {
    const char* ext = path_extension(path);
    const char* fallback = nullptr;
    for (int i = 0; i < editor_count; i++) {
        if (!strcmp(editors[i].extension, "*")) { fallback = editors[i].program; continue; }
        const char* a = ext;
        const char* b = editors[i].extension;
        while (*a && *b && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
        if (!*a && !*b) return editors[i].program;
    }
    return fallback;
}

// ---------------------------------------------------------------- camera

Vec3 ViewCamera::eye() const {
    float y = radians(yaw), p = radians(pitch);
    Vec3 dir(fcos(p) * fsin(y), fsin(p), fcos(p) * fcos(y));
    return target + dir * distance;
}

Mat4 ViewCamera::view() const { return Mat4::look_at(eye(), target, Vec3(0, 1, 0)); }

Mat4 ViewCamera::projection(float aspect) const { return Mat4::perspective(radians(fov), aspect, 0.1f, 500.0f); }

// ---------------------------------------------------------------- theme
//
// Quiet and dense: panels are flat cards a few shades apart on a darker
// background, one accent color for selection and focus, small rounding,
// medium-weight type for headings. Every custom widget takes its colors
// from the palette, so the light theme follows.

namespace {
ImVec4 rgb(uint32_t hex, float a = 1.0f) {
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, a);
}
ImU32 rgbu(uint32_t hex, float a = 1.0f) { return ImGui::ColorConvertFloat4ToU32(rgb(hex, a)); }
}  // namespace

void Editor::apply_theme() {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.WindowPadding = ImVec2(10, 10);
    st.FramePadding = ImVec2(8, 4);
    st.ItemSpacing = ImVec2(6, 5);
    st.ItemInnerSpacing = ImVec2(6, 4);
    st.CellPadding = ImVec2(6, 4);
    st.IndentSpacing = 14;
    st.ScrollbarSize = 9;
    st.GrabMinSize = 9;
    // Docked panels: no outlines (they are cards on the darker background,
    // separated by gaps). Floating windows and dialogs add their own border
    // and shadow (begin_floating / begin_modal in widgets.cpp).
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 0;
    st.PopupBorderSize = 1;
    st.FrameBorderSize = 0;
    st.TabBorderSize = 0;
    st.TabBarBorderSize = 0;
    st.TabBarOverlineSize = 2;
    st.WindowRounding = 8;
    st.ChildRounding = 6;
    st.FrameRounding = 5;
    st.PopupRounding = 8;
    st.ScrollbarRounding = 6;
    st.GrabRounding = 4;
    st.TabRounding = 5;
    st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    st.WindowMenuButtonPosition = ImGuiDir_None;
    st.SeparatorTextBorderSize = 1;
    st.DockingSeparatorSize = 3;
    st.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    st.DisabledAlpha = 0.45f;
    st.AntiAliasedLines = true;
    st.AntiAliasedFill = true;

    Palette& p = pal;
    // One accent, the logo's blue.
    uint32_t accent = 0x4C8DFF;
    p.accent = rgbu(accent);
    p.axis[0] = rgbu(0xEF5B57);
    p.axis[1] = rgbu(0x72C64F);
    p.axis[2] = rgbu(0x4C8DFF);
    uint32_t bg0, bg1, bg2, bg3, bg4, head, border, text, dim, faint;
    if (settings.dark_theme) {
        // Neutral, slightly cool greys: background, panel, input, hover, pressed.
        bg0 = 0x0D0F12; bg1 = 0x16181D; bg2 = 0x1E2127; bg3 = 0x262A31; bg4 = 0x30353D;
        head = 0x1B1E23; border = 0x2A2E35; text = 0xE4E7EB; dim = 0x969DA7; faint = 0x626973;
        p.accent_soft = rgbu(accent, 0.20f);
        p.accent_text = rgbu(0x78AAFF);
        p.play = rgbu(0x3FBF7F);
        p.warning = rgbu(0xE8B04B);
        p.error = rgbu(0xEF6461);
        p.overlay = rgbu(0x1B1E23, 0.92f);
        p.shadow = rgbu(0x000000, 0.42f);
        p.float_bg = rgbu(0x1A1D22);
        p.float_border = rgbu(0x353A43);
        p.float_title = rgbu(0x1A1D22);
        p.sidebar = rgbu(0x15171B);
    } else {
        bg0 = 0xD5D9DF; bg1 = 0xF6F7F9; bg2 = 0xFFFFFF; bg3 = 0xEAECF0; bg4 = 0xDDE0E6;
        head = 0xEEF0F3; border = 0xD6DAE0; text = 0x1A1D22; dim = 0x5A616B; faint = 0x98A0A9;
        p.accent_soft = rgbu(accent, 0.16f);
        p.accent_text = rgbu(0x2F6FE4);
        p.play = rgbu(0x1E9E5E);
        p.warning = rgbu(0xC4860F);
        p.error = rgbu(0xD64541);
        p.overlay = rgbu(0xFFFFFF, 0.94f);
        p.shadow = rgbu(0x1A2030, 0.16f);
        p.float_bg = rgbu(0xFFFFFF);
        p.float_border = rgbu(0xC9CED6);
        p.float_title = rgbu(0xFFFFFF);
        p.sidebar = rgbu(0xF3F4F7);
    }
    p.app_bg = rgbu(bg0); p.panel = rgbu(bg1); p.frame = rgbu(bg2); p.hover = rgbu(bg3); p.active = rgbu(bg4);
    p.header = rgbu(head); p.border = rgbu(border); p.text = rgbu(text); p.text_dim = rgbu(dim); p.text_faint = rgbu(faint);
    set_ui_palette(pal, font_medium);

    ImVec4* c = st.Colors;
    ImVec4 acc = rgb(accent);
    bool dark = settings.dark_theme;
    c[ImGuiCol_Text] = rgb(text);
    c[ImGuiCol_TextDisabled] = rgb(faint);
    c[ImGuiCol_WindowBg] = rgb(bg1);
    c[ImGuiCol_ChildBg] = rgb(0, 0);
    // Menus, combo lists and tooltips float too: lifted, with an outline.
    c[ImGuiCol_PopupBg] = dark ? rgb(0x1A1D22) : rgb(0xFFFFFF);
    c[ImGuiCol_Border] = dark ? rgb(0x353A43) : rgb(0xC9CED6);
    c[ImGuiCol_BorderShadow] = rgb(0, 0);
    c[ImGuiCol_FrameBg] = rgb(bg2);
    c[ImGuiCol_FrameBgHovered] = rgb(bg3);
    c[ImGuiCol_FrameBgActive] = rgb(bg4);
    // A docked panel's tab strip is part of its card: the panel color.
    // (Floating windows set their own title colors, widgets.cpp.)
    c[ImGuiCol_TitleBg] = rgb(bg1);
    c[ImGuiCol_TitleBgActive] = rgb(bg1);
    c[ImGuiCol_TitleBgCollapsed] = rgb(bg1);
    c[ImGuiCol_MenuBarBg] = rgb(bg0);
    c[ImGuiCol_ScrollbarBg] = rgb(0, 0);
    c[ImGuiCol_ScrollbarGrab] = rgb(bg3);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(bg4);
    c[ImGuiCol_ScrollbarGrabActive] = dark ? rgb(0x454B54) : rgb(0xC3C8CF);
    c[ImGuiCol_CheckMark] = acc;
    c[ImGuiCol_SliderGrab] = acc;
    c[ImGuiCol_SliderGrabActive] = rgb(0x6EA3FF);
    c[ImGuiCol_Button] = rgb(bg3);
    c[ImGuiCol_ButtonHovered] = rgb(bg4);
    c[ImGuiCol_ButtonActive] = dark ? rgb(0x3E444D) : rgb(0xCDD1D8);
    c[ImGuiCol_Header] = rgb(accent, dark ? 0.20f : 0.16f);
    c[ImGuiCol_HeaderHovered] = dark ? rgb(0xFFFFFF, 0.05f) : rgb(0x000000, 0.045f);
    c[ImGuiCol_HeaderActive] = rgb(accent, 0.30f);
    c[ImGuiCol_Separator] = rgb(border);
    // The gaps between docked panels are the background itself.
    c[ImGuiCol_DockingEmptyBg] = rgb(bg0);
    c[ImGuiCol_SeparatorHovered] = acc;
    c[ImGuiCol_SeparatorActive] = acc;
    c[ImGuiCol_ResizeGrip] = rgb(0, 0);
    c[ImGuiCol_ResizeGripHovered] = rgb(accent, 0.5f);
    c[ImGuiCol_ResizeGripActive] = acc;
    c[ImGuiCol_InputTextCursor] = rgb(text);
    // Tabs: the selected one a step raised, the accent line on the focused panel only.
    c[ImGuiCol_Tab] = rgb(bg1, 0);
    c[ImGuiCol_TabHovered] = rgb(bg3);
    c[ImGuiCol_TabSelected] = rgb(bg2);
    c[ImGuiCol_TabSelectedOverline] = acc;
    c[ImGuiCol_TabDimmed] = rgb(bg1, 0);
    c[ImGuiCol_TabDimmedSelected] = rgb(bg2);
    c[ImGuiCol_TabDimmedSelectedOverline] = rgb(0, 0);
    c[ImGuiCol_DockingPreview] = rgb(accent, 0.40f);
    c[ImGuiCol_PlotLines] = acc;
    c[ImGuiCol_PlotLinesHovered] = rgb(0x6EA3FF);
    c[ImGuiCol_PlotHistogram] = acc;
    c[ImGuiCol_PlotHistogramHovered] = rgb(0x6EA3FF);
    c[ImGuiCol_TableHeaderBg] = rgb(head);
    c[ImGuiCol_TableBorderStrong] = rgb(border);
    c[ImGuiCol_TableBorderLight] = rgb(border, 0.6f);
    c[ImGuiCol_TableRowBg] = rgb(0, 0);
    c[ImGuiCol_TableRowBgAlt] = dark ? rgb(0xFFFFFF, 0.02f) : rgb(0x000000, 0.025f);
    c[ImGuiCol_TextLink] = acc;
    c[ImGuiCol_TextSelectedBg] = rgb(accent, 0.35f);
    c[ImGuiCol_TreeLines] = rgb(bg4);
    c[ImGuiCol_DragDropTarget] = acc;
    c[ImGuiCol_DragDropTargetBg] = rgb(accent, 0.12f);
    c[ImGuiCol_UnsavedMarker] = rgb(text);
    c[ImGuiCol_NavCursor] = acc;
    c[ImGuiCol_NavWindowingHighlight] = acc;
    c[ImGuiCol_NavWindowingDimBg] = rgb(0x000000, dark ? 0.35f : 0.20f);
    c[ImGuiCol_ModalWindowDimBg] = rgb(0x000000, dark ? 0.45f : 0.25f);
    c[ImGuiCol_CheckboxSelectedBg] = rgb(accent, 0.22f);
    st.ScaleAllSizes(settings.ui_scale);
}

// ---------------------------------------------------------------- init

bool Editor::init(int width, int height) {
    if (!platform::init(width, height)) return false;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.BackendRendererName = "farcade_swraster";
    io.BackendPlatformName = "farcade_platform";
    io.IniFilename = nullptr;               // the layout is the editor's default for now
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigDockingWithShift = false;
    io.DisplaySize = ImVec2((float)platform::width(), (float)platform::height());

    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    // Each UI font gets the vector icons merged in (FE_ICON_* characters).
    font_ui = io.Fonts->AddFontFromMemoryTTF((void*)fe_font_inter_regular, (int)fe_font_inter_regular_size, 14.0f * settings.ui_scale, &cfg);
    if (font_ui) icon_font_merge(io.Fonts);
    font_medium = io.Fonts->AddFontFromMemoryTTF((void*)fe_font_inter_medium, (int)fe_font_inter_medium_size, 14.0f * settings.ui_scale, &cfg);
    if (font_medium) icon_font_merge(io.Fonts);
    font_mono = io.Fonts->AddFontFromMemoryTTF((void*)fe_font_roboto_mono, (int)fe_font_roboto_mono_size, 13.0f * settings.ui_scale, &cfg);
    if (font_mono) icon_font_merge(io.Fonts);
    icon_font_add_box(io.Fonts);        // draw_icon: one icon per box
    if (!font_ui) font_ui = io.Fonts->AddFontDefault();
    if (!font_medium) font_medium = font_ui;
    if (!font_mono) font_mono = font_ui;
    io.FontDefault = font_ui;

    apply_theme();

    // Default file editors: text files in MOSdesk's editor.
    static const char* text_exts[] = { ".lua", ".txt", ".md", ".json", ".fscene", ".cfg", ".cpp", ".h" };
    for (const char* e : text_exts) {
        FileEditor& fe_ = settings.editors[settings.editor_count++];
        snprintf(fe_.extension, sizeof(fe_.extension), "%s", e);
        snprintf(fe_.program, sizeof(fe_.program), "%s", "0:/programs/edit.run");
    }

    snprintf(project_dir, sizeof(project_dir), "%s", platform::default_project_root());
    snprintf(files_dir, sizeof(files_dir), "%s", project_dir);
    load_project();
    // Models and other project files are read through the VFS: plain files under the project.
    {
        vfs::Disk disk;
        disk.read_file = platform::read_file;
        disk.file_exists = platform::file_exists;
        vfs::set_disk(disk);
        vfs::set_root(project_dir);
    }
    history_reset();
    log(LogLevel::Info, "Farcade Editor started (%dx%d)", platform::width(), platform::height());
    return true;
}

void Editor::shutdown() {
    delete play_backup_;
    play_backup_ = nullptr;
    for (int i = 0; i < undo_count_; i++) free(undo_[i]);
    for (int i = 0; i < redo_count_; i++) free(redo_[i]);
    undo_count_ = redo_count_ = 0;
    free(snap_);
    free(saved_snap_);
    snap_ = saved_snap_ = nullptr;
    ImGui::DestroyContext();
    platform::shutdown();
}

Editor::~Editor() {
    delete fx_bridge_;
    fx_bridge_ = nullptr;
    free_world();
    if (audio_device_) { audio_device_->shutdown(); delete audio_device_; }
    delete play_backup_;
}

// ---------------------------------------------------------------- UI rects

void Editor::track_rect(const char* key, float x, float y, float w, float h) {
    if (ui_rect_count >= kMaxUiRects) return;
    UiRect& r = ui_rects[ui_rect_count++];
    snprintf(r.key, sizeof(r.key), "%s", key);
    r.x = x; r.y = y; r.w = w; r.h = h;
}

void Editor::track(const char* key) {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    track_rect(key, a.x, a.y, b.x - a.x, b.y - a.y);
}

const Editor::UiRect* Editor::find_rect(const char* key) const {
    for (int i = ui_rect_count - 1; i >= 0; i--)
        if (!strcmp(ui_rects[i].key, key)) return &ui_rects[i];
    return nullptr;
}

// ---------------------------------------------------------------- log

void Editor::log(LogLevel level, const char* fmt, ...) {
    LogLine& l = log_lines[(log_head + log_count) % kLogLines];
    if (log_count == kLogLines) log_head = (log_head + 1) % kLogLines;
    else log_count++;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l.text, sizeof(l.text), fmt, ap);
    va_end(ap);
    l.level = level;
    l.time = platform::time_seconds();
    log_scroll_to_end = true;
}

// ---------------------------------------------------------------- actions

ObjectId Editor::create_object(const char* name, MeshKind mesh, const Vec3& at) {
    // Name it "Cube", "Cube 2", ...
    char unique[64];
    snprintf(unique, sizeof(unique), "%s", name);
    for (int n = 2;; n++) {
        bool taken = false;
        for (int i = 0; i < scene.count(); i++)
            if (!strcmp(scene.at(i).name, unique)) { taken = true; break; }
        if (!taken) break;
        snprintf(unique, sizeof(unique), "%s %d", name, n);
    }
    ObjectId id = scene.create(unique, mesh);
    if (Object* o = scene.get(id)) o->position = at;
    scene.select_only(id);
    return id;
}

void Editor::delete_selection() {
    // Removing an object removes its children too (Scene::remove).
    int n = 0;
    while (scene.selected_count()) {
        ObjectId id = scene.selected(0);
        if (!scene.has_selected_ancestor(id)) n++;
        scene.remove(id);
    }
    if (n) log(LogLevel::Info, "Deleted %d object%s", n, n == 1 ? "" : "s");
}

ObjectId Editor::duplicate_tree(ObjectId src_id, ObjectId parent, bool offset) {
    // Pairs (original, copy) of everything copied, so that script inputs
    // linking inside the copied tree link to the copies (a switch and its
    // door duplicated together stay a pair).
    ObjectId from[256], to[256];
    int pairs = 0;
    ObjectId id = duplicate_one(src_id, parent, offset, from, to, &pairs);
    for (int i = 0; i < pairs; i++) {
        Object* o = scene.get(to[i]);
        if (!o) continue;
        for (int s = 0; s < o->script_count; s++)
            for (int k = 0; k < o->scripts[s].prop_count; k++) {
                ScriptProp& p = o->scripts[s].props[k];
                if (p.type != PropType::Object) continue;
                for (int j = 0; j < pairs; j++)
                    if (p.object == from[j]) { p.object = to[j]; break; }
            }
    }
    return id;
}

ObjectId Editor::duplicate_one(ObjectId src_id, ObjectId parent, bool offset, ObjectId* from, ObjectId* to, int* pairs) {
    Object* src = scene.get(src_id);
    if (!src) return kNoObject;
    ObjectId id = create_object(src->name, src->mesh, src->position);
    src = scene.get(src_id);                    // objects are separate allocations, but look it up again anyway
    Object* o = scene.get(id);
    char name[64];
    memcpy(name, o->name, sizeof(name));
    *o = *src;                                  // everything but the id, the name and the parent
    o->id = id;
    memcpy(o->name, name, sizeof(name));
    o->parent = parent;
    if (offset) o->position = src->position + Vec3(0.5f, 0, 0.5f);
    // Only one object per kind is current: the copy is not.
    for (int k = 0; k < kModuleKindCount; k++)
        if (o->modules.has((ModuleKind)k)) set_current_flag(module_info((ModuleKind)k), o->modules.data((ModuleKind)k), false);
    if (*pairs < 256) { from[*pairs] = src_id; to[*pairs] = id; (*pairs)++; }
    // The children, in their order (the list grows while copying).
    int n = scene.count();
    ObjectId kids[256];
    int nk = 0;
    for (int i = 0; i < n && nk < 256; i++)
        if (scene.at(i).parent == src_id) kids[nk++] = scene.at(i).id;
    for (int k = 0; k < nk; k++) duplicate_one(kids[k], id, false, from, to, pairs);
    return id;
}

int Editor::add_script(ObjectId id, const char* path) {
    Object* o = scene.get(id);
    if (!o) return -1;
    int i = scene.add_script(id, path);
    if (i < 0) {
        log(LogLevel::Warning, "%s already has %d scripts (the most an object can hold)", o->name, kMaxScripts);
        return -1;
    }
    reload_script(id, i);
    return i;
}

char* Editor::read_script_text(const char* path, size_t* size) {
    char* text = (char*)platform::read_file(path, size);
    if (!text && project_dir[0] && path[0] != '/') {
        char full[600];
        snprintf(full, sizeof(full), "%s/%s", project_dir, path);
        text = (char*)platform::read_file(full, size);
    }
    return text;
}

bool Editor::reload_script(ObjectId id, int index) {
    Object* o = scene.get(id);
    if (!o || index < 0 || index >= o->script_count) return false;
    ScriptSlot& slot = o->scripts[index];
    // Paths from the Files panel are full; others are relative to the project.
    size_t size = 0;
    char* text = read_script_text(slot.path, &size);
    if (!text) {
        if (!slot.missing) log(LogLevel::Warning, "%s: cannot read %s; its inputs keep their values", o->name, slot.path);
        slot.missing = true;
        return false;
    }
    slot.missing = false;
    ScriptProp declared[kMaxProps];
    int bad = 0;
    int n = parse_script_props(text, (uint32_t)size, declared, kMaxProps, &bad);
    free(text);
    merge_script_props(&slot, declared, n);
    if (bad) log(LogLevel::Warning, "%s: %d @prop line%s could not be read", path_filename(slot.path), bad, bad == 1 ? "" : "s");
    return true;
}

int Editor::reload_all_scripts() {
    int failed = 0;
    for (int i = 0; i < scene.count(); i++) {
        Object& o = scene.at(i);
        for (int s = 0; s < o.script_count; s++) failed += !reload_script(o.id, s);
    }
    return failed;
}

void Editor::duplicate_selection() {
    int n = scene.selected_count();
    if (!n) return;
    ObjectId src_ids[64], new_ids[64];
    int ns = 0;
    for (int i = 0; i < n && ns < 64; i++)
        if (!scene.has_selected_ancestor(scene.selected(i))) src_ids[ns++] = scene.selected(i);
    int made = 0;
    for (int i = 0; i < ns; i++) {
        Object* src = scene.get(src_ids[i]);
        if (!src) continue;
        ObjectId id = duplicate_tree(src_ids[i], src->parent, true);
        if (id) new_ids[made++] = id;
    }
    scene.clear_selection();
    for (int i = 0; i < made; i++) scene.toggle(new_ids[i]);
    log(LogLevel::Info, "Duplicated %d object%s", made, made == 1 ? "" : "s");
}

ObjectId Editor::create_child(const char* name, MeshKind mesh, ObjectId parent) {
    ObjectId id = create_object(name, mesh, Vec3(0, 0, 0));
    if (Object* o = scene.get(id)) {
        o->parent = parent;
        o->position = Vec3(0, mesh == MeshKind::Plane ? 0.0f : 1.0f, 0);   // just above the parent's origin
        if (Object* p = scene.get(parent)) p->expanded = true;
    }
    return id;
}

ObjectId Editor::create_kind(const char* name, ObjectKind kind, const Vec3& at, ObjectId parent) {
    ObjectId id = create_object(name, MeshKind::Cube, at);
    scene.set_kind(id, kind);
    if (Object* o = scene.get(id)) {
        if (parent && scene.get(parent)) {
            o->parent = parent;
            o->position = Vec3(0, 1, 0);
            scene.get(parent)->expanded = true;
        }
        if (kind == ObjectKind::Camera) {
            // Looking at the middle of the scene from where the view is.
            o->rotation = Quat::euler_deg(Vec3(-15, 0, 0));
        }
        ModuleKind k = settings_of(kind);
        if (k != ModuleKind::Count && module_info(k).has_current)
            log(LogLevel::Info, "%s created%s", o->name, scene.is_current(id, k) ? " (current)" : "");
    }
    return id;
}

ObjectId Editor::create_light(LightType type, const Vec3& at, ObjectId parent) {
    static const char* names[] = { "Directional Light", "Point Light", "Spot Light" };
    ObjectId id = create_kind(names[(int)type], ObjectKind::Empty, at, parent);
    scene.add_module(id, ModuleKind::Light);
    if (Object* o = scene.get(id)) {
        o->modules.light.type = type;
        // Directional and spot lights point down (along their -Z, tipped over).
        if (type != LightType::Point) o->rotation = Quat::euler_deg(Vec3(type == LightType::Directional ? -50.0f : -90.0f, 30.0f, 0));
        if (type == LightType::Directional) o->modules.light.intensity = 2.0f;
    }
    return id;
}

void Editor::create_menu_items(ObjectId parent, const Vec3& at) {
    Vec3 ground(at.x, 0.5f, at.z), flat(at.x, 0, at.z);
    auto mesh = [&](const char* name, MeshKind k, const Vec3& p, const char* icon) {
        if (!menu_item(name, nullptr, false, true, icon)) return;
        if (parent) create_child(name, k, parent);
        else create_object(name, k, p);
    };
    mesh("Cube", MeshKind::Cube, ground, FE_ICON_CUBE);
    mesh("Sphere", MeshKind::Sphere, ground, FE_ICON_SPHERE);
    mesh("Cylinder", MeshKind::Cylinder, ground, FE_ICON_CYLINDER);
    mesh("Plane", MeshKind::Plane, flat, FE_ICON_PLANE);
    if (menu_item("Empty", nullptr, false, true, FE_ICON_EMPTY)) create_kind("Empty", ObjectKind::Empty, ground, parent);
    if (menu_item("Import model...", nullptr, false, true, FE_ICON_IMPORT))
        open_file_dialog(PickFor::ImportModel, PickRoot, "Import model", false, project_dir, ".gltf;.glb;.obj;.mof");
    ImGui::Separator();
    if (ImGui::BeginMenu(FE_ICON_BULB "  Light###Light")) {
        if (menu_item("Directional Light", nullptr, false, true, FE_ICON_LIGHT_DIRECTIONAL)) create_light(LightType::Directional, Vec3(at.x, 4, at.z), parent);
        if (menu_item("Point Light", nullptr, false, true, FE_ICON_LIGHT_POINT)) create_light(LightType::Point, Vec3(at.x, 2, at.z), parent);
        if (menu_item("Spot Light", nullptr, false, true, FE_ICON_LIGHT_SPOT)) create_light(LightType::Spot, Vec3(at.x, 3, at.z), parent);
        ImGui::EndMenu();
    }
    if (menu_item("Camera", nullptr, false, true, FE_ICON_CAMERA)) create_kind("Camera", ObjectKind::Camera, Vec3(at.x, 1.6f, at.z + 6), parent);
    ImGui::Separator();
    if (menu_item("Sky", nullptr, false, true, FE_ICON_CLOUD)) create_kind("Sky", ObjectKind::Sky, Vec3(at.x, 3, at.z), parent);
    if (menu_item("Sky Light", nullptr, false, true, FE_ICON_SUN)) create_kind("Sky Light", ObjectKind::SkyLight, Vec3(at.x, 3, at.z), parent);
    if (menu_item("Post Processing", nullptr, false, true, FE_ICON_WAND)) create_kind("Post Processing", ObjectKind::PostProcess, Vec3(at.x, 3, at.z), parent);
}

void Editor::frame_selection() {
    if (!scene.selected_count()) return;
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (int i = 0; i < scene.selected_count(); i++) {
        Object* o = scene.get(scene.selected(i));
        if (!o) continue;
        const MeshData& m = Scene::mesh_for(*o);
        Mat4 mm = scene.world_matrix(*o);
        for (int c = 0; c < 8; c++) {
            Vec3 p((c & 1) ? m.bounds_max.x : m.bounds_min.x, (c & 2) ? m.bounds_max.y : m.bounds_min.y,
                   (c & 4) ? m.bounds_max.z : m.bounds_min.z);
            p = transform_point(mm, p);
            lo = vmin(lo, p);
            hi = vmax(hi, p);
        }
    }
    camera.target = (lo + hi) * 0.5f;
    float r = length(hi - lo) * 0.5f;
    camera.distance = fmaxf_(2.0f, r / ftan(radians(camera.fov) * 0.5f) * 1.2f);
}

void Editor::set_view(float yaw, float pitch) {
    camera.yaw = yaw;
    camera.pitch = fclamp(pitch, -89.9f, 89.9f);
}

void Editor::play() {
    if (playing()) return;
    // Script inputs follow the files as they are now (the values set in the
    // editor stay).
    reload_all_scripts();
    model_cache_clear();                // model files may have changed
    play_backup_ = new Scene();
    play_backup_->copy_from(scene);
    play_time = 0.0;
    build_world();
    log(LogLevel::Info, "Play: %d entities, %d script%s running; the scene comes back on Stop", session_.entity_count(), session_.script_count(), session_.script_count() == 1 ? "" : "s");
}

void Editor::build_world() {
    free_world();
    // The device stays open between plays.
    if (!audio_device_) {
        audio_device_ = new AudioDevice();
        if (!audio_device_->init()) log(LogLevel::Warning, "Audio: no device (sound is mixed but not heard)");
    }
    PlayHost host;
    host.user = this;
    host.log = [](PlayLog level, const char* text, void* u) {
        ((Editor*)u)->log(level == PlayLog::Error ? LogLevel::Error : (level == PlayLog::Warning ? LogLevel::Warning : LogLevel::Info), "%s", text);
    };
    host.read_file = [](const char* path, size_t* size, void* u) -> void* { return ((Editor*)u)->read_script_text(path, size); };
    host.physics_event = [](const char* what, uint64_t a, uint64_t b, void* u) { ((Editor*)u)->physics_event(what, a, b); };
    // fe.graphics changes the live settings during Play; Stop restores them.
    play_gfx_backup_ = settings.graphics;
    host.graphics = &settings.graphics;
    host.graphics_changed = &gfx_script_changed_;
    session_.start(scene, host, audio_device_);
    session_.set_quality(fx_quality_of(settings.graphics));
}

FxBridge* Editor::fx() {
    if (!fx_bridge_) fx_bridge_ = new FxBridge();
    return fx_bridge_;
}

ObjectId Editor::object_of(uint64_t e) const { return session_.object_of(e); }

void Editor::physics_event(const char* what, uint64_t a, uint64_t b) {
    physics_event_count++;
    if (on_physics_event) { on_physics_event(this, what, a, b); return; }
    Object* oa = scene.get(object_of(a));
    Object* ob = scene.get(object_of(b));
    if (!oa || !ob) return;
    // "Ball entered Zone" / "Ball hit Crate"
    if (what[0] == 'h') log(LogLevel::Info, "Physics: %s hit %s", oa->name, ob->name);
    else log(LogLevel::Info, "Physics: %s %s trigger %s", ob->name, what, oa->name);
}

uint64_t Editor::entity_of(ObjectId id) const { return session_.entity_of(id); }

void Editor::step_world(float dt) {
    session_.set_quality(fx_quality_of(settings.graphics));
    session_.step(dt);
}

void Editor::free_world() { session_.stop(); }

void Editor::stop() {
    if (!playing()) return;
    ObjectId next = scene.next_id();
    free_world();
    settings.graphics = play_gfx_backup_;
    scene.copy_from(*play_backup_);
    scene.keep_ids_above(next);         // objects made while playing keep their ids retired
    delete play_backup_;
    play_backup_ = nullptr;
    log(LogLevel::Info, "Stop: scene restored (%.1f s of play)", play_time);
}

void Editor::project_path(const char* path, char* out, size_t cap) const {
    // Full paths: "/..." on the host, "0:/..." (a drive) on Minimal-OS.
    const char* colon = strchr(path, ':');
    bool full = path[0] == '/' || (colon && colon[1] == '/' && !memchr(path, '/', (size_t)(colon - path)));
    // Paths from the Files panel already start with the project folder.
    size_t pl = strlen(project_dir);
    bool in_project = pl && strncmp(path, project_dir, pl) == 0 && path[pl] == '/';
    if (full || in_project || !project_dir[0]) snprintf(out, cap, "%s", path);
    else snprintf(out, cap, "%s/%s", project_dir, path);
}

bool Editor::save_scene(const char* path) {
    char full[512];
    if (path && *path) project_path(path, full, sizeof full);
    else if (scene_path[0]) snprintf(full, sizeof full, "%s", scene_path);
    else project_path("scenes/main.fscene", full, sizeof full);
    // The folder it goes in (scenes/) is made when missing.
    if (const char* slash = strrchr(full, '/')) {
        char dir[512];
        snprintf(dir, sizeof dir, "%.*s", (int)(slash - full), full);
        if (dir[0] && !platform::file_exists(dir)) platform::make_dir(dir);
    }
    Scene& src = playing() ? *play_backup_ : scene;
    size_t size = 0;
    char* text = save_scene_text(src, project_dir, &size);
    if (!text) { log(LogLevel::Error, "Save: out of memory"); return false; }
    bool ok = platform::write_file(full, text, size);
    free(text);
    if (!ok) { log(LogLevel::Error, "Could not write %s", full); return false; }
    snprintf(scene_path, sizeof scene_path, "%s", full);
    free(saved_snap_);
    saved_snap_ = scene_snapshot(src);
    log(LogLevel::Info, "Saved %s (%d object%s)%s", path_filename(full), src.count(), src.count() == 1 ? "" : "s",
        playing() ? ", as it was before Play" : "");
    return true;
}

// ---------------------------------------------------------------- undo / redo

char* Editor::scene_snapshot(Scene& sc) {
    return save_scene_text(sc, project_dir, nullptr, kSceneTextHistory);
}

void Editor::history_capture() {
    if (playing()) return;
    char* now = scene_snapshot(scene);
    if (!now) return;
    if (snap_ && strcmp(now, snap_) == 0) { free(now); return; }
    if (snap_) {
        if (undo_count_ == kMaxUndo) {
            free(undo_[0]);
            memmove(undo_, undo_ + 1, sizeof(char*) * (kMaxUndo - 1));
            undo_count_--;
        }
        undo_[undo_count_++] = snap_;
        for (int i = 0; i < redo_count_; i++) free(redo_[i]);
        redo_count_ = 0;
    }
    snap_ = now;
}

void Editor::history_reset() {
    for (int i = 0; i < undo_count_; i++) free(undo_[i]);
    for (int i = 0; i < redo_count_; i++) free(redo_[i]);
    undo_count_ = redo_count_ = 0;
    free(snap_);
    free(saved_snap_);
    snap_ = scene_snapshot(scene);
    saved_snap_ = snap_ ? strdup(snap_) : nullptr;
}

bool Editor::scene_modified() {
    Scene& src = playing() ? *play_backup_ : scene;
    char* now = scene_snapshot(src);
    bool modified = now && (!saved_snap_ || strcmp(now, saved_snap_) != 0);
    free(now);
    return modified;
}

// Puts a snapshot back, keeping the selection (where the objects still
// exist), the hierarchy's folding and the id counter.
void Editor::restore_snapshot(const char* text) {
    Scene back;
    if (!load_scene_text(back, text, strlen(text), nullptr)) { log(LogLevel::Error, "Undo: the snapshot could not be read"); return; }
    for (int i = 0; i < back.count(); i++) {
        Object& o = back.at(i);
        if (Object* cur = scene.get(o.id)) {
            o.expanded = cur->expanded;
            // Script files are not read again; keep what the editor knows of them.
            for (int s = 0; s < o.script_count && s < cur->script_count; s++)
                if (!strcmp(o.scripts[s].path, cur->scripts[s].path)) o.scripts[s].missing = cur->scripts[s].missing;
        }
    }
    ObjectId sel[256];
    int ns = 0;
    ObjectId act = scene.active();
    for (int i = 0; i < scene.selected_count() && ns < 256; i++) if (scene.selected(i) != act) sel[ns++] = scene.selected(i);
    ObjectId next = scene.next_id();
    scene.copy_from(back);
    scene.keep_ids_above(next);
    scene.clear_selection();
    for (int i = 0; i < ns; i++) if (scene.get(sel[i])) scene.toggle(sel[i]);
    if (act && scene.get(act)) scene.toggle(act);       // the active one last, so it stays active
}

void Editor::undo() {
    if (playing()) return;
    history_capture();
    if (!undo_count_) return;
    if (redo_count_ == kMaxUndo) { free(redo_[0]); memmove(redo_, redo_ + 1, sizeof(char*) * (kMaxUndo - 1)); redo_count_--; }
    redo_[redo_count_++] = snap_;
    snap_ = undo_[--undo_count_];
    restore_snapshot(snap_);
    log(LogLevel::Info, "Undo");
}

void Editor::redo() {
    if (playing()) return;
    history_capture();
    if (!redo_count_) return;
    if (undo_count_ == kMaxUndo) { free(undo_[0]); memmove(undo_, undo_ + 1, sizeof(char*) * (kMaxUndo - 1)); undo_count_--; }
    undo_[undo_count_++] = snap_;
    snap_ = redo_[--redo_count_];
    restore_snapshot(snap_);
    log(LogLevel::Info, "Redo");
}

// ---------------------------------------------------------------- unsaved changes

void Editor::request_new_scene() { ask_or_do(Pending::NewScene, nullptr); }
void Editor::request_open_scene(const char* path) { ask_or_do(Pending::OpenScene, path); }
void Editor::request_quit() { ask_or_do(Pending::Quit, nullptr); }

void Editor::ask_or_do(Pending what, const char* path) {
    pending_ = what;
    snprintf(pending_path_, sizeof pending_path_, "%s", path ? path : "");
    if (scene_modified()) want_unsaved_prompt_ = true;
    else do_pending();
}

void Editor::do_pending() {
    Pending p = pending_;
    pending_ = Pending::None;
    if (p == Pending::NewScene) new_scene();
    else if (p == Pending::OpenScene) open_scene(pending_path_);
    else if (p == Pending::OpenProject) open_project(pending_path_);
    else if (p == Pending::Quit) quit = true;
}

void Editor::unsaved_popup() {
    if (want_unsaved_prompt_) { ImGui::OpenPopup("Unsaved changes"); want_unsaved_prompt_ = false; }
    ImGui::SetNextWindowSize(ImVec2(400 * settings.ui_scale, 0));
    if (!begin_modal("Unsaved changes", nullptr, ImGuiWindowFlags_NoResize)) return;
    const char* name = scene_path[0] ? path_filename(scene_path) : "the scene";
    ImGui::TextWrapped("Save the changes to %s first? Without saving they are lost.", name);
    ImGui::Dummy(ImVec2(0, 4));
    bool save = primary_button("Save");
    track("unsaved:save");
    ImGui::SameLine();
    bool discard = ImGui::Button("Don't save");
    track("unsaved:discard");
    ImGui::SameLine();
    bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    track("unsaved:cancel");
    if (save) {
        if (save_scene()) do_pending();
        else pending_ = Pending::None;          // the error is in the console; nothing is lost
    } else if (discard) {
        do_pending();
    } else if (cancel) {
        pending_ = Pending::None;
    }
    if (save || discard || cancel) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

bool Editor::open_scene(const char* path) {
    char full[512];
    project_path(path, full, sizeof full);
    size_t size = 0;
    char* text = (char*)platform::read_file(full, &size);
    if (!text) { log(LogLevel::Error, "Could not read %s", full); return false; }
    Scene loaded;
    SceneLoadReport rep;
    bool ok = load_scene_text(loaded, text, size, &rep);
    free(text);
    if (!ok) { log(LogLevel::Error, "%s: %s", path_filename(full), rep.error); return false; }
    stop();
    ObjectId next = scene.next_id();
    scene.copy_from(loaded);
    scene.keep_ids_above(next);         // ids are never handed out twice in a session
    scene.clear_selection();
    edit_collider = false;
    int failed = reload_all_scripts();  // inputs follow the files; saved values stay
    snprintf(scene_path, sizeof scene_path, "%s", full);
    history_reset();
    log(LogLevel::Info, "Opened %s (%d object%s)", path_filename(full), rep.objects, rep.objects == 1 ? "" : "s");
    if (rep.skipped) log(LogLevel::Warning, "%s: %d line%s skipped (first: line %d)", path_filename(full), rep.skipped, rep.skipped == 1 ? "" : "s", rep.first_bad_line);
    if (failed) log(LogLevel::Warning, "%d script%s could not be read", failed, failed == 1 ? "" : "s");
    return true;
}

void Editor::new_scene() {
    stop();
    scene.clear();
    edit_collider = false;
    scene_path[0] = 0;
    history_reset();
    log(LogLevel::Info, "New scene");
}

// File > Save scene as...: a name, saved under scenes/ in the project.
void Editor::save_as_popup() {
    if (want_save_as_) { ImGui::OpenPopup("Save scene as"); want_save_as_ = false; }
    ImGui::SetNextWindowSize(ImVec2(360 * settings.ui_scale, 0));
    if (!begin_modal("Save scene as", nullptr, ImGuiWindowFlags_NoResize)) return;
    ImGui::TextUnformatted("Name (saved in scenes/ as .fscene)");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    bool enter = ImGui::InputText("##save_as_name", save_as_name, sizeof save_as_name, ImGuiInputTextFlags_EnterReturnsTrue);
    track("save_as:name");
    bool valid = save_as_name[0] != 0 && !strchr(save_as_name, '/');
    ImGui::BeginDisabled(!valid);
    bool save = primary_button("Save") || (enter && valid);
    track("save_as:save");
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (save) {
        char rel[128];
        snprintf(rel, sizeof rel, "scenes/%s%s", save_as_name, path_is_scene(save_as_name) ? "" : ".fscene");
        save_scene(rel);
    }
    if (save || cancel) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Editor::open_file(const char* path) {
    if (path_is_scene(path)) { request_open_scene(path); return; }
    if (path_is_importable_model(path)) { import_model(path); return; }
    const char* program = settings.program_for(path);
    if (!program || !*program) {
        log(LogLevel::Warning, "No editor set for %s files (Settings > File editors)", path_extension(path));
        return;
    }
    if (platform::open_with(program, path)) log(LogLevel::Info, "Opened %s in %s", path_filename(path), path_filename(program));
    else log(LogLevel::Error, "Could not start %s", program);
}

void Editor::apply_file_to_object(const char* path, ObjectId id) {
    Object* o = scene.get(id);
    if (!o) return;
    if (path_is_image(path)) {
        snprintf(o->texture_path, sizeof(o->texture_path), "%s", path);
        log(LogLevel::Info, "%s: texture %s", o->name, path_filename(path));
    } else if (path_is_shader(path)) {
        snprintf(o->shader_path, sizeof(o->shader_path), "%s", path);
        log(LogLevel::Info, "%s: shader %s", o->name, path_filename(path));
    } else if (path_is_model(path)) {
        // .gltf/.glb/.obj are converted to a .mof first; the object then uses that.
        char mof[512], msg[1024];
        if (path_is_importable_model(path)) {
            char full[600];
            project_path(path, full, sizeof full);
            if (!model_convert_file(full, mof, sizeof mof, msg, sizeof msg)) {
                log(LogLevel::Error, "Import %s: %s", path_filename(path), msg);
                return;
            }
            if (msg[0]) log(LogLevel::Warning, "Import %s: %s", path_filename(path), msg);
            path = mof;
        }
        model_cache_clear();
        o->mesh = MeshKind::Model;
        snprintf(o->model_path, sizeof(o->model_path), "%s", path);
        log(LogLevel::Info, "%s: model %s", o->name, path_filename(path));
    } else if (path_is_script(path)) {
        int i = add_script(id, path);
        if (i >= 0) log(LogLevel::Info, "%s: added script %s (%d input%s)", o->name, path_filename(path), o->scripts[i].prop_count,
                        o->scripts[i].prop_count == 1 ? "" : "s");
    } else {
        log(LogLevel::Warning, "%s cannot be put on an object", path_filename(path));
    }
}

// ---------------------------------------------------------------- frame

bool Editor::frame() {
    ImGuiIO& io = ImGui::GetIO();
    static double last = platform::time_seconds();
    double now = platform::time_seconds();
    io.DeltaTime = now > last ? (float)(now - last) : 1.0f / 60.0f;
    if (io.DeltaTime > 0.25f) io.DeltaTime = 0.25f;
    last = now;

    platform::Event e;
    while (platform::poll(e)) {
        // Only these can change the scene (a plain mouse move cannot).
        if (e.type != platform::EventType::MouseMove) had_events_ = true;
        // Play: keyboard and mouse reach the scripts (fe.key_down ...); the mouse
        // in viewport pixels, buttons and wheel only over the viewport.
        if (playing() && !io.WantTextInput) {
            platform::Event f = e;
            bool over = e.x >= view_rect[0] && e.y >= view_rect[1] && e.x < view_rect[0] + view_rect[2] && e.y < view_rect[1] + view_rect[3];
            f.x -= view_rect[0];
            f.y -= view_rect[1];
            if (e.type == platform::EventType::Key || e.type == platform::EventType::MouseMove || over) session_.forward_event(f);
        }
        switch (e.type) {
            case platform::EventType::MouseMove: io.AddMousePosEvent(e.x, e.y); break;
            case platform::EventType::MouseButton:
                io.AddMousePosEvent(e.x, e.y);
                io.AddMouseButtonEvent(e.button, e.down);
                break;
            case platform::EventType::MouseWheel:
                io.AddMouseWheelEvent(settings.invert_scrolling ? -e.wheel_x : e.wheel_x,
                                      settings.invert_scrolling ? -e.wheel_y : e.wheel_y);
                break;
            case platform::EventType::Text: io.AddInputCharacter(e.codepoint); break;
            case platform::EventType::Key: {
                io.AddKeyEvent(ImGuiMod_Ctrl, (e.mods & platform::MOD_CTRL) != 0);
                io.AddKeyEvent(ImGuiMod_Shift, (e.mods & platform::MOD_SHIFT) != 0);
                io.AddKeyEvent(ImGuiMod_Alt, (e.mods & platform::MOD_ALT) != 0);
                ImGuiKey k = ImGuiKey_None;
                if (e.key >= platform::KEY_A && e.key < platform::KEY_A + 26) k = (ImGuiKey)(ImGuiKey_A + (e.key - platform::KEY_A));
                else if (e.key >= platform::KEY_0 && e.key < platform::KEY_0 + 10) k = (ImGuiKey)(ImGuiKey_0 + (e.key - platform::KEY_0));
                else switch (e.key) {
                    case platform::KEY_TAB: k = ImGuiKey_Tab; break;
                    case platform::KEY_LEFT: k = ImGuiKey_LeftArrow; break;
                    case platform::KEY_RIGHT: k = ImGuiKey_RightArrow; break;
                    case platform::KEY_UP: k = ImGuiKey_UpArrow; break;
                    case platform::KEY_DOWN: k = ImGuiKey_DownArrow; break;
                    case platform::KEY_PAGEUP: k = ImGuiKey_PageUp; break;
                    case platform::KEY_PAGEDOWN: k = ImGuiKey_PageDown; break;
                    case platform::KEY_HOME: k = ImGuiKey_Home; break;
                    case platform::KEY_END: k = ImGuiKey_End; break;
                    case platform::KEY_DELETE: k = ImGuiKey_Delete; break;
                    case platform::KEY_BACKSPACE: k = ImGuiKey_Backspace; break;
                    case platform::KEY_SPACE: k = ImGuiKey_Space; break;
                    case platform::KEY_ENTER: k = ImGuiKey_Enter; break;
                    case platform::KEY_ESCAPE: k = ImGuiKey_Escape; break;
                    case platform::KEY_LCTRL: k = ImGuiKey_LeftCtrl; break;
                    case platform::KEY_LSHIFT: k = ImGuiKey_LeftShift; break;
                    case platform::KEY_LALT: k = ImGuiKey_LeftAlt; break;
                    case platform::KEY_RCTRL: k = ImGuiKey_RightCtrl; break;
                    case platform::KEY_RSHIFT: k = ImGuiKey_RightShift; break;
                    case platform::KEY_RALT: k = ImGuiKey_RightAlt; break;
                    case platform::KEY_F1: k = ImGuiKey_F1; break;
                    case platform::KEY_F2: k = ImGuiKey_F2; break;
                    case platform::KEY_F3: k = ImGuiKey_F3; break;
                    case platform::KEY_F4: k = ImGuiKey_F4; break;
                    case platform::KEY_F5: k = ImGuiKey_F5; break;
                    case platform::KEY_F6: k = ImGuiKey_F6; break;
                    case platform::KEY_F7: k = ImGuiKey_F7; break;
                    case platform::KEY_F8: k = ImGuiKey_F8; break;
                    case platform::KEY_F9: k = ImGuiKey_F9; break;
                    case platform::KEY_F10: k = ImGuiKey_F10; break;
                    case platform::KEY_F11: k = ImGuiKey_F11; break;
                    case platform::KEY_F12: k = ImGuiKey_F12; break;
                    default: break;
                }
                if (k != ImGuiKey_None) io.AddKeyEvent(k, e.down);
                break;
            }
            case platform::EventType::Resize:
                break;
            case platform::EventType::Quit: quit = true; break;
            default: break;
        }
    }
    io.DisplaySize = ImVec2((float)platform::width(), (float)platform::height());
    if (playing()) {
        play_time += io.DeltaTime;
        step_world(io.DeltaTime);
    }
    fx()->update_preview(*this, io.DeltaTime);          // edit mode: the selected emitters preview

    ui_rect_count = 0;
    double t_build = platform::time_seconds();
    uint32_t renders_before = view_renders;
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    build_ui();
    ImGui::Render();
    // Undo: a snapshot once an edit has settled.
    if (had_events_ && !playing() && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(1) && !ImGui::IsMouseDown(2)) {
        history_capture();
        had_events_ = false;
    }
    double t_raster = platform::time_seconds();

    static SwRenderer renderer;
    renderer.set_verify(verify_damage);
    Framebuffer& fb = platform::framebuffer();
    SwRect damage[SwRenderer::kMaxDamage];
    int nd = renderer.render_incremental(ImGui::GetDrawData(), fb, settings.dark_theme ? 0x101215 : 0xD9DCE1, damage);
    damage_mismatches = renderer.verify_mismatches();
    raster_stats = renderer.stats();
    double t_present = platform::time_seconds();
    if (nd > 0) {
        // Rectangles close together go to the screen as one.
        SwRect all;
        int64_t sum = 0;
        for (int i = 0; i < nd; i++) {
            all = all.empty() ? damage[i] : SwRect{ damage[i].x0 < all.x0 ? damage[i].x0 : all.x0, damage[i].y0 < all.y0 ? damage[i].y0 : all.y0,
                                                    damage[i].x1 > all.x1 ? damage[i].x1 : all.x1, damage[i].y1 > all.y1 ? damage[i].y1 : all.y1 };
            sum += damage[i].area();
        }
        if (nd == 1 || sum * 3 > all.area() * 2) platform::present_region(all.x0, all.y0, all.x1 - all.x0, all.y1 - all.y0);
        else for (int i = 0; i < nd; i++) platform::present_region(damage[i].x0, damage[i].y0, damage[i].x1 - damage[i].x0, damage[i].y1 - damage[i].y0);
        perf.drawn++;
    } else {
        perf.skipped++;
    }
    last_frame_idle = nd == 0;
    double t_end = platform::time_seconds();
    perf.build += (t_raster - t_build) * 1000.0;
    perf.raster += (t_present - t_raster) * 1000.0;
    perf.present += (t_end - t_present) * 1000.0;
    if (view_renders != renders_before) { perf.view += view_ms; perf.view_renders++; }
    perf.frames++;
    return !quit;
}

// ---------------------------------------------------------------- layout

void Editor::build_ui() {
    main_menu();
    save_as_popup();
    unsaved_popup();
    project_settings_popup();
    export_popup();
    import_model_popup();
    file_dialog_popup(PickRoot);
    ImGuiWindow* menubar = ImGui::FindWindowByName("##MainMenuBar");
    float top = menubar ? menubar->Size.y : ImGui::GetFrameHeight();
    float bar_h = ImGui::GetTextLineHeight() + 10.0f * settings.ui_scale;
    status_bar(bar_h);
    dockspace(top, bar_h);
    panel_hierarchy();
    panel_inspector();
    panel_viewport();
    // After a layout reset, Files is the tab shown in the bottom dock.
    static int focus_files = 2;
    if (want_reset_layout) focus_files = 2;
    panel_console();
    if (focus_files > 0 && --focus_files == 0) ImGui::SetNextWindowFocus();
    panel_files();
    if (show_settings) window_settings();
    if (show_demo) ImGui::ShowDemoWindow(&show_demo);
    shortcuts();
}

// A menu item that also records where it was drawn ("menu:<label>").
bool Editor::menu_item(const char* label, const char* shortcut, bool selected, bool enabled, const char* icon) {
    // The icon, two spaces, the label; the id stays the label's.
    char text[96];
    if (icon) snprintf(text, sizeof(text), "%s  %s###%s", icon, label, label);
    bool r = ImGui::MenuItem(icon ? text : label, shortcut, selected, enabled);
    char key[48];
    snprintf(key, sizeof(key), "menu:%s", label);
    track(key);
    return r;
}

void Editor::main_menu() {
    // The top bar: the mark and the menus on the left, the project in the
    // middle, run controls and the renderer on the right.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4 + 4.0f * settings.ui_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 6));
    bool open = ImGui::BeginMainMenuBar();
    ImGui::PopStyleVar(2);
    if (!open) return;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 6));
    ImGuiWindow* bar = ImGui::GetCurrentWindow();
    float bar_h = bar->Size.y;
    float fs = ImGui::GetFontSize();

    // The mark.
    ImVec2 lp = ImGui::GetCursorScreenPos();
    float ls = fs + 4;
    draw_logo(ImGui::GetWindowDrawList(), ImVec2(lp.x + 2, bar->Pos.y + (bar_h - ls) * 0.5f), ls, pal.accent);
    ImGui::Dummy(ImVec2(ls + 8, 1));
    ImGui::SameLine();

    bool file_menu = ImGui::BeginMenu("File");
    track("menubar:File");
    if (file_menu) {
        if (menu_item("New scene", nullptr, false, true, FE_ICON_FILE)) request_new_scene();
        if (menu_item("Open project...", nullptr, false, !playing(), FE_ICON_FOLDER_OPEN))
            open_file_dialog(PickFor::OpenProject, PickRoot, "Open project", true, project_dir, "");
        if (menu_item("Save scene", "Ctrl+S", false, true, FE_ICON_SAVE)) save_scene();
        if (menu_item("Save scene as...", nullptr, false, true, FE_ICON_COPY)) {
            const char* cur = scene_path[0] ? path_filename(scene_path) : "main.fscene";
            snprintf(save_as_name, sizeof save_as_name, "%.*s", (int)(strlen(cur) - (path_is_scene(cur) ? 7 : 0)), cur);
            want_save_as_ = true;
        }
        ImGui::Separator();
        if (menu_item("Project settings...", nullptr, false, !playing(), FE_ICON_SETTINGS_SLIDERS)) request_project_settings();
        if (menu_item("Export game...", nullptr, false, !playing(), FE_ICON_EXPORT)) request_export();
        ImGui::Separator();
        if (menu_item("Quit", nullptr, false, true, FE_ICON_CLOSE)) request_quit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (menu_item("Undo", "Ctrl+Z", false, can_undo() && !playing(), FE_ICON_UNDO)) undo();
        if (menu_item("Redo", "Ctrl+Y", false, can_redo() && !playing(), FE_ICON_REDO)) redo();
        ImGui::Separator();
        if (menu_item("Duplicate", "Ctrl+D", false, scene.selected_count() > 0, FE_ICON_DUPLICATE)) duplicate_selection();
        if (menu_item("Delete", "Del", false, scene.selected_count() > 0, FE_ICON_TRASH)) delete_selection();
        ImGui::Separator();
        if (menu_item("Settings...", nullptr, false, true, FE_ICON_GEAR)) show_settings = true;
        ImGui::EndMenu();
    }
    bool create_menu = ImGui::BeginMenu("Create");
    track("menubar:Create");
    if (create_menu) {
        create_menu_items(kNoObject, camera.target);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (menu_item("Frame selection", "F", false, scene.selected_count() > 0, FE_ICON_FULLSCREEN)) frame_selection();
        if (menu_item("Grid", "G", settings.show_grid, true, FE_ICON_GRID)) settings.show_grid = !settings.show_grid;
        ImGui::Separator();
        if (menu_item("Reset layout", nullptr, false, true, FE_ICON_LAYERS)) want_reset_layout = true;
        if (menu_item("Dark theme", nullptr, settings.dark_theme, true, FE_ICON_PALETTE)) { settings.dark_theme = !settings.dark_theme; apply_theme(); }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (menu_item("ImGui demo", nullptr, show_demo, true, FE_ICON_HELP)) show_demo = !show_demo;
        ImGui::EndMenu();
    }

    // The project, centered.
    {
        const char* name = path_filename(project_dir);
        char title[160];
        // "project · scene" (the scene file without .fscene).
        const char* sn = scene_path[0] ? path_filename(scene_path) : "";
        int sl = (int)strlen(sn) - (path_is_scene(sn) ? 7 : 0);
        if (sl > 0) snprintf(title, sizeof(title), "%s  \xC2\xB7  %.*s", *name ? name : "No project", sl, sn);
        else snprintf(title, sizeof(title), "%s", *name ? name : "No project");
        float tw = ImGui::CalcTextSize(title).x;
        float x = (bar->Size.x - tw) * 0.5f;
        if (x > ImGui::GetCursorPosX() + 20) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float ty = bar->Pos.y + (bar_h - fs) * 0.5f;
            if (font_medium) ImGui::PushFont(font_medium, 0.0f);
            dl->AddText(ImVec2(bar->Pos.x + x, ty), pal.text_dim, title);
            if (font_medium) ImGui::PopFont();
        }
    }
    run_controls();
    ImGui::PopStyleVar(2);
    ImGui::EndMainMenuBar();
}

void Editor::run_controls() {
    // Right side of the top bar: [Play Stop]  [renderer chip]  [settings].
    ImGuiWindow* bar = ImGui::GetCurrentWindow();
    float bar_h = bar->Size.y;
    float bs = ImGui::GetFrameHeight();
    char chip_label[64];
    snprintf(chip_label, sizeof(chip_label), "%s  %.1f ms", view_renderer == ViewRenderer::FarcadeGL ? "FarcadeGL" : "CPU preview", view_ms);
    float chip_w = ImGui::CalcTextSize("CPU preview  000.0 ms").x + bs + 14;
    float group_w = bs * 2 + 8;
    float total = group_w + 10 + chip_w + 6 + bs + 8;
    float x = bar->Size.x - total;
    if (x < ImGui::GetCursorPosX() + 10) return;
    float y = (bar_h - bs) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Run group on a pill.
    ImVec2 gp(bar->Pos.x + x, bar->Pos.y + y);
    dl->AddRectFilled(ImVec2(gp.x - 1, gp.y - 1), ImVec2(gp.x + group_w + 1, gp.y + bs + 1), pal.frame, ImGui::GetStyle().FrameRounding + 1);
    ImGui::SetCursorPos(ImVec2(x + 2, y));
    if (playing()) {
        ImVec2 pp = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(pp, ImVec2(pp.x + bs, pp.y + bs), (pal.play & 0x00FFFFFF) | 0x40000000, ImGui::GetStyle().FrameRounding);
    }
    if (icon_button(Icon::Play, playing() ? "Playing (Stop puts the scene back)" : "Play the scene (F5)", false, bs, pal.play))
        if (!playing()) play();
    track("run:play");
    ImGui::SameLine(0, 2);
    ImGui::SetCursorPosY(y);
    ImGui::BeginDisabled(!playing());
    if (icon_button(Icon::Stop, "Stop and restore the scene (F5)", false, bs)) stop();
    ImGui::EndDisabled();
    track("run:stop");

    // The renderer chip: what draws the viewport, with its settings.
    ImGui::SameLine(0, 10);
    ImGui::SetCursorPosY(y);
    ImVec2 cp = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##renderer", ImVec2(chip_w, bs))) ImGui::OpenPopup("##renderer_popup");
    track("run:renderer");
    bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(cp, ImVec2(cp.x + chip_w, cp.y + bs), hov ? pal.hover : pal.frame, ImGui::GetStyle().FrameRounding);
    float fs = ImGui::GetFontSize();
    draw_icon(dl, Icon::Monitor, ImVec2(cp.x + 8, cp.y + (bs - fs) * 0.5f), fs, pal.accent_text);
    dl->AddText(ImVec2(cp.x + 14 + fs, cp.y + (bs - fs) * 0.5f), pal.text, chip_label);
    draw_icon(dl, Icon::ChevronDown, ImVec2(cp.x + chip_w - fs - 4, cp.y + (bs - fs) * 0.5f), fs * 0.9f, pal.text_faint);
    if (hov) ImGui::SetItemTooltip("Viewport renderer and display mode");
    ImGui::SetNextWindowPos(ImVec2(cp.x + chip_w, cp.y + bs + 4), ImGuiCond_Always, ImVec2(1, 0));
    if (ImGui::BeginPopup("##renderer_popup")) {
        heading("Viewport renderer");
        if (ImGui::RadioButton("CPU preview", view_renderer == ViewRenderer::CpuPreview)) view_renderer = ViewRenderer::CpuPreview;
        track("menu:CPU preview");
        ImGui::SameLine();
        ImGui::TextDisabled("grid, outline, shadows");
        if (ImGui::RadioButton("FarcadeGL", view_renderer == ViewRenderer::FarcadeGL)) view_renderer = ViewRenderer::FarcadeGL;
        track("menu:FarcadeGL");
        ImGui::SameLine();
        ImGui::TextDisabled("lights, sky, post processing");
        ImGui::Checkbox("Play through the current camera", &settings.play_through_camera);
        track("menu:Play camera");
        ImGui::Separator();
        ImGui::TextDisabled("Display");
        if (ImGui::RadioButton("Lit", view_mode == ViewMode::Lit)) view_mode = ViewMode::Lit;
        track("menu:Lit");
        if (ImGui::RadioButton("Unlit", view_mode == ViewMode::Unlit)) view_mode = ViewMode::Unlit;
        track("menu:Unlit");
        if (ImGui::RadioButton("Wireframe", view_mode == ViewMode::Wireframe)) view_mode = ViewMode::Wireframe;
        track("menu:Wireframe");
        ImGui::Separator();
        ImGui::TextDisabled("Quality");
        ImGui::Checkbox("Smooth edges (2x2 supersampling)", &settings.view_supersample);
        track("menu:Smooth edges");
        ImGui::Checkbox("Ground shadows", &settings.view_shadows);
        track("menu:Ground shadows");
        ImGui::Checkbox("Grid", &settings.show_grid);
        ImGui::Separator();
        ImGui::TextDisabled("%d x %d   %d triangles   %.2f ms", (int)view_rect[2], (int)view_rect[3], view_triangles, view_ms);
        ImGui::EndPopup();
    }

    ImGui::SameLine(0, 6);
    ImGui::SetCursorPosY(y);
    if (icon_button(Icon::Gear, "Settings", show_settings, bs)) show_settings = !show_settings;
    track("run:settings");
}

void Editor::status_bar(float h) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - h));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, h));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoNavFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(pal.app_bg));
    ImGui::Begin("##statusbar", nullptr, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetWindowPos();
    float fs = ImGui::GetFontSize();
    float ty = p.y + (h - fs) * 0.5f;
    float x = p.x + 10;
    if (playing()) {
        // A colored segment while playing.
        char t[48];
        snprintf(t, sizeof(t), "Playing  %d:%02d", (int)play_time / 60, (int)play_time % 60);
        float w = ImGui::CalcTextSize(t).x + fs + 22;
        dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y + h), pal.play);
        draw_icon(dl, Icon::Play, ImVec2(x, ty), fs, IM_COL32(255, 255, 255, 255));
        dl->AddText(ImVec2(x + fs + 6, ty), IM_COL32(255, 255, 255, 255), t);
        x = p.x + w + 10;
    }
    // The newest log line.
    if (log_count > 0) {
        const LogLine& l = log_lines[(log_head + log_count - 1) % kLogLines];
        static const Icon icons[3] = { Icon::Info, Icon::Warning, Icon::Error };
        ImU32 col = l.level == LogLevel::Error ? pal.error : (l.level == LogLevel::Warning ? pal.warning : pal.text_faint);
        draw_icon(dl, icons[(int)l.level], ImVec2(x, ty + 1), fs - 2, col);
        dl->PushClipRect(ImVec2(x, p.y), ImVec2(p.x + vp->Size.x * 0.6f, p.y + h), true);
        dl->AddText(ImVec2(x + fs + 4, ty), pal.text_dim, l.text);
        dl->PopClipRect();
    }
    // Right side: counts and the renderer.
    char right[128];
    const char* space = tool == Tool::Select ? "" : (gizmo_local || tool == Tool::Scale) ? ", local axes" : ", world axes";
    snprintf(right, sizeof(right), "%d object%s   %d selected   |   %s%s   |   %d x %d", scene.count(), scene.count() == 1 ? "" : "s",
             scene.selected_count(), tool == Tool::Select ? "Select" : tool == Tool::Move ? "Move" : tool == Tool::Rotate ? "Rotate" : "Scale",
             space, platform::width(), platform::height());
    float rw = ImGui::CalcTextSize(right).x;
    dl->AddText(ImVec2(p.x + vp->Size.x - rw - 12, ty), pal.text_faint, right);
    ImGui::End();
}

void Editor::dockspace(float top, float bottom) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 pos(vp->Pos.x, vp->Pos.y + top), size(vp->Size.x, vp->Size.y - top - bottom);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 2));
    ImGui::Begin("##dockhost", nullptr, flags);
    ImGui::PopStyleVar();
    ImGuiID id = ImGui::GetID("MainDock");
    if (want_reset_layout) {
        want_reset_layout = false;
        ImGui::DockBuilderRemoveNode(id);
        ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(id, ImVec2(size.x - 8, size.y - 4));
        ImGuiID center = id, left, right, bottom_id;
        left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.17f, nullptr, &center);
        right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
        bottom_id = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
        ImGui::DockBuilderDockWindow("###Hierarchy", left);
        ImGui::DockBuilderDockWindow("###Inspector", right);
        ImGui::DockBuilderDockWindow("###Files", bottom_id);
        ImGui::DockBuilderDockWindow("###Console", bottom_id);
        ImGui::DockBuilderDockWindow("Viewport", center);
        if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(center)) n->LocalFlags |= ImGuiDockNodeFlags_HiddenTabBar;
        ImGui::DockBuilderFinish(id);
    }
    ImGui::DockSpace(id, ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();
}

void Editor::shortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false) && !platform::toggle_fullscreen())
        log(LogLevel::Warning, "Could not toggle fullscreen");
    if (io.WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) tool = Tool::Select;
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) tool = Tool::Move;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) tool = Tool::Rotate;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) tool = Tool::Scale;
    if (ImGui::IsKeyPressed(ImGuiKey_G, false) && !io.KeyCtrl) settings.show_grid = !settings.show_grid;
    if (ImGui::IsKeyPressed(ImGuiKey_X, false) && !io.KeyCtrl) gizmo_local = !gizmo_local;
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) frame_selection();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) delete_selection();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicate_selection();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) save_scene();
    if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, true)) undo();
    if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Y, true) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, true)))) redo();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) scene.clear_selection();
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) { if (playing()) stop(); else play(); }
}

}  // namespace fe
