// The editor's own widgets on top of Dear ImGui: section headers, property
// rows, switches, chips. They draw with the theme palette (ui_palette()).
#pragma once

#include "icons.h"
#include <stdint.h>

struct ImFont;

namespace fe {

// The theme's colors as ImU32 (set by Editor::apply_theme), so custom
// drawing follows the dark and light themes.
struct Palette {
    uint32_t app_bg;        // behind the panels
    uint32_t panel;         // panel background
    uint32_t frame;         // inputs, buttons
    uint32_t hover;
    uint32_t active;
    uint32_t header;        // section headers
    uint32_t border;
    uint32_t text;
    uint32_t text_dim;
    uint32_t text_faint;
    uint32_t accent;
    uint32_t accent_soft;   // selection fill
    uint32_t accent_text;   // accent used for text and icons
    uint32_t axis[3];       // x, y, z
    uint32_t play;          // play mode
    uint32_t warning;
    uint32_t error;
    uint32_t overlay;       // floating viewport toolbars
    uint32_t shadow;
};

const Palette& ui_palette();
void set_ui_palette(const Palette& p, ImFont* medium);

// A collapsible section ("Transform", "Mesh"): a quiet header with a
// chevron and an icon. Returns true while open.
bool section_header(const char* title, Icon icon, bool default_open = true);

// A property row: the label in the left column, then the cursor sits in the
// right column with the item width set to fill it.
void prop_label(const char* label);
float prop_label_width();

// An on/off switch. Returns true when flipped.
bool toggle_switch(const char* id, bool* value);

// A toggle chip with an icon, a label and an optional count ("12").
bool chip(const char* id, Icon icon, uint32_t icon_color, const char* label, bool on);

// A small text button styled like a frame (used in toolbars).
bool flat_button(const char* label, float height = 0.0f);

// Text drawn with the medium font.
void heading(const char* text);

}  // namespace fe
