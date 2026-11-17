// File > Project settings... and File > Export game...: the dialogs on top of
// project.h and export.h.
#include "editor.h"
#include "export.h"
#include "graphics_text.h"
#include "platform/platform.h"

#include "imgui/imgui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe {

namespace {

void export_log(int level, const char* line, void* user) {
    ((Editor*)user)->log(level == 2 ? LogLevel::Error : (level == 1 ? LogLevel::Warning : LogLevel::Info), "Export: %s", line);
}

// The scene file of the editor as a project relative path ("scenes/main.fscene"), or "".
void relative_scene(const Editor& ed, char* out, size_t cap) {
    out[0] = 0;
    if (!ed.scene_path[0]) return;
    size_t pl = strlen(ed.project_dir);
    const char* p = ed.scene_path;
    if (pl && strncmp(p, ed.project_dir, pl) == 0 && p[pl] == '/') p += pl + 1;
    snprintf(out, cap, "%s", p);
}

}  // namespace

bool Editor::load_project() {
    project = ProjectSettings();
    // The folder's name is the game's name until project.fe says otherwise.
    const char* dir_name = path_filename(project_dir);
    if (*dir_name && *dir_name != '.') snprintf(project.name, sizeof project.name, "%s", dir_name);
    char path[600];
    snprintf(path, sizeof path, "%s/project.fe", project_dir);
    size_t n = 0;
    char* text = (char*)platform::read_file(path, &n);
    if (!text) return false;
    ProjectSettings p;
    bool ok = project_parse(text, n, &p);
    free(text);
    if (!ok) { log(LogLevel::Warning, "project.fe is not a project file; using defaults"); return false; }
    project = p;
    return true;
}

// graphics.cfg in the project: the editor's current Settings > Graphics.
static void write_graphics_cfg(Editor& ed) {
    size_t gn = 0;
    char* g = graphics_settings_to_text(ed.settings.graphics, &gn);
    if (!g) return;
    char path[600];
    snprintf(path, sizeof path, "%s/graphics.cfg", ed.project_dir);
    platform::write_file(path, g, gn);
    free(g);
}

bool Editor::save_project() {
    if (project.graphics) write_graphics_cfg(*this);
    size_t n = 0;
    char* text = project_text(project, &n);
    if (!text) return false;
    char path[600];
    snprintf(path, sizeof path, "%s/project.fe", project_dir);
    bool ok = platform::write_file(path, text, n);
    free(text);
    if (ok) log(LogLevel::Info, "Saved project settings (%s)", path);
    else log(LogLevel::Error, "Could not write %s", path);
    return ok;
}

void Editor::request_export() {
    snprintf(export_name, sizeof export_name, "%s", project.name);
    snprintf(export_scene, sizeof export_scene, "%s", project.entry_scene);
    export_default_out_dir(project.name, export_out, sizeof export_out);
    export_status[0] = 0;
    want_export_ = true;
}

// File > Project settings...
void Editor::project_settings_popup() {
    if (want_project_settings_) { ImGui::OpenPopup("Project settings"); want_project_settings_ = false; }
    ImGui::SetNextWindowSize(ImVec2(460 * settings.ui_scale, 0));
    if (!ImGui::BeginPopupModal("Project settings", nullptr, ImGuiWindowFlags_NoResize)) return;
    ImGui::TextDisabled("%s/project.fe", project_dir);
    ImGui::Spacing();
    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##ps_name", project.name, sizeof project.name);
    track("project:name");
    ImGui::TextUnformatted("Version");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##ps_version", project.version, sizeof project.version);
    ImGui::TextUnformatted("Entry scene (the first one the game loads)");
    char cur[256];
    relative_scene(*this, cur, sizeof cur);
    float use_w = ImGui::CalcTextSize("Use open scene").x + 20;
    ImGui::SetNextItemWidth(-use_w - 8);
    ImGui::InputText("##ps_scene", project.entry_scene, sizeof project.entry_scene);
    track("project:scene");
    ImGui::SameLine();
    ImGui::BeginDisabled(cur[0] == 0);
    if (ImGui::Button("Use open scene")) snprintf(project.entry_scene, sizeof project.entry_scene, "%s", cur);
    track("project:use_scene");
    ImGui::EndDisabled();
    ImGui::TextUnformatted("Window size (when the game runs in a window)");
    ImGui::SetNextItemWidth(120 * settings.ui_scale);
    ImGui::InputInt("##ps_w", &project.window_width, 0, 0);
    ImGui::SameLine();
    ImGui::TextUnformatted("x");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120 * settings.ui_scale);
    ImGui::InputInt("##ps_h", &project.window_height, 0, 0);
    if (project.window_width < 64) project.window_width = 64;
    if (project.window_height < 64) project.window_height = 64;
    if (project.window_width > 8192) project.window_width = 8192;
    if (project.window_height > 8192) project.window_height = 8192;
    ImGui::TextUnformatted("Icon (a .bmp in the project; shown by MOSdesk)");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##ps_icon", project.icon, sizeof project.icon);
    track("project:icon");
    ImGui::Checkbox("Use these graphics settings for the exported game", &project.graphics);
    track("project:graphics");
    ImGui::TextDisabled("Settings > Graphics as they are when you save or export.");
    ImGui::Spacing();
    bool save = ImGui::Button("Save");
    track("project:save");
    ImGui::SameLine();
    bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    track("project:cancel");
    if (save) save_project();
    if (cancel) load_project();             // forget the edits
    if (save || cancel) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// File > Export game...
void Editor::export_popup() {
    if (want_export_) { ImGui::OpenPopup("Export game"); want_export_ = false; }
    ImGui::SetNextWindowSize(ImVec2(500 * settings.ui_scale, 0));
    if (!ImGui::BeginPopupModal("Export game", nullptr, ImGuiWindowFlags_NoResize)) return;
    ImGui::TextWrapped("Packs the scenes and everything they use into a game package and puts the player next to it.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##ex_name", export_name, sizeof export_name);
    track("export:name");
    ImGui::TextUnformatted("Entry scene");
    char cur[256];
    relative_scene(*this, cur, sizeof cur);
    float use_w = ImGui::CalcTextSize("Use open scene").x + 20;
    ImGui::SetNextItemWidth(-use_w - 8);
    ImGui::InputText("##ex_scene", export_scene, sizeof export_scene);
    track("export:scene");
    ImGui::SameLine();
    ImGui::BeginDisabled(cur[0] == 0);
    if (ImGui::Button("Use open scene##ex")) snprintf(export_scene, sizeof export_scene, "%s", cur);
    track("export:use_scene");
    ImGui::EndDisabled();
    ImGui::TextUnformatted("Output folder");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##ex_out", export_out, sizeof export_out);
    track("export:out");
    ImGui::Checkbox("Also make an installable .mpkg", &export_mpkg);
    track("export:mpkg");
    bool modified = scene_modified();
    if (modified) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "The open scene has unsaved changes: it is saved first.");
    ImGui::Spacing();
    bool valid = export_name[0] && export_scene[0] && export_out[0];
    ImGui::BeginDisabled(!valid);
    bool go = ImGui::Button("Export");
    track("export:go");
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool can_run = export_run_path[0] && export_status[0] && strncmp(export_status, "Exported", 8) == 0;
    ImGui::BeginDisabled(!can_run);
    if (ImGui::Button("Run the exported game")) {
        const char* args[3] = { export_run_path, export_pak_path, nullptr };
        if (platform::run_program(export_run_path, args, false) < 0) log(LogLevel::Error, "Could not start %s", export_run_path);
    }
    track("export:run");
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool close = ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    track("export:close");
    if (export_status[0]) ImGui::TextWrapped("%s", export_status);
    if (go) {
        if (modified) save_scene();
        // The settings the dialog shows become the project's.
        snprintf(project.name, sizeof project.name, "%s", export_name);
        snprintf(project.entry_scene, sizeof project.entry_scene, "%s", export_scene);
        save_project();
        ExportOptions opt;
        opt.project_dir = project_dir;
        opt.entry_scene = export_scene;
        opt.name = export_name;
        opt.out_dir = export_out;
        opt.mpkg = export_mpkg;
        opt.log = export_log;
        opt.user = this;
        ExportReport rep;
        if (export_game(opt, &rep)) {
            snprintf(export_status, sizeof export_status, "Exported %d files (%.1f KB) to %s%s", rep.files, rep.pak_bytes / 1024.0, export_out,
                     rep.missing ? "; some referenced files were missing (see the Console)" : "");
            snprintf(export_run_path, sizeof export_run_path, "%s", rep.run_path);
            snprintf(export_pak_path, sizeof export_pak_path, "%s", rep.pak_path);
            log(LogLevel::Info, "%s", export_status);
        } else {
            snprintf(export_status, sizeof export_status, "Export failed: %s", rep.error);
            export_run_path[0] = 0;
        }
    }
    if (close) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

}  // namespace fe
