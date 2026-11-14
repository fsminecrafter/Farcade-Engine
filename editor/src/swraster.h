// Software renderer for Dear ImGui draw data.
//
// The editor draws its whole UI on the CPU into a 32-bit XRGB framebuffer
// (0x00RRGGBB), on the host and on Minimal-OS alike. FGPU has no scissor and
// no alpha textures yet, and a CPU renderer is the same everywhere, so the
// UI looks the same in host screenshots and on the OS.
//
// Fast paths: axis-aligned rectangles (solid fills and font glyphs, which is
// most of a UI) are filled span by span; anything else goes through a
// triangle rasterizer with interpolated color and texture coordinates.
#pragma once

#include <stdint.h>

struct ImDrawData;
struct ImTextureData;

namespace fe {

struct Framebuffer {
    uint32_t* pixels = nullptr;     // 0x00RRGGBB
    int width = 0;
    int height = 0;
    int pitch = 0;                  // in pixels
};

// A texture the renderer can sample: RGBA8 (ImGui's IM_COL32 byte order:
// R, G, B, A in memory) or 8-bit alpha. It does not own the pixels.
struct SwTexture {
    enum Format : uint8_t { RGBA8, ALPHA8 };
    Format format = RGBA8;
    int width = 0;
    int height = 0;
    int pitch = 0;                  // in pixels
    const uint8_t* pixels = nullptr;
    bool linear = false;            // bilinear sampling (images); nearest otherwise (UI, glyphs)
};

struct SwStats {
    uint32_t commands = 0;
    uint32_t triangles = 0;
    uint32_t fast_rects = 0;
    uint64_t pixels = 0;
};

class SwRenderer {
public:
    // Handles ImGui's texture requests (create, update, destroy) for every
    // texture in draw_data->Textures, then draws. `fb` must be at least
    // DisplaySize * FramebufferScale.
    void render(ImDrawData* draw_data, Framebuffer& fb);

    // Clears to a color first (0x00RRGGBB).
    static void clear(Framebuffer& fb, uint32_t color);

    // Registers a texture the application owns (e.g. a rendered viewport),
    // returning the ImTextureID to pass to ImGui::Image().
    static uint64_t texture_id(SwTexture* tex) { return (uint64_t)(uintptr_t)tex; }

    const SwStats& stats() const { return stats_; }

private:
    void update_texture(ImTextureData* tex);
    SwStats stats_;
};

}  // namespace fe
