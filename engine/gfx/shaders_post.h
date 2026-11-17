// The post processing chain of the final-color shaders, generated for the
// exact set of features of a frame. FGSL has no preprocessor and the host
// shader machine runs `if` as predication (both sides are computed), so each
// choice (tonemap operator, grading, grain) is compiled in, not branched on:
// the default look costs no more than the old uniform-switched chain.
//
// The chain declares these uniforms (the renderer sets them: post_stack.h):
//   u_post  x exposure EV (manual + auto), y contrast, z saturation, w vignette
//   u_tint  rgb tint
//   u_res   xy target size, zw band offset (render_rows)
//   grade:  u_lift rgb, u_gamma rgb (1 / gamma), u_gain rgb
//   grain:  u_grain x amount, yz noise offset (changes every frame)
// and these functions:
//   vec3 lin2srgb(vec3), vec3 tone(vec3 c)   (c: linear, exposure and tint applied)
//   vec3 post(vec3 c)                         (applies exposure and tint, then tone)
//   vec3 hdr_enc(vec3) / hdr_dec(vec3)        (only with `hdr_codec`; see shaders_hq.h)
#pragma once

#include "scene/modules.h"
#include <stddef.h>
#include <stdint.h>

namespace fe::gfx::shaders {

struct PostKey {
    uint8_t tonemap = (uint8_t)Tonemap::ACES;   // Tonemap
    bool grade = false;                         // lift / gamma / gain
    bool grain = false;                         // film grain
    bool draft = false;                         // exposure + tonemap + a cheap sRGB curve only
    bool hdr_codec = false;                     // hdr_enc / hdr_dec (the GPU tier's 8 bit HDR targets)
    bool operator==(const PostKey& o) const {
        return tonemap == o.tonemap && grade == o.grade && grain == o.grain && draft == o.draft && hdr_codec == o.hdr_codec;
    }
    bool operator!=(const PostKey& o) const { return !(*this == o); }
};

// The key a PostProcess needs (grading and grain are compiled in only when they are not neutral).
PostKey post_key_for(const PostProcess& p, bool draft = false, bool hdr_codec = false);

// Writes the uniform declarations and functions listed above; returns the length.
size_t emit_post_chain(char* out, size_t cap, const PostKey& k);

// CPU copies of the tonemap operators (tests, and the auto exposure meter,
// which inverts them). Per channel, input and output linear 0..1.
float tonemap_channel(Tonemap t, float c);
void tonemap_rgb(Tonemap t, const float in[3], float out[3]);

}  // namespace fe::gfx::shaders
