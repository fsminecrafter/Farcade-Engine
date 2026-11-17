// Graphics settings as text, for the exported game (docs/export.md).
//
//   preset 2
//   render_scale 1
//   shadows true
//   ...
//
// One "name value" per line, by field name; unknown names and bad values are
// skipped, missing ones keep the struct's current value.
//
// INTEGRATION HOOK: these two functions are the only place that knows the
// fields. When engine/gfx/graphics_settings.{h,cpp} gets its own
// to_text / from_text, make the bodies in graphics_text.cpp call them
// (one line each); nothing else changes.
#pragma once

#include "gfx/graphics_settings.h"
#include <stddef.h>

namespace fe {

// malloc'd text (length in *size).
char* graphics_settings_to_text(const gfx::GraphicsSettings& g, size_t* size);
// Applies the lines of `text` onto *g. False when the text has no known field.
bool graphics_settings_from_text(const char* text, size_t size, gfx::GraphicsSettings* g);

}  // namespace fe
