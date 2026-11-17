// Particles and sprites in FarcadeGL's image: camera-facing quads composited
// over the frame the renderer read back (renderer.h calls draw() once per
// frame, after the readback; one hook).
//
// FarcadeGL only has alpha blending, no additive mode and no depth readback,
// so this pass is a small CPU rasterizer: each quad is a screen-aligned
// square (size in meters, in perspective), alpha or additive blended into
// the 8 bit image, and tested against a coarse depth map (a quarter size
// z-buffer of the scene's triangles, built here) so geometry hides particles
// behind it. Quads of a batch are drawn far to near; a frame has a budget of
// kMaxQuads quads and kMaxPixels filled pixels (it stops drawing, never
// fails). No per-frame allocation after the first frames.
#pragma once

#include "core/math.h"
#include <stddef.h>
#include <stdint.h>

namespace fe::gfx {

// One camera-facing quad: a center, a size and a color (sRGB, 0..1).
struct Particle {
    Vec3 pos;
    float size = 0.1f;                  // meters, edge length
    float r = 1, g = 1, b = 1, a = 1;
    float rot = 0;                      // radians (sprites; not used by the soft dot)
};

// Quads that share a texture and a blend mode.
struct ParticleBatch {
    const Particle* items = nullptr;
    int count = 0;
    const char* texture = nullptr;      // project path, null/empty = a soft round dot
    bool additive = false;
};

// Triangles that hide quads behind them.
struct Occluder {
    const Vec3* positions = nullptr;    // triangle list, object space
    uint32_t vertex_count = 0;
    Mat4 model;
};

constexpr int kMaxQuads = 20000;        // per frame
constexpr long kMaxPixels = 6000000;    // filled per frame

class ParticleRaster {
public:
    ParticleRaster();
    ~ParticleRaster();
    ParticleRaster(const ParticleRaster&) = delete;
    ParticleRaster& operator=(const ParticleRaster&) = delete;

    // Draws the batches into rows [y0, y0 + rows) of an image of width x
    // height pixels in `dst` (width * rows words). rgba: red in the low byte
    // (the renderer's pixels()); else 0x00RRGGBB (render_rows). `occluders`
    // may be null. read_asset is the renderer's asset reader (textures).
    // Returns the number of quads drawn.
    int draw(const ParticleBatch* batches, int batch_count, const Mat4& view, const Mat4& proj,
             const Occluder* occluders, int occluder_count,
             uint32_t* dst, int width, int height, int y0, int rows, bool rgba,
             void* (*read_asset)(const char* path, size_t* size, void* user), void* asset_user);

    // Statistics of the last draw().
    int last_quads() const { return last_quads_; }
    long last_pixels() const { return last_pixels_; }

    // Drops cached textures (a file changed).
    void forget_textures();

    struct Texture {
        char path[96];
        int w, h;
        uint32_t* px;           // 0xAABBGGRR
        uint64_t last_use;
    };

private:
    void build_depth(const Occluder* occ, int n, const Mat4& view, const Mat4& proj, int width, int height);
    const Texture* texture(const char* path, void* (*read_asset)(const char*, size_t*, void*), void* user);
    float* depth_ = nullptr;
    int dw_ = 0, dh_ = 0, dcap_ = 0;
    int* order_ = nullptr;
    float* keys_ = nullptr;
    int ocap_ = 0;
    Texture tex_[8];
    uint64_t frame_ = 0;
    int last_quads_ = 0;
    long last_pixels_ = 0;
};

}  // namespace fe::gfx
