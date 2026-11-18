// Export: turns a project into a standalone game (docs/export.md).
//
//   <out>/<Name>.fepak     every scene, script, model, texture, sound and
//                          shader the game needs, plus project.fe (core/pak.h)
//   <out>/<Name>.run       the prebuilt player (Minimal-OS; on the PC <out>/<Name>)
//   <out>/<Name>.mpkg      with ExportOptions::mpkg: an installable package (mpkg.h) that
//                          unpacks to 0:/programs/<Name>.run and 0:/games/<Name>/<Name>.fepak (+ icon)
//   <out>/icon.bmp         the project's icon, when it has one (also inside the .run)
//
// Needs no editor state and no screen: the editor's File > Export game... and
// the command line (`farcade-editor --export <project-or-scene> <outdir>`)
// both call export_game().
#pragma once

#include "project.h"
#include <stddef.h>
#include <stdint.h>

namespace fe {

struct ExportOptions {
    const char* project_dir = nullptr;      // the project folder (project.fe, scenes/, scripts/ ...)
    const char* entry_scene = nullptr;      // project relative; null = project.fe's
    const char* out_dir = nullptr;          // created when missing
    const char* name = nullptr;             // null = project.fe's name
    const char* player_path = nullptr;      // prebuilt player; null = export_default_player()
    bool mpkg = false;                      // also write <Name>.mpkg (installs to 0:/programs/<Name>.run and 0:/games/<Name>/)
    bool all_scenes = true;                 // pack every .fscene in the project, not only the entry scene
    // Progress and problems, one line each (level 0 info, 1 warning, 2 error). May be null.
    void (*log)(int level, const char* line, void* user) = nullptr;
    void* user = nullptr;
};

struct ExportReport {
    int files = 0;                          // entries in the package
    int scenes = 0;
    int missing = 0;                        // referenced files that were not found (warned about)
    uint64_t pak_bytes = 0;
    char pak_path[512] = {};
    char run_path[512] = {};
    char mpkg_path[512] = {};               // when ExportOptions::mpkg
    char error[256] = {};                   // set when the export failed
};

// Where the prebuilt player lives: 0:/programs/feplayer.run on Minimal-OS,
// build/host/bin/feplayer on the PC.
const char* export_default_player();
// The default output folder for a game name: 0:/games/<Name>/ on Minimal-OS, ./export/<Name> on the PC.
void export_default_out_dir(const char* name, char* out, size_t cap);

bool export_game(const ExportOptions& opt, ExportReport* report);

// `farcade-editor --export <project-or-scene> <outdir> [--name N] [--scene S]
// [--player P] [--mpkg] [--run [N]]`. `argv[i]` is the "--export" argument. Prints
// "FE EXPORT OK ..." or "FE EXPORT FAIL: ..." and returns the exit code
// (0 = exported). With --run the game is started afterwards (N frames when given).
int export_cli(int argc, char** argv, int i);

// MINIRUN1 (the SDK's .run bundle) with one more file under Resources/.
// Returns malloc'd bytes, or null when `run` is not a MINIRUN1 bundle.
uint8_t* run_bundle_add_resource(const uint8_t* run, size_t run_size, const char* name, const void* data, size_t size, size_t* out_size);

}  // namespace fe
