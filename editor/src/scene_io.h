// Scene files (.fscene): the editor's scene as readable text.
//
//   farcade-scene 1
//   next_id 14
//   object 3
//     name "Ball"
//     kind mesh
//     position 0.9 0.6 -0.3
//     ...
//     module rigidbody3d
//       mass 2
//     end
//     script "scripts/ball.lua"
//       prop speed number 7.5 0 20
//     end
//   end
//
// Every value is written by name: module fields come from the reflection
// tables (engine/scene/modules.h), so a field added later reads back as its
// default from older files, and names a reader does not know are skipped.
// Object ids are kept (parents and script object links use them).
#pragma once

#include "scene.h"
#include <stddef.h>

namespace fe {

constexpr int kSceneFormatVersion = 1;

// Writes the scene. Paths that start with `project_dir` are stored relative
// to it. Returns malloc'd text (its length in *size), or null.
char* save_scene_text(Scene& scene, const char* project_dir, size_t* size);

struct SceneLoadReport {
    int objects = 0;
    int skipped = 0;            // lines with an unknown name or a bad value
    int first_bad_line = 0;     // the first of them (1-based), 0 = none
    char error[128] = {};       // set when loading failed
};

// Replaces the scene's objects with the ones in `text`. On failure the scene
// is left empty and report->error says why. Selection is cleared; the
// caller re-reads script inputs (Editor::open_scene does).
bool load_scene_text(Scene& scene, const char* text, size_t size, SceneLoadReport* report);

}  // namespace fe
