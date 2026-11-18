// The Farcade player: opens a game package (.fepak) or a project folder,
// loads the entry scene and runs it like the editor's Play mode, drawn by
// FarcadeGL, full screen or in a MOSdesk window.
#pragma once

#include "project.h"
#include <stdint.h>

namespace fe::player {

struct Options {
    const char* pak_path = nullptr;     // game package to mount
    const char* project_dir = nullptr;  // or a project folder with loose files (development)
    const char* scene = nullptr;        // entry scene; default: project.fe's
    long frames = 0;                    // > 0: run this many frames at a fixed 1/60 s step, then stop
    const char* png_path = nullptr;     // host build: write the last frame here
    int render_div = 1;                 // render at 1/render_div of the screen size and scale up
    bool audio = true;
    bool quiet = false;                 // no "FE PLAYER" progress lines
};

struct Result {
    long frames = 0;
    uint32_t checksum = 0;              // of the last frame's pixels
    int objects = 0;
    int models = 0;                     // model objects whose .mof loaded
    int scripts = 0;                    // script instances that started
    int script_lines = 0;               // lines scripts printed
    ProjectSettings project;
    char error[200] = {};               // set when the game could not start
};

// Runs the game; returns 0 when it ran (until quit or `frames`), 1 when it could not start.
int run(const Options& opt, Result* result);

}  // namespace fe::player
