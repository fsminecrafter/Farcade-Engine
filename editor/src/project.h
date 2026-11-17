// Project settings: project.fe at the project's root.
//
//   farcade-project 1
//   name "Demo Game"
//   version "1.0"
//   entry_scene "scenes/main.fscene"
//   window_width 1280
//   window_height 720
//   icon "icon.bmp"
//   graphics true
//
// `graphics true`: the exported game uses the graphics settings in
// graphics.cfg (graphics_text.h) at the project's root; the editor writes that
// file from Settings > Graphics. The export packs it as graphics.cfg and the
// player applies it before the first frame.
//
// Unknown keys are kept out of the way (skipped), missing ones keep their
// defaults, so older and newer files read in both directions.
#pragma once

#include <stddef.h>

namespace fe {

constexpr int kProjectFormatVersion = 1;

struct ProjectSettings {
    char name[64] = "Game";
    char version[32] = "1.0";
    char entry_scene[256] = "scenes/main.fscene";
    int window_width = 1280;            // window size when the game runs in a window (full screen uses the screen)
    int window_height = 720;
    bool graphics = true;               // pack graphics.cfg and let the player apply it
    char icon[256] = {};                // project relative image (BMP is what MOSdesk shows); "" = none
};

// Parses project.fe text. False when it is not a project file.
bool project_parse(const char* text, size_t size, ProjectSettings* out);
// Text of the file (malloc'd, length in *size).
char* project_text(const ProjectSettings& p, size_t* size);
// "Demo Game" -> "Demo_Game": a name that is safe as a file or folder name.
void project_file_name(const char* name, char* out, size_t cap);

}  // namespace fe
