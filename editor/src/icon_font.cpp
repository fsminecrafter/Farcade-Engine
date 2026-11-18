#include "icon_font.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The image codec (header-only, plain C) from the Minimal-OS SDK.
#include "mosmedia.h"

namespace fe {

namespace {

enum class Mode : uint8_t { Inline, Box };
struct LoaderMode { Mode mode; };
LoaderMode g_inline = { Mode::Inline }, g_box = { Mode::Box };
ImFont* g_box_font = nullptr;

Mode mode_of(const ImFontConfig* src) {
    return src->FontData == &g_box ? Mode::Box : Mode::Inline;
}

bool src_init(ImFontAtlas*, ImFontConfig*) { return true; }
void src_destroy(ImFontAtlas*, ImFontConfig*) {}

int glyph_index(ImFontAtlas*, ImFontConfig*, ImWchar cp) {
    for (unsigned i = 0; i < kIconPackCount; i++)
        if (kIconPack[i].codepoint == (unsigned)cp) return (int)i + 1;
    return 0;
}

bool contains(ImFontAtlas*, ImFontConfig*, int index) { return index > 0 && index <= (int)kIconPackCount; }

bool baked_init(ImFontAtlas*, ImFontConfig* src, ImFontBaked* baked, void*) {
    if (!src->MergeMode) {          // the box font: a line is one icon high
        baked->Ascent = baked->Size;
        baked->Descent = 0.0f;
    }
    return true;
}

bool load_glyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void*, int index, ImFontGlyph* out,
                float* out_advance_x) {
    if (index <= 0 || index > (int)kIconPackCount) return false;
    const IconPackEntry& e = kIconPack[index - 1];
    bool box = mode_of(src) == Mode::Box;
    // The icon's square in layout units, and where it sits.
    float s = box ? baked->Size : ImFloor(baked->Size * 1.2f + 0.5f);
    float advance = box ? baked->Size : s + ImFloor(baked->Size * 0.08f + 0.5f);
    if (out_advance_x) { *out_advance_x = advance; return true; }
    out->AdvanceX = advance;
    float density = src->RasterizerDensity * baked->RasterizerDensity;
    int px = (int)(s * density + 0.5f);
    if (px < 1) return true;
    uint32_t* argb = (uint32_t*)malloc((size_t)px * px * 4);
    if (!argb) return false;
    bool colored = strcmp(e.name, "logo") == 0;
    if (!icon_rasterize(e, px, 0xFFFFFFFFu, argb)) { free(argb); return true; }     // an empty glyph
    ImFontAtlasRectId id = ImFontAtlasPackAddRect(atlas, px, px);
    if (id == ImFontAtlasRectId_Invalid) { free(argb); return false; }
    ImTextureRect* r = ImFontAtlasPackGetRect(atlas, id);
    // Box: fills [0, s]. Inline: centered on the middle of the capitals
    // (cap height about 0.73 em above the baseline), whole pixels.
    float y0 = box ? 0.0f : ImFloor(baked->Ascent - baked->Size * 0.365f - s * 0.5f + 0.5f);
    float x0 = box ? 0.0f : ImFloor((advance - s) * 0.5f);
    out->X0 = x0;
    out->Y0 = y0;
    out->X1 = x0 + px / density;
    out->Y1 = y0 + px / density;
    out->Visible = true;
    out->PackId = id;
    if (colored) {
        // RGBA bytes for the atlas.
        unsigned char* rgba = (unsigned char*)argb;
        for (int i = 0; i < px * px; i++) {
            uint32_t c = argb[i];
            rgba[i * 4 + 0] = (unsigned char)(c >> 16);
            rgba[i * 4 + 1] = (unsigned char)(c >> 8);
            rgba[i * 4 + 2] = (unsigned char)c;
            rgba[i * 4 + 3] = (unsigned char)(c >> 24);
        }
        out->Colored = true;
        ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out, r, rgba, ImTextureFormat_RGBA32, px * 4);
    } else {
        // Coverage only: the glyph takes the text color.
        unsigned char* a8 = (unsigned char*)argb;
        for (int i = 0; i < px * px; i++) a8[i] = (unsigned char)(argb[i] >> 24);
        ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out, r, a8, ImTextureFormat_Alpha8, px);
    }
    free(argb);
    return true;
}

const ImFontLoader* loader() {
    static ImFontLoader l;
    if (!l.Name) {
        l.Name = "farcade-vector-icons";
        l.FontSrcInit = src_init;
        l.FontSrcDestroy = src_destroy;
        l.FontSrcGetGlyphIndexFromCodepoint = glyph_index;
        l.FontSrcContainsGlyph = contains;
        l.FontBakedInit = baked_init;
        l.FontBakedLoadGlyph = load_glyph;
    }
    return &l;
}

ImFont* add_source(ImFontAtlas* atlas, LoaderMode* mode, bool merge) {
    if (!kIconPackCount) return nullptr;
    static const ImWchar ranges[] = { (ImWchar)FE_ICON_FIRST, (ImWchar)FE_ICON_LAST, 0 };
    ImFontConfig cfg;
    cfg.FontData = mode;                    // the loader reads its mode from here
    cfg.FontDataSize = (int)sizeof(*mode);
    cfg.FontDataOwnedByAtlas = false;
    cfg.FontLoader = loader();
    cfg.MergeMode = merge;
    cfg.GlyphRanges = ranges;
    cfg.PixelSnapH = true;
    cfg.SizePixels = 16.0f;
    snprintf(cfg.Name, sizeof(cfg.Name), "%s", merge ? "icons (inline)" : "icons (box)");
    return atlas->AddFont(&cfg);
}

}  // namespace

void icon_font_merge(ImFontAtlas* atlas) { add_source(atlas, &g_inline, true); }

ImFont* icon_font_add_box(ImFontAtlas* atlas) {
    g_box_font = add_source(atlas, &g_box, false);
    return g_box_font;
}

ImFont* icon_box_font() { return g_box_font; }

const IconPackEntry* icon_find(const char* name) {
    for (unsigned i = 0; i < kIconPackCount; i++)
        if (strcmp(kIconPack[i].name, name) == 0) return &kIconPack[i];
    return nullptr;
}

bool icon_rasterize(const IconPackEntry& e, int px, uint32_t current, uint32_t* argb) {
    if (!e.data || px < 1 || px > 4096) return false;
    mimg_info_t info;
    if (!mimg_info(e.data, e.size, &info) || !info.vector) return false;
    mimg_render_opts_t o;
    mimg_render_opts_init(&o);
    o.current_color = current;          // the "currentColor" paint (white: coverage for tinting)
    o.clear = true;
    o.alloc = malloc;                   // scratch for the rasterizer
    o.free = free;
    return mimg_render_ex(e.data, e.size, argb, (uint32_t)px, (uint32_t)px, (uint32_t)px, &o);
}

}  // namespace fe
