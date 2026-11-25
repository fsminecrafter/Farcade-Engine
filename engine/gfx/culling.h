// Which draw items the camera can see (internal to engine/gfx; renderer.cpp
// runs it at the start of every frame).
//
// Frustum culling: an item whose world box lies outside the view frustum is
// left out of the camera passes (shadow passes still get every caster).
//
// Occlusion culling (GraphicsSettings::occlusion_culling): big nearby items
// (terrain chunks with their coarse occluder mesh, primitives and small
// models that cover enough of the screen) are drawn into a small CPU depth
// buffer (256 pixels wide); every other item's box is then tested against it
// through a max-depth pyramid, and an item whose box is behind the occluders
// at every pixel it covers is left out. Occluders are exact or lie below the
// surface they stand for (terrain), so nothing visible is culled, up to the
// buffer's pixel size for thin occluders.
#pragma once

#include "gfx/renderer.h"

namespace fe::gfx {

struct CullStats {
    int tested = 0, frustum_culled = 0, occlusion_culled = 0;
    int occluders = 0, occluder_triangles = 0;
    double ms = 0;
};

class Culler {
public:
    Culler() = default;
    ~Culler();
    Culler(const Culler&) = delete;
    Culler& operator=(const Culler&) = delete;

    // Writes the indices of the items the camera can see, in their order, to
    // `out` (room for scene.item_count) and returns how many there are.
    int run(const RenderScene& scene, bool occlusion, int* out, CullStats* stats);

    // The object space box of a mesh (from its bounds, a cache by key, or its vertices).
    bool local_bounds(const RenderMesh& m, Vec3* lo, Vec3* hi);

    // The depth buffer of the last run (tests, debug views): width x height NDC depths, 1 = empty.
    const float* depth() const { return depth_; }
    int width() const { return w_; }
    int height() const { return h_; }

private:
    struct BoundsEntry { uint64_t key; uint32_t count; Vec3 lo, hi; };
    BoundsEntry* bounds_ = nullptr;         // direct mapped cache
    float* depth_ = nullptr;                // w_ x h_
    float* pyramid_ = nullptr;              // max depth levels after level 0
    int w_ = 0, h_ = 0, levels_ = 0;
    int level_w_[12] = {}, level_h_[12] = {};
    float* level_[12] = {};
    Vec3* wlo_ = nullptr;                   // world boxes of the frame's items
    Vec3* whi_ = nullptr;
    uint8_t* state_ = nullptr;              // per item: 0 culled, 1 visible, 2 visible occluder
    int cap_ = 0;
    bool ensure(int items, int w, int h);
    void raster(const Vec3* pos, uint32_t count, const Mat4& mvp, bool mirrored, int* tris);
    void build_pyramid();
    bool box_hidden(const Mat4& vp, const Vec3& lo, const Vec3& hi) const;
};

}  // namespace fe::gfx
