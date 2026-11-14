// Small line icons drawn with ImGui's draw list (no icon font needed).
#pragma once

#include "imgui/imgui.h"

namespace fe {

enum class Icon {
    Pointer, Move, Rotate, Scale, World, Local, Magnet, Grid, Play, Pause,
    Folder, File, Image, Model, Script, Cube, Sphere, Cylinder, Plane, Up, Refresh, Trash,
    Info, Warning, Error,
};

// Draws `icon` centered in the box [p, p + size] with the given color.
void draw_icon(ImDrawList* dl, Icon icon, ImVec2 p, float size, ImU32 color);

// A square toolbar button with an icon; `active` shows it pressed in the
// accent color. Returns true when clicked.
bool icon_button(Icon icon, const char* tooltip, bool active, float size);

}  // namespace fe
