// A small depth-buffered CPU renderer for the editor viewport.
//
// Draws lit meshes (per-pixel normals, sky/ground ambient, a sun with a
// highlight, filmic tone mapping), a fading anti-aliased ground grid, soft
// planar shadows on the ground, depth-tested lines and an outline around
// selected objects, into an RGBA image that the viewport shows with
// ImGui::Image. Optional 2x2 supersampling smooths edges.
//
// It is a stand-in for the FarcadeGL renderer (which will render into a GPU
// surface instead); the same call shape (begin, draw, finish, a texture to
// show) keeps that switch local to viewport.cpp. Uses no libm.
#pragma once

#include "core/math.h"
#include "swraster.h"
#include <stdint.h>

namespace fe {

class View3D {
public:
    ~View3D();

    // Starts a frame of width x height output pixels, rendering internally
    // at `supersample` (1 or 2) times that in each direction. The background
    // is a vertical gradient (colors 0xRRGGBB).
    void begin(int width, int height, const Mat4& view, const Mat4& proj, uint32_t top, uint32_t bottom, int supersample);

    // The selected ids (outlined by finish()); call before mesh().
    void select(const uint16_t* ids, int count, uint16_t active);

    // A triangle list (object-space positions and normals) with one sRGB
    // color. `id` (nonzero) marks its pixels for the selection outline.
    void mesh(const Vec3* pos, const Vec3* nrm, uint32_t count, const Mat4& model, const Vec3& color, uint16_t id, bool unlit);

    // Projects a mesh onto the ground (y = 0) along the sun and marks its
    // shadow. Call after all mesh() calls, so that objects hide the shadow.
    void shadow(const Vec3* pos, uint32_t count, const Mat4& model);

    // The ground grid: lines every `spacing` (stronger every 10), fading
    // with distance and where lines would get denser than the pixels.
    void grid(float spacing, bool dark_theme);

    // A line in world space, depth-tested (not written), alpha 0..255.
    void line(const Vec3& a, const Vec3& b, uint32_t color, int alpha);

    // Applies the shadows, resolves the supersampling and draws the outline
    // around the pixels of the selected ids (the active one in its own color).
    void finish(uint32_t active_color, uint32_t other_color, float shadow_strength);

    // The image, valid until the next begin().
    SwTexture* texture() { return &tex_; }
    int width() const { return w_; }
    int height() const { return h_; }
    int triangles() const { return triangles_; }

    // The sun (direction towards it), used for lighting and shadows.
    Vec3 sun = Vec3(0.48f, 0.80f, 0.36f);

private:
    struct CV { Vec4 c; Vec3 n; Vec3 p; };
    void clip_and_raster(const CV* in, const Vec3& albedo, uint32_t unlit_px, bool unlit, uint16_t id);
    void raster(const CV* v, const Vec3& albedo, uint32_t unlit_px, bool unlit, uint16_t id);
    void raster_shadow(const Vec4* v, const float* strength);
    uint32_t shade(const Vec3& albedo, const Vec3& n, const Vec3& p) const;
    void ensure(int iw, int ih, int ow, int oh);

    // Internal (supersampled) buffers.
    uint32_t* color_ = nullptr;     // RGBA8 (R in the low byte)
    float* depth_ = nullptr;
    uint16_t* ids_ = nullptr;
    uint8_t* shadow_ = nullptr;
    int iw_ = 0, ih_ = 0, icap_ = 0;
    // Output buffers.
    uint32_t* out_ = nullptr;
    uint8_t* mask_ = nullptr;       // 0, 1 = selected, 2 = active
    uint8_t* tmp_ = nullptr;
    int w_ = 0, h_ = 0, ocap_ = 0;
    int ss_ = 1;
    int triangles_ = 0;
    bool any_shadow_ = false;
    // Bounding boxes (internal pixels, inclusive) of the shadows and of the
    // selected objects, so the passes over them stay small.
    int shx0_, shy0_, shx1_, shy1_;
    int slx0_, sly0_, slx1_, sly1_;
    uint16_t sel_[256];
    int sel_count_ = 0;
    uint16_t sel_active_ = 0;
    Mat4 vp_, view_;
    Vec3 eye_;
    Vec3 sun_n_;
    SwTexture tex_;
};

}  // namespace fe
