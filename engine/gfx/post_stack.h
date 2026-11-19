// CPU side of the post processing stack (the shaders are in shaders_post.h
// and shaders_hq.cpp): per frame uniform values, the auto exposure meter and
// controller, dynamic resolution, and the upscale from the internal
// resolution to the image. Pure code, no FarcadeGL: host tests cover it.
#pragma once

#include "gfx/shaders_post.h"
#include "scene/modules.h"
#include "core/math.h"
#include <stdint.h>

namespace fe::gfx {

// What the shaders need of a PostProcess in this frame.
struct PostFrame {
    shaders::PostKey key;                   // compile time choices
    float exposure_ev = 0;                  // PostProcess::exposure + the auto exposure
    float lift[4] = {}, inv_gamma[4] = { 1, 1, 1, 0 }, gain[4] = { 1, 1, 1, 0 };
    float grain[4] = {};                    // x amount, yz noise offset
    float chromatic = 0;                    // 0..1
    float bloom_radius = 0.5f;

    // `auto_ev`: the controller's current EV (0 when auto exposure is off). `frame` moves the grain.
    static PostFrame make(const PostProcess& p, float auto_ev, uint32_t frame, bool draft, bool hdr_codec);
};

// Auto exposure. The meter estimates the average brightness of the scene from
// the image of the frame just drawn: it undoes the sRGB curve and the tonemap
// (and the exposure that was applied), so the answer does not depend on the
// current exposure, and takes the geometric mean of 64 x 36 samples with a
// mild weight towards the centre. The controller then eases `ev` towards the
// value that puts that mean at mid grey (0.18), with a time constant of
// 1 / speed seconds. A frame late, like every readback loop; free of GPU work.
class AutoExposure {
public:
    float ev = 0;                           // the automatic part, EV
    bool primed = false;                    // false until the first measurement (it then snaps)

    static constexpr float kKey = 0.18f;    // the scene value that maps to mid grey

    // log2 of the estimated scene luminance (geometric mean), from an image of
    // w x h pixels. `rgba`: R in the low byte (Renderer::pixels()), else XRGB (B low).
    // `applied_ev`: the exposure the image was drawn with (manual + automatic).
    static float meter(const uint32_t* px, int w, int h, bool rgba, Tonemap tm, float applied_ev);
    // The EV that maps a scene of log2 luminance `log2_lum` to mid grey.
    static float target_for(float log2_lum, float min_ev, float max_ev);
    // One step of dt seconds towards target.
    void step(float target, float speed, float dt);
    void reset() { ev = 0; primed = false; }
};

// Dynamic resolution: holds a frame time by changing the internal scale.
// Pixel cost grows with the square of the scale, so the step follows the
// square root of the time ratio; going down is fast (up to 15% a frame), going
// up is slow (up to 3%) and only with 15% of time to spare, so it settles.
struct DynamicResolution {
    float scale = 1.0f;
    // `frame_ms`: the last frame; returns the new scale in [min_scale, max_scale].
    float update(float frame_ms, float target_ms, float min_scale, float max_scale);
    // The internal size for an image of `full` pixels at `scale`: rounded to
    // multiples of 8 (so the surfaces do not change for tiny scale changes),
    // never below 16, and exactly `full` at scale >= 0.995.
    static int internal_size(int full, float scale);
};

// SMAA-style morphological AA, the part that is data: the area texture. A pixel sits in a run of edge pixels
// (the contour along an edge) with d1 pixels of run before it and d2 after it (0..7); each end of the run is
// crossed by a perpendicular edge on neither side, the up/left side (bit 0), the down/right side (bit 1), or both
// (3, treated like none). The contour is approximated by a line from the height at one end to the height at the
// other (+-0.5 pixel for a crossed end, 0 else): a Z shape (opposite ends) is one straight line, a U or L shape
// two ramps meeting at the middle of the run. The result is the area of the pixel on the far side of the
// contour: `toward_before` is the share of the pixel that takes the colour of the neighbour on the up/left side,
// `toward_after` the share for the down/right side (only one is non-zero). Orthogonal patterns only: no
// diagonal or corner patterns, no search texture (the search is done by the shader).
void smaa_area(int cross_before, int cross_after, int d1, int d2, float* toward_before, float* toward_after);
constexpr int kSmaaAreaSize = 32;                 // the texture: x = cross_before * 8 + d1, y = cross_after * 8 + d2
void smaa_area_texture(uint32_t out[kSmaaAreaSize * kSmaaAreaSize]);     // r, g = the two shares * 2, 8 bit

// Temporal anti-aliasing: the Halton(2, 3) jitter of frame `i` in pixels (-0.5..0.5), 8 frame cycle.
void taa_jitter(uint32_t frame, float* x, float* y);

// The inverse of a general 4x4 matrix (TAA reprojection). False when it is singular.
bool mat4_inverse(const Mat4& m, Mat4* out);

// Writes the XRGB image `src` (sw x sh) as an RGBA image `dst` (dw x dh,
// R in the low byte, opaque), bilinearly when the sizes differ.
void upscale_xrgb_to_rgba(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int dh);
// The same for a larger `src` (supersampling): each destination pixel is the area-weighted average of the
// source pixels under it (exactly a 2 x 2 box at twice the size). Called by upscale_xrgb_to_rgba as needed.
void downsample_xrgb_to_rgba(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int dh);

}  // namespace fe::gfx
