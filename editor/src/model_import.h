// Importing models: .gltf / .glb / .obj to .mof with the engine's importers
// (engine/mof/import_*), then into the scene.
#pragma once

#include <stddef.h>

namespace fe {

// True for the formats the importers read (.gltf .glb .obj, any case).
bool path_is_importable_model(const char* path);

// Converts `src` to a .mof file next to it (same name, ".mof"; replaced when
// it exists). On success `out_path` is the new file. `message` gets the
// importer's warnings or the error text.
bool model_convert_file(const char* src, char* out_path, size_t out_cap, char* message, size_t message_cap);

}  // namespace fe
