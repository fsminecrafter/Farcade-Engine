// A small depth-buffered CPU renderer for the editor viewport.
//
// Renders flat-shaded triangles and depth-tested lines into an RGBA image
// that the viewport shows with ImGui::Image. Stand-in for the FarcadeGL
// renderer (which will render into a GPU surface instead); the same API
// shape (begin, draw, end, a texture to show) keeps that switch local.
#pragma once

#include "core/math.h"
#include "swraster.h"
#include <stdint.h>

namespace fe {

class View3D {
public:
    ~View3D();
    // Starts a frame of the given size, clearing to a vertical gradient
    // (colors 0xRRGGBB).
    void begin(int width, int height, const Mat4& viewproj, uint32_t top, uint32_t bottom);
    // A triangle in world space with one color (0xRRGGBB); back faces culled.
    void triangle(const Vec3& a, const Vec3& b, const Vec3& c, uint32_t color);
    // A line in world space, depth-tested (not written), alpha 0..255.
    void line(const Vec3& a, const Vec3& b, uint32_t color, int alpha);
    // The image, valid until the next begin().
    SwTexture* texture() { return &tex_; }
    int width() const { return w_; }
    int height() const { return h_; }

private:
    void raster(const Vec4* v, uint32_t color);
    uint8_t* color_ = nullptr;      // RGBA8
    float* depth_ = nullptr;
    int w_ = 0, h_ = 0, cap_ = 0;
    Mat4 vp_;
    SwTexture tex_;
};

}  // namespace fe
