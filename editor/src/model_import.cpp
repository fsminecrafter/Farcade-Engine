#include "editor.h"
#include "model_cache.h"
#include "model_import.h"
#include "platform/platform.h"

#include "imgui/imgui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe {

// A model file as an object in the scene: .gltf/.glb/.obj are converted to .mof first.
ObjectId Editor::import_model(const char* path, ObjectId parent) {
    char mof[512], msg[2048];
    char full[600];
    project_path(path, full, sizeof full);
    const char* use = full;
    if (path_is_importable_model(full)) {
        if (!model_convert_file(full, mof, sizeof mof, msg, sizeof msg)) {
            log(LogLevel::Error, "Import %s: %s", path_filename(path), msg);
            return kNoObject;
        }
        if (msg[0]) log(LogLevel::Warning, "Import %s: %s", path_filename(path), msg);
        log(LogLevel::Info, "Imported %s as %s", path_filename(path), path_filename(mof));
        use = mof;
    }
    model_cache_clear();
    char name[64];
    snprintf(name, sizeof name, "%s", path_filename(use));
    if (char* dot = strrchr(name, '.')) *dot = 0;
    ObjectId id = parent ? create_child(name, MeshKind::Model, parent) : create_object(name, MeshKind::Model, camera.target + Vec3(0, 0.5f, 0));
    apply_file_to_object(use, id);
    scene.select_only(id);
    return id;
}

// Create > Import model...
void Editor::import_model_popup() {
    if (want_import_model_) { ImGui::OpenPopup("Import model"); want_import_model_ = false; }
    ImGui::SetNextWindowSize(ImVec2(480 * settings.ui_scale, 0));
    if (!ImGui::BeginPopupModal("Import model", nullptr, ImGuiWindowFlags_NoResize)) return;
    ImGui::TextWrapped("A .gltf, .glb or .obj file (a path in the project, or a full path). It is converted to a .mof next to it and added to the scene.");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    bool enter = ImGui::InputText("##import_path", import_path, sizeof import_path, ImGuiInputTextFlags_EnterReturnsTrue);
    track("import:path");
    bool valid = import_path[0] && path_is_model(import_path);
    ImGui::BeginDisabled(!valid);
    bool go = ImGui::Button("Import") || (enter && valid);
    track("import:go");
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (go) import_model(import_path);
    if (go || cancel) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

}  // namespace fe
