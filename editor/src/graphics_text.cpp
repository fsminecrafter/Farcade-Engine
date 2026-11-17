#include "graphics_text.h"

#include <string.h>

namespace fe {

char* project_graphics_to_text(const gfx::GraphicsSettings& g, size_t* size) {
    return gfx::graphics_settings_to_text(g, size);
}

bool project_graphics_from_text(const char* text, size_t size, gfx::GraphicsSettings* g) {
    if (!text || !g) return false;
    // Lines that say something (not empty, a comment or the header).
    int lines = 0;
    for (size_t i = 0; i < size && text[i];) {
        size_t e = i;
        while (e < size && text[e] && text[e] != '\n') e++;
        size_t a = i;
        while (a < e && (text[a] == ' ' || text[a] == '\t' || text[a] == '\r')) a++;
        if (a < e && text[a] != '#' && !(e - a >= 16 && !strncmp(text + a, "farcade-graphics", 16))) lines++;
        i = e < size && text[e] ? e + 1 : e;
    }
    gfx::GraphicsSettings t;
    int ignored = gfx::graphics_settings_from_text(&t, text, size);
    if (ignored < 0 || ignored >= lines) return false;
    *g = t;
    return true;
}

}  // namespace fe
