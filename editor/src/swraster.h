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
#include <stddef.h>

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
    // XRGB8 is the framebuffer's own format (0x00RRGGBB words, pitch in pixels):
    // opaque, and blitted 1:1 by copying rows.
    enum Format : uint8_t { RGBA8, ALPHA8, XRGB8 };
    Format format = RGBA8;
    int width = 0;
    int height = 0;
    int pitch = 0;                  // in pixels
    const uint8_t* pixels = nullptr;
    bool opaque = false;            // every texel has alpha 255 (allows the 1:1 row copy)
    bool linear = false;            // bilinear sampling (images); nearest otherwise (UI, glyphs)
    // Bumped by the owner whenever the pixels change: the incremental renderer
    // redraws what shows a texture whose version changed.
    uint32_t version = 0;
};

struct SwRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;     // pixels [x0, x1) x [y0, y1)
    bool empty() const { return x0 >= x1 || y0 >= y1; }
    int64_t area() const { return empty() ? 0 : (int64_t)(x1 - x0) * (y1 - y0); }
};

struct SwStats {
    uint32_t commands = 0;
    uint32_t triangles = 0;
    uint32_t fast_rects = 0;
    uint64_t pixels = 0;
};

class SwRenderer {
public:
    ~SwRenderer();
    // Handles ImGui's texture requests (create, update, destroy) for every
    // texture in draw_data->Textures, then draws. `fb` must be at least
    // DisplaySize * FramebufferScale.
    void render(ImDrawData* draw_data, Framebuffer& fb);

    // Clears to a color first (0x00RRGGBB).
    static void clear(Framebuffer& fb, uint32_t color);

    // Draws only what changed since the previous call of render_incremental:
    // the draw lists are compared (by a hash of their commands and vertices)
    // with the previous frame's, and the rectangles they covered in either
    // frame are cleared to `clear_color` and redrawn from all lists, clipped
    // to the rectangle. The result in `fb` is identical to clear() + render()
    // (provided fb still holds the previous result: the first call, a changed
    // size or clear color, and set_verify() mismatches redraw everything).
    // The changed rectangles (at most kMaxDamage, disjoint) go to `out`; the
    // return value is how many (0: nothing changed, fb is untouched).
    static constexpr int kMaxDamage = 6;
    int render_incremental(ImDrawData* draw_data, Framebuffer& fb, uint32_t clear_color, SwRect* out);
    // Debug: after every incremental render, also draw the frame from scratch
    // and compare (counted in verify_mismatches()).
    void set_verify(bool on) { verify_ = on; }
    int verify_mismatches() const { return verify_mismatches_; }
    // Forget the previous frame (the next render_incremental redraws all).
    void invalidate() { prev_count_ = -1; }

    // Registers a texture the application owns (e.g. a rendered viewport),
    // returning the ImTextureID to pass to ImGui::Image().
    static uint64_t texture_id(SwTexture* tex) { return (uint64_t)(uintptr_t)tex; }

    const SwStats& stats() const { return stats_; }

private:
    struct ListSig { uint64_t hash; SwRect box; };
    static constexpr int kMaxLists = 64;
    void update_texture(ImTextureData* tex);
    void draw_lists(ImDrawData* dd, Framebuffer& fb, const SwRect& area, const ListSig* sigs);
    SwStats stats_;
    ListSig prev_[kMaxLists];
    int prev_count_ = -1;               // -1: nothing drawn yet
    uint32_t prev_clear_ = 0;
    int prev_w_ = 0, prev_h_ = 0;
    bool verify_ = false;
    int verify_mismatches_ = 0;
    uint32_t* verify_buf_ = nullptr;
    size_t verify_cap_ = 0;
};

}  // namespace fe
