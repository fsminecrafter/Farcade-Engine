// The FGSL source of the renderer's shaders (see engine/gfx/renderer.cpp for
// how they are used). Generated as text: FGSL has no preprocessor and
// unrolls its loops, so the lit fragment shader is made for the exact number
// of directional, point and spot lights of a frame.
#pragma once

#include "gfx/shaders_post.h"
#include <stddef.h>

namespace fe::gfx::shaders {

// Lights the lit shader can take (uniform registers: 4 per light).
constexpr int kMaxLights = 8;

// Sizes of the buffers the generators write into (they are always enough).
constexpr size_t kSourceBytes = 12 * 1024;

// Uniforms, as the renderer sets them (names of the FGSL uniforms):
//   lit/unlit/sky:  the post chain of shaders_post.h (u_post, u_tint, u_res and, when compiled
//                   in, u_lift / u_gamma / u_gain and u_grain); the tonemap is chosen by the
//                   PostKey at compile time, not by a uniform
//   lit/unlit:      u_mat (rgb sRGB albedo, w roughness), fgl_Model, fgl_NormalMatrix,
//                   fgl_ModelViewProjection
//   lit:            u_cam (xyz eye), u_sky / u_gnd (rgb ambient, intensity applied),
//                   u_l0 (xyz position, w range), u_l1 (xyz direction the light
//                   travels), u_l2 (rgb color * intensity), u_l3 (x cos outer,
//                   y cos inner), each an array of directional + point + spot
//                   lights in this order
//   lit/unlit:      u_texture (sampler2D, unit selected by FarcadeGL)
//   sky:            u_fwd, u_rx, u_ry (camera basis: ray = fwd + ndc.x*rx + ndc.y*ry),
//                   u_zen (rgb, w 1 = flat color), u_hor, u_grd, u_sun (xyz toward
//                   the sun, w cos of the disc radius, > 1 = no sun), u_suncol

// The probe GI lookup (gi.h) both lit shaders can include: declares sampler u_gi, u_gi0 (window origin xyz,
// 1 / spacing), u_gi1 (probe counts xyz, intensity) and `vec3 gi_amb(vec3 N, vec3 P)`, the ambient colour
// from the probe texture (4 coefficient tiles, linear filtering inside a tile, a manual lerp between two
// slices: 8 taps). Appends to `out`; returns the length.
size_t emit_gi_lookup(char* out, size_t cap);

// Lit meshes with `directional`, `point` and `spot` lights.
// `post.draft` shaders are for images that are replaced at once (camera drags): no
// specular highlight, no sun disc or halo, no contrast, saturation or vignette,
// and a two-term square root instead of pow() for the sRGB curve. They cost
// about a third of the full ones per pixel.
void lit_vertex(char* out, size_t cap);
void lit_fragment(char* out, size_t cap, int directional, int point, int spot, const PostKey& post = PostKey(), bool gi = false);
// Flat albedo (still tonemapped and graded).
void unlit_vertex(char* out, size_t cap);
void unlit_fragment(char* out, size_t cap, const PostKey& post = PostKey());
// The background: one triangle covering the screen at the far plane.
void sky_vertex(char* out, size_t cap);
void sky_fragment(char* out, size_t cap, const PostKey& post = PostKey());

}  // namespace fe::gfx::shaders
