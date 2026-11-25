// The file and folder picker: a modal that lists a folder, with places on
// the left, so no dialog has to ask for a typed path. Fields that take a
// path get a "..." button (path_field) that opens it.
#include "editor.h"
#include "icons.h"
#include "widgets.h"
#include "platform/platform.h"
#include "core/vfs.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"   // ImStristr

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe {

namespace {

struct PickEntry {
    char name[128];
    bool is_dir;
    uint64_t size;
};

struct PickListing {
    PickEntry items[512];
    int count = 0;
    bool valid = false;
    char dir[512] = {};
};

PickListing g_pick;

int ci_compare(const char* a, const char* b) {
    for (;; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
}

int entry_compare(const void* pa, const void* pb) {
    const PickEntry* a = (const PickEntry*)pa;
    const PickEntry* b = (const PickEntry*)pb;
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    return ci_compare(a->name, b->name);
}

void collect(const platform::DirEntry& e, void* user) {
    PickListing* l = (PickListing*)user;
    if (l->count >= 512 || e.name[0] == '.') return;
    PickEntry& p = l->items[l->count++];
    snprintf(p.name, sizeof p.name, "%s", e.name);
    p.is_dir = e.is_dir;
    p.size = e.size;
}

void list(const char* dir) {
    g_pick.count = 0;
    snprintf(g_pick.dir, sizeof g_pick.dir, "%s", dir);
    g_pick.valid = platform::list_dir(dir, collect, &g_pick);
    qsort(g_pick.items, (size_t)g_pick.count, sizeof(PickEntry), entry_compare);
}

// One of the ';'-separated extensions, or any file when `exts` is empty.
bool ext_matches(const char* name, const char* exts) {
    if (!exts[0]) return true;
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    size_t dl = strlen(dot);
    for (const char* p = exts; *p;) {
        const char* e = strchr(p, ';');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == dl) {
            bool same = true;
            for (size_t i = 0; i < n && same; i++) {
                char a = dot[i], b = p[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                same = a == b;
            }
            if (same) return true;
        }
        p += n;
        if (*p == ';') p++;
    }
    return false;
}

void join_path(char* out, size_t cap, const char* dir, const char* name) {
    size_t n = strlen(dir);
    bool sep = n && (dir[n - 1] == '/' || dir[n - 1] == ':');
    snprintf(out, cap, sep ? "%s%s" : "%s/%s", dir, name);
}

// "0:/a/b" -> "0:/a"; "/a" -> "/"; a root stays as it is.
void parent_of(char* dir) {
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/') dir[--n] = 0;
    char* slash = strrchr(dir, '/');
    if (!slash) return;
    if (slash == dir || (slash == dir + 2 && dir[1] == ':')) slash[1] = 0;
    else *slash = 0;
}

bool is_root(const char* dir) {
    return !strcmp(dir, "/") || (dir[0] && dir[1] == ':' && dir[2] == '/' && !dir[3]);
}

void format_size(char* out, size_t cap, uint64_t b) {
    if (b >= 10ull * 1024 * 1024) snprintf(out, cap, "%llu MB", (unsigned long long)(b >> 20));
    else if (b >= 10ull * 1024) snprintf(out, cap, "%llu KB", (unsigned long long)(b >> 10));
    else snprintf(out, cap, "%llu B", (unsigned long long)b);
}

}  // namespace

void Editor::open_file_dialog(PickFor target, uint8_t owner, const char* title, bool folder, const char* start,
                              const char* exts) {
    FileDialog& d = file_dialog_;
    d.target = target;
    d.owner = owner;
    d.folder = folder;
    d.want_open = true;
    d.filter[0] = 0;
    d.name[0] = 0;
    d.multi = false;
    d.pick_count = 0;
    d.last_click = -1;
    snprintf(d.title, sizeof d.title, "%s", title);
    snprintf(d.exts, sizeof d.exts, "%s", exts ? exts : "");
    // Start in `start` when it is a folder, in its folder when it is a file,
    // else in the project.
    char dir[512];
    snprintf(dir, sizeof dir, "%s", start && start[0] ? start : project_dir);
    if (start && start[0] && start[0] != '/' && !(start[1] == ':')) join_path(dir, sizeof dir, project_dir, start);
    if (!folder && platform::file_exists(dir) && !platform::list_dir(dir, [](const platform::DirEntry&, void*) {}, nullptr)) {
        snprintf(d.name, sizeof d.name, "%s", path_filename(dir));
        parent_of(dir);
    }
    if (!platform::list_dir(dir, [](const platform::DirEntry&, void*) {}, nullptr))
        snprintf(dir, sizeof dir, "%s", project_dir[0] ? project_dir : platform::default_project_root());
    snprintf(d.dir, sizeof d.dir, "%s", dir);
    g_pick.dir[0] = 0;      // list it again
}

bool Editor::path_field(const char* id, char* buf, size_t cap, PickFor target, uint8_t owner, const char* title,
                        bool folder, const char* exts) {
    ImGui::PushID(id);
    // The whole field (the pushed item width, or the row) holds the text and the button.
    float total = ImGui::CalcItemWidth();
    float bw = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2;
    float iw = total - bw - ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(iw > 40 ? iw : 40);
    bool changed = ImGui::InputText("##path", buf, cap);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) open_file_dialog(target, owner, title, folder, buf, exts);
    char k[64];
    snprintf(k, sizeof k, "browse:%s", id);
    track(k);
    ImGui::PopID();
    return changed;
}

void Editor::file_dialog_popup(uint8_t owner) {
    FileDialog& d = file_dialog_;
    if (d.owner != owner || d.target == PickFor::None) return;
    if (d.want_open) { ImGui::OpenPopup("###file_dialog"); d.want_open = false; }
    const Palette& pal = ui_palette();
    float s = settings.ui_scale;
    ImGui::SetNextWindowSize(ImVec2(640 * s, 440 * s), ImGuiCond_Appearing);
    char title[96];
    snprintf(title, sizeof title, "%s###file_dialog", d.title);
    if (!begin_modal(title, nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) return;
    if (strcmp(g_pick.dir, d.dir)) list(d.dir);
    char go[512] = {};
    char picked[600] = {};
    bool cancel = false;
    float bs = ImGui::GetFrameHeight();

    // Toolbar: up, the folder, a filter.
    ImGui::BeginDisabled(is_root(d.dir));
    if (icon_button(Icon::Up, "Up one folder", false, bs)) { snprintf(go, sizeof go, "%s", d.dir); parent_of(go); }
    track("pick:up");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (icon_button(Icon::Refresh, "Refresh", false, bs)) g_pick.dir[0] = 0;
    ImGui::SameLine();
    float fw = 160 * s;
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pal.text_dim));
    char shown[512];
    snprintf(shown, sizeof shown, "%s", d.dir);
    float room = ImGui::GetContentRegionAvail().x - fw - 12;
    const char* start = shown;
    while (*start && ImGui::CalcTextSize(start).x > room) start++;
    ImGui::TextUnformatted(start);
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - fw);
    ImGui::SetNextItemWidth(fw);
    ImGui::InputTextWithHint("##pick_filter", "Filter", d.filter, sizeof d.filter);
    track("pick:filter");

    // Places on the left, the folder's contents on the right.
    float footer = bs * 2 + 22;
    float side = 150 * s;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(pal.sidebar));
    ImGui::BeginChild("##pick_places", ImVec2(side, -footer), ImGuiChildFlags_AlwaysUseWindowPadding);
    struct Place { const char* label; Icon icon; char path[512]; };
    Place places[4];
    int np = 0;
    if (project_dir[0]) { places[np] = { "Project", Icon::Star, {} }; snprintf(places[np++].path, 512, "%s", project_dir); }
    {
        char up[512];
        snprintf(up, sizeof up, "%s", project_dir[0] ? project_dir : platform::default_project_root());
        parent_of(up);
        places[np] = { "Projects", Icon::Folder, {} };
        snprintf(places[np++].path, 512, "%s", up);
    }
#if FE_MINIMALOS
    places[np] = { "System (0:)", Icon::Monitor, {} };
    snprintf(places[np++].path, 512, "%s", "0:/");
    places[np] = { "Users", Icon::Body, {} };
    snprintf(places[np++].path, 512, "%s", "0:/users");
#else
    places[np] = { "Root", Icon::Monitor, {} };
    snprintf(places[np++].path, 512, "%s", "/");
#endif
    for (int i = 0; i < np; i++) {
        char id[24];
        snprintf(id, sizeof id, "##place%d", i);
        if (nav_item(id, places[i].icon, places[i].label, !strcmp(places[i].path, d.dir)))
            snprintf(go, sizeof go, "%s", places[i].path);
        char k[32];
        snprintf(k, sizeof k, "pick:place%d", i);
        track(k);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::BeginChild("##pick_list", ImVec2(0, -footer));
    if (!g_pick.valid) ImGui::TextDisabled("This folder cannot be read.");
    int shown_count = 0;
    auto visible = [&](const PickEntry& e) {
        if (!e.is_dir && (d.folder || !ext_matches(e.name, d.exts))) return false;
        return !d.filter[0] || ImStristr(e.name, nullptr, d.filter, nullptr) != nullptr;
    };
    auto picked_at = [&](const char* nm) {
        for (int k = 0; k < d.pick_count; k++) if (!strcmp(d.picks[k], nm)) return k;
        return -1;
    };
    auto add_pick = [&](const char* nm) {
        if (picked_at(nm) < 0 && d.pick_count < FileDialog::kMaxPicks) snprintf(d.picks[d.pick_count++], sizeof d.picks[0], "%s", nm);
    };
    ImGuiIO& pio = ImGui::GetIO();
    if (d.multi && pio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false) && !ImGui::IsAnyItemActive()) {
        d.pick_count = 0;
        for (int i = 0; i < g_pick.count; i++)
            if (!g_pick.items[i].is_dir && visible(g_pick.items[i])) add_pick(g_pick.items[i].name);
    }
    for (int i = 0; i < g_pick.count; i++) {
        const PickEntry& e = g_pick.items[i];
        if (!visible(e)) continue;
        shown_count++;
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool sel = !e.is_dir && (d.multi ? picked_at(e.name) >= 0 : !strcmp(d.name, e.name));
        if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns,
                              ImVec2(0, bs))) {
            if (e.is_dir && ImGui::IsMouseDoubleClicked(0)) join_path(go, sizeof go, d.dir, e.name);
            else if (!e.is_dir) {
                snprintf(d.name, sizeof d.name, "%s", e.name);
                if (d.multi) {
                    if (pio.KeyShift && d.last_click >= 0) {
                        int a = d.last_click < i ? d.last_click : i, b = d.last_click < i ? i : d.last_click;
                        if (!pio.KeyCtrl) d.pick_count = 0;
                        for (int k = a; k <= b; k++)
                            if (!g_pick.items[k].is_dir && visible(g_pick.items[k])) add_pick(g_pick.items[k].name);
                    } else if (pio.KeyCtrl) {
                        int at = picked_at(e.name);
                        if (at >= 0) { for (int k = at; k + 1 < d.pick_count; k++) memcpy(d.picks[k], d.picks[k + 1], sizeof d.picks[0]); d.pick_count--; }
                        else add_pick(e.name);
                        d.last_click = i;
                    } else {
                        d.pick_count = 0;
                        add_pick(e.name);
                        d.last_click = i;
                    }
                }
                if (ImGui::IsMouseDoubleClicked(0)) {
                    join_path(picked, sizeof picked, d.dir, e.name);
                    if (d.multi) { d.pick_count = 0; add_pick(e.name); }
                }
            }
        }
        char k[160];
        snprintf(k, sizeof k, "pick:%s", e.name);
        track(k);
        float fs = ImGui::GetFontSize();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 color = e.is_dir ? IM_COL32(230, 180, 80, 255) : pal.text_dim;
        draw_icon(dl, e.is_dir ? Icon::Folder : Icon::File, ImVec2(p.x + 4, p.y + (bs - fs) * 0.5f), fs, color);
        dl->AddText(ImVec2(p.x + fs + 12, p.y + (bs - fs) * 0.5f), pal.text, e.name);
        if (!e.is_dir) {
            char size[24];
            format_size(size, sizeof size, e.size);
            float tw = ImGui::CalcTextSize(size).x;
            dl->AddText(ImVec2(p.x + ImGui::GetContentRegionAvail().x - tw - 6, p.y + (bs - fs) * 0.5f), pal.text_faint, size);
        }
        ImGui::PopID();
    }
    if (g_pick.valid && !shown_count)
        ImGui::TextDisabled(d.folder ? "No folders here." : d.exts[0] ? "No matching files here (%s)." : "Empty folder.", d.exts);
    ImGui::EndChild();

    // Footer: the choice and the buttons.
    ImGui::Dummy(ImVec2(0, 4));
    if (d.folder) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Folder: %s", path_filename(d.dir));
    } else if (d.multi && d.pick_count > 1) {
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d files", d.pick_count);
        ImGui::TextDisabled("Ctrl-click adds or removes one, Shift-click a range, Ctrl+A all");
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("File");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        // Focused when the picker opens: a name or a path typed here works too.
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##pick_name", d.name, sizeof d.name, ImGuiInputTextFlags_EnterReturnsTrue) && d.name[0])
            join_path(picked, sizeof picked, d.dir, d.name);
        track("pick:name");
        if (d.multi) ImGui::TextDisabled("Shows %s. Ctrl-click or Shift-click to choose several", d.exts[0] ? d.exts : "every file");
        else if (d.exts[0]) ImGui::TextDisabled("Shows %s", d.exts);
        else ImGui::NewLine();
    }
    if (d.folder) ImGui::NewLine();
    float bw = ImGui::CalcTextSize("Select folder").x + 32;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - bw * 2 - 8);
    bool can = d.folder || d.name[0] || (d.multi && d.pick_count > 0);
    ImGui::BeginDisabled(!can);
    if (primary_button(d.folder ? "Select folder" : "Open")) {
        if (d.folder) snprintf(picked, sizeof picked, "%s", d.dir);
        else if (d.multi && d.pick_count > 1) snprintf(picked, sizeof picked, "%s", "*");     // every pick, below
        else join_path(picked, sizeof picked, d.dir, d.name);
    }
    track("pick:ok");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancel = true;
    track("pick:cancel");

    if (go[0]) {
        snprintf(d.dir, sizeof d.dir, "%s", go);
        d.filter[0] = 0;
    }
    if (picked[0] || cancel) {
        PickFor t = d.target;
        d.target = PickFor::None;
        ImGui::CloseCurrentPopup();
        if (picked[0] == '*' && !picked[1]) {
            for (int k = 0; k < d.pick_count; k++) {
                char full[640];
                join_path(full, sizeof full, d.dir, d.picks[k]);
                file_dialog_done(t, full);
            }
        } else if (picked[0]) {
            file_dialog_done(t, picked);
        }
    }
    ImGui::EndPopup();
}

// A path in the project is kept relative; others stay full.
static void relative_to(const char* root, const char* path, char* out, size_t cap) {
    size_t n = strlen(root);
    if (n && !strncmp(path, root, n) && path[n] == '/') snprintf(out, cap, "%s", path + n + 1);
    else snprintf(out, cap, "%s", path);
}

void Editor::file_dialog_done(PickFor target, const char* path) {
    switch (target) {
        case PickFor::ImportModel:
            if (path_is_model(path)) import_model(path);
            else log(LogLevel::Warning, "%s is not a model (.gltf, .glb, .obj or .mof)", path_filename(path));
            break;
        case PickFor::OpenProject: request_open_project(path); break;
        case PickFor::ExportFolder: snprintf(export_out, sizeof export_out, "%s", path); break;
        case PickFor::ProjectIcon: relative_to(project_dir, path, project.icon, sizeof project.icon); break;
        case PickFor::ProjectScene: relative_to(project_dir, path, project.entry_scene, sizeof project.entry_scene); break;
        case PickFor::ExportScene: relative_to(project_dir, path, export_scene, sizeof export_scene); break;
        case PickFor::FoliageModels:
        case PickFor::FoliageFolder: {
            char rel[512];
            relative_to(project_dir, path, rel, sizeof rel);
            if (!foliage_add_model(foliage_pick_object_, foliage_pick_layer_, rel) && target == PickFor::FoliageModels)
                log(LogLevel::Warning, "%s was not added (not a .mof, already in the layer, or the list is full)", path_filename(path));
            break;
        }
        case PickFor::None: break;
    }
}

bool Editor::create_script_file(const char* dir, const char* name, char* out, size_t cap) {
    bool cpp = settings.new_script_language == 1;
    const char* ext = cpp ? ".cpp" : ".lua";
    char base[128];
    snprintf(base, sizeof base, "%s", name && name[0] ? name : "new_script");
    // Spaces and slashes make awkward file names.
    for (char* c = base; *c; c++)
        if (*c == ' ' || *c == '/' || *c == '\\' || *c == ':') *c = '_';
    bool has_ext = path_is_script(base);
    char file[160];
    snprintf(file, sizeof file, "%s%s", base, has_ext ? "" : ext);
    platform::make_dir(dir);
    join_path(out, cap, dir, file);
    if (platform::file_exists(out)) return false;
    char stem[128];
    snprintf(stem, sizeof stem, "%s", file);
    if (char* dot = strrchr(stem, '.')) *dot = 0;
    char text[1024];
    if (path_extension(file) && !strcmp(path_extension(file), ".cpp"))
        snprintf(text, sizeof text,
                 "// %s: a Farcade script in C++.\n"
                 "// Note: the editor's Play mode and exported games run Lua scripts; a C++\n"
                 "// script is kept with the project for a native build of the game.\n\n"
                 "struct %s {\n"
                 "    void on_start() {}\n"
                 "    void on_update(float dt) { (void)dt; }\n"
                 "};\n", file, stem);
    else
        snprintf(text, sizeof text,
                 "-- %s: a Farcade script.\n"
                 "-- Inputs shown in the Inspector, for example:\n"
                 "-- @prop speed number = 1 range 0 10\n\n"
                 "function on_start(self)\n"
                 "end\n\n"
                 "function on_update(self, dt)\n"
                 "end\n", file);
    return platform::write_file(out, text, strlen(text));
}

void Editor::request_open_project(const char* dir) { ask_or_do(Pending::OpenProject, dir); }

void Editor::open_project(const char* dir) {
    if (!dir || !dir[0] || !platform::list_dir(dir, [](const platform::DirEntry&, void*) {}, nullptr)) {
        log(LogLevel::Error, "Cannot open %s as a project", dir ? dir : "");
        return;
    }
    snprintf(project_dir, sizeof project_dir, "%s", dir);
    size_t n = strlen(project_dir);
    while (n > 1 && project_dir[n - 1] == '/' && !(n == 3 && project_dir[1] == ':')) project_dir[--n] = 0;
    snprintf(files_dir, sizeof files_dir, "%s", project_dir);
    bool has_file = load_project();
    vfs::set_root(project_dir);
    // Open its entry scene, or start empty.
    char scene[600] = {};
    if (project.entry_scene[0]) snprintf(scene, sizeof scene, "%s/%s", project_dir, project.entry_scene);
    if (scene[0] && platform::file_exists(scene)) open_scene(scene);
    else new_scene();
    log(LogLevel::Info, "Opened project %s%s", project_dir, has_file ? "" : " (no project.fe yet: defaults)");
}

}  // namespace fe
