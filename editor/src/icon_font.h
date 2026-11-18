// The editor's icons as font glyphs.
//
// Each icon is a vector .img (the Minimal-OS image format's vector mode;
// editor/icons/img, made from editor/icons/*.svg by tools/icons/build_pack.py)
// embedded in the editor. A custom ImGui font loader rasterizes it into the
// font atlas at exactly the size a frame asks for, so icons are sharp at any
// UI scale and take the text color of wherever they are drawn:
//   - merged into the UI fonts, an icon is a character (FE_ICON_* in
//     icon_glyphs.h) that sits in labels: ImGui::Button(FE_ICON_SAVE " Save");
//   - the box font draws one icon filling a square (draw_icon in icons.h).
// The logo glyph keeps its own colors.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "icon_glyphs.h"

struct ImFontAtlas;
struct ImFont;

namespace fe {

struct IconPackEntry {
    const char* name;           // editor/icons/<name>.svg
    unsigned codepoint;         // FE_ICON_FIRST ...
    const unsigned char* data;  // the vector .img
    unsigned size;
};
extern const IconPackEntry kIconPack[];
extern const unsigned kIconPackCount;

// Merges the icons into the font added to `atlas` just before (call right
// after AddFont...): inline glyphs about 1.1 em, centered on the capitals.
void icon_font_merge(ImFontAtlas* atlas);
// Adds the font draw_icon uses: one icon filling a size x size box.
ImFont* icon_font_add_box(ImFontAtlas* atlas);
// That font (null until added).
ImFont* icon_box_font();

// Rasterizes an icon at px x px into `argb` (0xAARRGGBB, px * px words),
// its "current color" paint in `current`. False if the data is not a
// vector image this build can draw. (Also used by tests and tools.)
bool icon_rasterize(const IconPackEntry& e, int px, uint32_t current, uint32_t* argb);
const IconPackEntry* icon_find(const char* name);

}  // namespace fe
