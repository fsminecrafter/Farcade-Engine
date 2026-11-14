#include "editor.h"
#include "icons.h"
#include "swraster.h"
#include "platform/platform.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imguizmo/ImGuizmo.h"

#include <stdarg.h>
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
bool path_is_model(const char* p) { static const char* l[] = { ".mof", ".gltf", ".glb", ".obj", nullptr }; return ext_is(p, l); }
bool path_is_script(const char* p) { static const char* l[] = { ".lua", ".cpp", ".h", nullptr }; return ext_is(p, l); }

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

Vec3 Camera::eye() const {
    float y = radians(yaw), p = radians(pitch);
    Vec3 dir(fcos(p) * fsin(y), fsin(p), fcos(p) * fcos(y));
    return target + dir * distance;
}

Mat4 Camera::view() const { return Mat4::look_at(eye(), target, Vec3(0, 1, 0)); }

Mat4 Camera::projection(float aspect) const { return Mat4::perspective(radians(fov), aspect, 0.1f, 500.0f); }

// ---------------------------------------------------------------- theme
//
// Modern and quiet: flat surfaces a few shades apart instead of borders,
// one accent color for selection and focus, rounded corners, room around
// things.

void Editor::apply_theme() {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.WindowPadding = ImVec2(10, 10);
    st.FramePadding = ImVec2(8, 5);
    st.ItemSpacing = ImVec2(8, 6);
    st.ItemInnerSpacing = ImVec2(6, 4);
    st.CellPadding = ImVec2(6, 4);
    st.IndentSpacing = 14;
    st.ScrollbarSize = 10;
    st.GrabMinSize = 10;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 0;
    st.PopupBorderSize = 1;
    st.FrameBorderSize = 0;
    st.TabBorderSize = 0;
    st.TabBarBorderSize = 0;
    st.WindowRounding = 8;
    st.ChildRounding = 6;
    st.FrameRounding = 6;
    st.PopupRounding = 8;
    st.ScrollbarRounding = 6;
    st.GrabRounding = 6;
    st.TabRounding = 6;
    st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    st.WindowMenuButtonPosition = ImGuiDir_None;
    st.SeparatorTextBorderSize = 1;
    st.DockingSeparatorSize = 3;
    st.AntiAliasedLines = true;
    st.AntiAliasedFill = true;

    ImVec4* c = st.Colors;
    auto rgb = [](int hex, float a = 1.0f) {
        return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, a);
    };
    ImVec4 accent = rgb(0x4C8DFF);
    if (settings.dark_theme) {
        ImVec4 bg0 = rgb(0x16171A), bg1 = rgb(0x1E1F23), bg2 = rgb(0x26282D), bg3 = rgb(0x2F3238), bg4 = rgb(0x3A3D44);
        c[ImGuiCol_Text] = rgb(0xE6E7EA);
        c[ImGuiCol_TextDisabled] = rgb(0x7D828C);
        c[ImGuiCol_WindowBg] = bg1;
        c[ImGuiCol_ChildBg] = rgb(0x000000, 0);
        c[ImGuiCol_PopupBg] = bg2;
        c[ImGuiCol_Border] = rgb(0x34373D);
        c[ImGuiCol_BorderShadow] = rgb(0x000000, 0);
        c[ImGuiCol_FrameBg] = bg2;
        c[ImGuiCol_FrameBgHovered] = bg3;
        c[ImGuiCol_FrameBgActive] = bg4;
        c[ImGuiCol_TitleBg] = bg0;
        c[ImGuiCol_TitleBgActive] = bg0;
        c[ImGuiCol_TitleBgCollapsed] = bg0;
        c[ImGuiCol_MenuBarBg] = bg0;
        c[ImGuiCol_ScrollbarBg] = rgb(0x000000, 0);
        c[ImGuiCol_ScrollbarGrab] = bg3;
        c[ImGuiCol_ScrollbarGrabHovered] = bg4;
        c[ImGuiCol_ScrollbarGrabActive] = rgb(0x4A4E57);
        c[ImGuiCol_CheckMark] = accent;
        c[ImGuiCol_SliderGrab] = accent;
        c[ImGuiCol_SliderGrabActive] = rgb(0x6AA1FF);
        c[ImGuiCol_Button] = bg2;
        c[ImGuiCol_ButtonHovered] = bg3;
        c[ImGuiCol_ButtonActive] = bg4;
        c[ImGuiCol_Header] = rgb(0x4C8DFF, 0.22f);
        c[ImGuiCol_HeaderHovered] = rgb(0xFFFFFF, 0.06f);
        c[ImGuiCol_HeaderActive] = rgb(0x4C8DFF, 0.32f);
        c[ImGuiCol_Separator] = rgb(0x2C2E33);
        c[ImGuiCol_SeparatorHovered] = accent;
        c[ImGuiCol_SeparatorActive] = accent;
        c[ImGuiCol_ResizeGrip] = rgb(0x000000, 0);
        c[ImGuiCol_ResizeGripHovered] = rgb(0x4C8DFF, 0.5f);
        c[ImGuiCol_ResizeGripActive] = accent;
        c[ImGuiCol_Tab] = bg0;
        c[ImGuiCol_TabHovered] = bg2;
        c[ImGuiCol_TabSelected] = bg1;
        c[ImGuiCol_TabSelectedOverline] = accent;
        c[ImGuiCol_TabDimmed] = bg0;
        c[ImGuiCol_TabDimmedSelected] = bg1;
        c[ImGuiCol_TabDimmedSelectedOverline] = rgb(0x000000, 0);
        c[ImGuiCol_DockingPreview] = rgb(0x4C8DFF, 0.45f);
        c[ImGuiCol_DockingEmptyBg] = bg0;
        c[ImGuiCol_TableHeaderBg] = bg2;
        c[ImGuiCol_TableBorderStrong] = rgb(0x2C2E33);
        c[ImGuiCol_TableBorderLight] = rgb(0x25272B);
        c[ImGuiCol_TableRowBg] = rgb(0x000000, 0);
        c[ImGuiCol_TableRowBgAlt] = rgb(0xFFFFFF, 0.02f);
        c[ImGuiCol_TextSelectedBg] = rgb(0x4C8DFF, 0.35f);
        c[ImGuiCol_DragDropTarget] = accent;
        c[ImGuiCol_NavCursor] = accent;
        c[ImGuiCol_ModalWindowDimBg] = rgb(0x000000, 0.45f);
        c[ImGuiCol_CheckboxSelectedBg] = rgb(0x4C8DFF, 0.25f);
        c[ImGuiCol_InputTextCursor] = rgb(0xE6E7EA);
        c[ImGuiCol_TextLink] = accent;
        c[ImGuiCol_TreeLines] = rgb(0x3A3D44);
        c[ImGuiCol_DragDropTargetBg] = rgb(0x4C8DFF, 0.12f);
        c[ImGuiCol_UnsavedMarker] = rgb(0xE6E7EA);
        c[ImGuiCol_NavWindowingHighlight] = accent;
        c[ImGuiCol_NavWindowingDimBg] = rgb(0x000000, 0.35f);
        c[ImGuiCol_PlotLines] = accent;
        c[ImGuiCol_PlotLinesHovered] = rgb(0x6AA1FF);
        c[ImGuiCol_PlotHistogram] = accent;
        c[ImGuiCol_PlotHistogramHovered] = rgb(0x6AA1FF);
    } else {
        ImVec4 bg0 = rgb(0xE9EAEE), bg1 = rgb(0xF6F7F9), bg2 = rgb(0xFFFFFF), bg3 = rgb(0xE4E6EA), bg4 = rgb(0xD7DAE0);
        c[ImGuiCol_Text] = rgb(0x1E2024);
        c[ImGuiCol_TextDisabled] = rgb(0x8A8F99);
        c[ImGuiCol_WindowBg] = bg1;
        c[ImGuiCol_ChildBg] = rgb(0x000000, 0);
        c[ImGuiCol_PopupBg] = bg2;
        c[ImGuiCol_Border] = rgb(0xD5D8DE);
        c[ImGuiCol_BorderShadow] = rgb(0x000000, 0);
        c[ImGuiCol_FrameBg] = bg2;
        c[ImGuiCol_FrameBgHovered] = bg3;
        c[ImGuiCol_FrameBgActive] = bg4;
        c[ImGuiCol_TitleBg] = bg0;
        c[ImGuiCol_TitleBgActive] = bg0;
        c[ImGuiCol_TitleBgCollapsed] = bg0;
        c[ImGuiCol_MenuBarBg] = bg0;
        c[ImGuiCol_ScrollbarBg] = rgb(0x000000, 0);
        c[ImGuiCol_ScrollbarGrab] = bg4;
        c[ImGuiCol_ScrollbarGrabHovered] = rgb(0xC5C9D0);
        c[ImGuiCol_ScrollbarGrabActive] = rgb(0xB3B8C1);
        c[ImGuiCol_CheckMark] = accent;
        c[ImGuiCol_SliderGrab] = accent;
        c[ImGuiCol_SliderGrabActive] = rgb(0x2F75F0);
        c[ImGuiCol_Button] = bg3;
        c[ImGuiCol_ButtonHovered] = bg4;
        c[ImGuiCol_ButtonActive] = rgb(0xC9CDD4);
        c[ImGuiCol_Header] = rgb(0x4C8DFF, 0.18f);
        c[ImGuiCol_HeaderHovered] = rgb(0x000000, 0.05f);
        c[ImGuiCol_HeaderActive] = rgb(0x4C8DFF, 0.28f);
        c[ImGuiCol_Separator] = rgb(0xDDE0E5);
        c[ImGuiCol_SeparatorHovered] = accent;
        c[ImGuiCol_SeparatorActive] = accent;
        c[ImGuiCol_ResizeGrip] = rgb(0x000000, 0);
        c[ImGuiCol_ResizeGripHovered] = rgb(0x4C8DFF, 0.5f);
        c[ImGuiCol_ResizeGripActive] = accent;
        c[ImGuiCol_Tab] = bg0;
        c[ImGuiCol_TabHovered] = bg2;
        c[ImGuiCol_TabSelected] = bg1;
        c[ImGuiCol_TabSelectedOverline] = accent;
        c[ImGuiCol_TabDimmed] = bg0;
        c[ImGuiCol_TabDimmedSelected] = bg1;
        c[ImGuiCol_TabDimmedSelectedOverline] = rgb(0x000000, 0);
        c[ImGuiCol_DockingPreview] = rgb(0x4C8DFF, 0.35f);
        c[ImGuiCol_DockingEmptyBg] = bg0;
        c[ImGuiCol_TableHeaderBg] = bg3;
        c[ImGuiCol_TableBorderStrong] = rgb(0xDDE0E5);
        c[ImGuiCol_TableBorderLight] = rgb(0xE6E8EC);
        c[ImGuiCol_TableRowBg] = rgb(0x000000, 0);
        c[ImGuiCol_TableRowBgAlt] = rgb(0x000000, 0.025f);
        c[ImGuiCol_TextSelectedBg] = rgb(0x4C8DFF, 0.3f);
        c[ImGuiCol_DragDropTarget] = accent;
        c[ImGuiCol_NavCursor] = accent;
        c[ImGuiCol_ModalWindowDimBg] = rgb(0x000000, 0.25f);
        c[ImGuiCol_CheckboxSelectedBg] = rgb(0x4C8DFF, 0.18f);
        c[ImGuiCol_InputTextCursor] = rgb(0x1E2024);
        c[ImGuiCol_TextLink] = accent;
        c[ImGuiCol_TreeLines] = rgb(0xCDD1D8);
        c[ImGuiCol_DragDropTargetBg] = rgb(0x4C8DFF, 0.10f);
        c[ImGuiCol_UnsavedMarker] = rgb(0x1E2024);
        c[ImGuiCol_NavWindowingHighlight] = accent;
        c[ImGuiCol_NavWindowingDimBg] = rgb(0x000000, 0.20f);
        c[ImGuiCol_PlotLines] = accent;
        c[ImGuiCol_PlotLinesHovered] = rgb(0x2F75F0);
        c[ImGuiCol_PlotHistogram] = accent;
        c[ImGuiCol_PlotHistogramHovered] = rgb(0x2F75F0);
    }
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
    font_ui = io.Fonts->AddFontFromMemoryTTF((void*)fe_font_inter_regular, (int)fe_font_inter_regular_size, 15.0f * settings.ui_scale, &cfg);
    font_mono = io.Fonts->AddFontFromMemoryTTF((void*)fe_font_roboto_mono, (int)fe_font_roboto_mono_size, 13.5f * settings.ui_scale, &cfg);
    if (!font_ui) font_ui = io.Fonts->AddFontDefault();
    if (!font_mono) font_mono = font_ui;
    io.FontDefault = font_ui;

    apply_theme();

    // Default file editors: text files in MOSdesk's editor.
    static const char* text_exts[] = { ".lua", ".txt", ".md", ".json", ".fescene", ".cfg", ".cpp", ".h" };
    for (const char* e : text_exts) {
        FileEditor& fe_ = settings.editors[settings.editor_count++];
        snprintf(fe_.extension, sizeof(fe_.extension), "%s", e);
        snprintf(fe_.program, sizeof(fe_.program), "%s", "0:/programs/edit.run");
    }

    snprintf(project_dir, sizeof(project_dir), "%s", platform::default_project_root());
    snprintf(files_dir, sizeof(files_dir), "%s", project_dir);
    log(LogLevel::Info, "Farcade Editor started (%dx%d)", platform::width(), platform::height());
    return true;
}

void Editor::shutdown() {
    ImGui::DestroyContext();
    platform::shutdown();
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
    int n = scene.selected_count();
    while (scene.selected_count()) scene.remove(scene.selected(0));
    if (n) log(LogLevel::Info, "Deleted %d object%s", n, n == 1 ? "" : "s");
}

void Editor::duplicate_selection() {
    int n = scene.selected_count();
    if (!n) return;
    ObjectId src_ids[64], new_ids[64];
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) src_ids[i] = scene.selected(i);
    int made = 0;
    for (int i = 0; i < n; i++) {
        Object* src = scene.get(src_ids[i]);
        if (!src) continue;
        Object copy = *src;
        ObjectId id = create_object(copy.name, copy.mesh, copy.position);
        Object* o = scene.get(id);
        char name[64];
        memcpy(name, o->name, sizeof(name));
        *o = copy;                              // everything but the id and the name
        o->id = id;
        memcpy(o->name, name, sizeof(name));
        o->position = copy.position + Vec3(0.5f, 0, 0.5f);
        new_ids[made++] = id;
    }
    scene.clear_selection();
    for (int i = 0; i < made; i++) scene.toggle(new_ids[i]);
    log(LogLevel::Info, "Duplicated %d object%s", made, made == 1 ? "" : "s");
}

void Editor::frame_selection() {
    if (!scene.selected_count()) return;
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (int i = 0; i < scene.selected_count(); i++) {
        Object* o = scene.get(scene.selected(i));
        if (!o) continue;
        const MeshData& m = Scene::mesh_for(*o);
        Mat4 mm = o->matrix();
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

void Editor::open_file(const char* path) {
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
    } else if (path_is_model(path)) {
        o->mesh = MeshKind::Model;
        snprintf(o->model_path, sizeof(o->model_path), "%s", path);
        log(LogLevel::Info, "%s: model %s", o->name, path_filename(path));
    } else if (path_is_script(path)) {
        snprintf(o->script_path, sizeof(o->script_path), "%s", path);
        log(LogLevel::Info, "%s: script %s", o->name, path_filename(path));
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
        switch (e.type) {
            case platform::EventType::MouseMove: io.AddMousePosEvent(e.x, e.y); break;
            case platform::EventType::MouseButton:
                io.AddMousePosEvent(e.x, e.y);
                io.AddMouseButtonEvent(e.button, e.down);
                break;
            case platform::EventType::MouseWheel: io.AddMouseWheelEvent(e.wheel_x, e.wheel_y); break;
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

    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    build_ui();
    ImGui::Render();

    static SwRenderer renderer;
    Framebuffer& fb = platform::framebuffer();
    SwRenderer::clear(fb, settings.dark_theme ? 0x16171A : 0xE9EAEE);
    renderer.render(ImGui::GetDrawData(), fb);
    platform::present();
    return !quit;
}

// ---------------------------------------------------------------- layout

void Editor::build_ui() {
    main_menu();
    float menu_h = ImGui::GetFrameHeight();
    float bar_h = 40.0f * settings.ui_scale;
    toolbar(menu_h, bar_h);
    dockspace(menu_h + bar_h);
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

void Editor::main_menu() {
    if (!ImGui::BeginMainMenuBar()) return;
    ImGui::TextColored(ImVec4(0.30f, 0.55f, 1.0f, 1.0f), "Farcade");
    ImGui::Spacing();
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New scene")) { scene.clear(); log(LogLevel::Info, "New scene"); }
        ImGui::MenuItem("Open project...", nullptr, false, false);
        ImGui::MenuItem("Save", "Ctrl+S", false, false);
        ImGui::Separator();
        ImGui::MenuItem("Export game...", nullptr, false, false);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        ImGui::MenuItem("Undo", "Ctrl+Z", false, false);
        ImGui::MenuItem("Redo", "Ctrl+Y", false, false);
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, scene.selected_count() > 0)) duplicate_selection();
        if (ImGui::MenuItem("Delete", "Del", false, scene.selected_count() > 0)) delete_selection();
        ImGui::Separator();
        if (ImGui::MenuItem("Settings...")) show_settings = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Create")) {
        Vec3 at = camera.target;
        at.y = 0.5f;
        if (ImGui::MenuItem("Cube")) create_object("Cube", MeshKind::Cube, at);
        if (ImGui::MenuItem("Sphere")) create_object("Sphere", MeshKind::Sphere, at);
        if (ImGui::MenuItem("Cylinder")) create_object("Cylinder", MeshKind::Cylinder, at);
        if (ImGui::MenuItem("Plane")) create_object("Plane", MeshKind::Plane, Vec3(at.x, 0, at.z));
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Frame selection", "F", false, scene.selected_count() > 0)) frame_selection();
        ImGui::MenuItem("Grid", "G", &settings.show_grid);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) want_reset_layout = true;
        if (ImGui::MenuItem("Dark theme", nullptr, settings.dark_theme)) { settings.dark_theme = !settings.dark_theme; apply_theme(); }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("ImGui demo", nullptr, &show_demo);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Editor::toolbar(float y, float h) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + y));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, h));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
    ImGui::Begin("##toolbar", nullptr, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    float bs = h - 12;
    struct ToolDef { Tool tool; Icon icon; const char* tip; };
    static const ToolDef tools[] = {
        { Tool::Select, Icon::Pointer, "Select (Q)" }, { Tool::Move, Icon::Move, "Move (W)" },
        { Tool::Rotate, Icon::Rotate, "Rotate (E)" }, { Tool::Scale, Icon::Scale, "Scale (R)" },
    };
    for (const ToolDef& t : tools) {
        if (icon_button(t.icon, t.tip, tool == t.tool, bs)) tool = t.tool;
        ImGui::SameLine(0, 2);
    }
    ImGui::SameLine(0, 14);
    if (icon_button(gizmo_local ? Icon::Local : Icon::World, gizmo_local ? "Local axes (click for world)" : "World axes (click for local)", false, bs))
        gizmo_local = !gizmo_local;
    ImGui::SameLine(0, 14);
    if (icon_button(Icon::Magnet, "Snap (hold Ctrl to invert)", settings.snap, bs)) settings.snap = !settings.snap;
    ImGui::SameLine(0, 6);
    ImGui::SetNextItemWidth(64 * settings.ui_scale);
    ImGui::SetCursorPosY((h - ImGui::GetFrameHeight()) * 0.5f);
    float* snapv = tool == Tool::Rotate ? &settings.snap_rotate : (tool == Tool::Scale ? &settings.snap_scale : &settings.snap_move);
    ImGui::DragFloat("##snapv", snapv, tool == Tool::Rotate ? 1.0f : 0.05f, 0.01f, 360.0f, tool == Tool::Rotate ? "%.0f deg" : "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Snap step for the current tool");
    ImGui::SameLine(0, 14);
    if (icon_button(Icon::Grid, "Grid (G)", settings.show_grid, bs)) settings.show_grid = !settings.show_grid;

    // Play controls in the middle.
    float mid = vp->Size.x * 0.5f - bs;
    ImGui::SameLine(mid);
    ImGui::BeginDisabled();
    icon_button(Icon::Play, "Play (coming with the Lua runtime)", false, bs);
    ImGui::SameLine(0, 2);
    icon_button(Icon::Pause, "Pause", false, bs);
    ImGui::EndDisabled();
    ImGui::End();
}

void Editor::dockspace(float top) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 pos(vp->Pos.x, vp->Pos.y + top), size(vp->Size.x, vp->Size.y - top);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::Begin("##dockhost", nullptr, flags);
    ImGui::PopStyleVar();
    ImGuiID id = ImGui::GetID("MainDock");
    if (want_reset_layout) {
        want_reset_layout = false;
        ImGui::DockBuilderRemoveNode(id);
        ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(id, ImVec2(size.x - 8, size.y - 8));
        ImGuiID center = id, left, right, bottom;
        left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
        right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
        bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
        ImGui::DockBuilderDockWindow("Hierarchy", left);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Files", bottom);
        ImGui::DockBuilderDockWindow("Console", bottom);
        ImGui::DockBuilderDockWindow("Viewport", center);
        if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(center)) n->LocalFlags |= ImGuiDockNodeFlags_HiddenTabBar;
        ImGui::DockBuilderFinish(id);
    }
    ImGui::DockSpace(id, ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();
}

void Editor::shortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) tool = Tool::Select;
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) tool = Tool::Move;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) tool = Tool::Rotate;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) tool = Tool::Scale;
    if (ImGui::IsKeyPressed(ImGuiKey_G, false) && !io.KeyCtrl) settings.show_grid = !settings.show_grid;
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) frame_selection();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) delete_selection();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicate_selection();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) scene.clear_selection();
}

}  // namespace fe
