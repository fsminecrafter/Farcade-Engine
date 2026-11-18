// Small line icons drawn with ImGui's draw list (no icon font needed).
#pragma once

#include "imgui/imgui.h"
#include <stdint.h>

namespace fe {

enum class Icon {
    Pointer, Move, Rotate, Scale, World, Local, Magnet, Grid, Play, Pause,
    Folder, File, Image, Model, Script, Cube, Sphere, Cylinder, Plane, Up, Refresh, Trash,
    Info, Warning, Error,
    Stop, Eye, EyeOff, Plus, Search, Gear, ChevronDown, ChevronRight, Close, Sun, Monitor, Layers,
    Camera, Bulb, Cloud, Speaker, Wave, Body, Collider, Ear, Wand, Empty, Star,
    Sparks, Joint, Clip, Wind, Spawn, Reverb, Sprite, TextT,
};

// Draws `icon` centered in the box [p, p + size] with the given color.
void draw_icon(ImDrawList* dl, Icon icon, ImVec2 p, float size, ImU32 color);

// A square toolbar button with an icon; `active` shows it pressed in the
// accent color. Returns true when clicked. `tint` (0 = theme text color)
// colors the icon when not active.
bool icon_button(Icon icon, const char* tooltip, bool active, float size, uint32_t tint = 0);

// The Farcade mark: a rounded square in the accent color with an "F".
void draw_logo(ImDrawList* dl, ImVec2 p, float size, ImU32 accent);

}  // namespace fe
