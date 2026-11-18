// Graphics settings as text, for the project and the exported game
// (graphics.cfg, docs/export.md). The format and the field names are the
// renderer's (engine/gfx/graphics_settings.h, docs/graphics.md, "Graphics
// settings in games"):
//
//   farcade-graphics 1
//   preset high
//   aa fxaa
//   shadows cascades
//   ...
//
// These wrappers only add what a project wants: a malloc'd text, and a load
// that succeeds when at least one line was understood (unknown names and
// bad values are skipped, missing ones keep their defaults).
#pragma once

#include "gfx/graphics_settings.h"
#include <stddef.h>

namespace fe {

// malloc'd text (length in *size).
char* project_graphics_to_text(const gfx::GraphicsSettings& g, size_t* size);
// Loads `text` into *g (starting from the defaults). False when no line
// was understood.
bool project_graphics_from_text(const char* text, size_t size, gfx::GraphicsSettings* g);

}  // namespace fe
