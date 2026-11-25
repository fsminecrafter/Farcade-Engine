// FGSL sources of the GPU tier (see engine/gfx/passes.h and docs/graphics.md).
// Like shaders.h they are generated as text: FGSL has no preprocessor, so each
// shader is made for the exact set of features of a frame.
//
// The post chain (tonemap, grading, grain) is generated per frame: see shaders_post.h.
//
// Encodings used between passes (every target is 8 bit XRGB):
//   packed depth   24 bit fixed point in r:g:b (r high byte), 0..1; the clear
//                  value is white (= 1.0, the far end)
//   HDR color      sqrt(clamp(c, 0, 8) / 8) per channel, exposure and tint
//                  already applied ("pre-exposed"): decode is e * e * 8
//   AO + depth     r = occlusion (1 = open), g:b = 16 bit packed depth / far
#pragma once
#include "gfx/clouds.h"

#include "gfx/shaders_post.h"
#include <stddef.h>

namespace fe::gfx::shaders {

constexpr size_t kHqSourceBytes = 28 * 1024;
constexpr int kMaxCascades = 3;                     // cascaded sun shadow maps
// The frame data buffer (storage buffer 0) of the lit shaders, in vec4s:
//   [4] direction to the sun, [7] light camera right, [8] up, [9] point / spot atlas info,
//   [kCascadeBase + 6 c ..] cascade c: 4 vec4 light matrix (columns), then
//       (tile offset x, y, tile scale, normal offset) and
//       (PCF radius in cascade uv, meters per uv / range, relative depth bias, 0),
//   [kFrameHeader ..] the lights (3 vec4 each), then the point / spot shadow records.
constexpr int kCascadeBase = 10;
constexpr int kCascadeStride = 6;
constexpr int kFrameHeader = kCascadeBase + kCascadeStride * kMaxCascades;
constexpr int kAoSamples = 12;                      // spiral samples of the SSAO shader
constexpr float kHdrMax = 8.0f;                     // brightest linear value an HDR target keeps

// What one lit shader variant is built for.
struct HqLitKey {
    int nd = 0, np = 0, ns = 0;                     // directional, point, spot lights
    bool pbr = false;                               // GGX shading, else Blinn-Phong
    int shadow = 0;                                 // 0 none, 1 hard, 2 soft (PCF)
    int cascades = 1;                               // sun shadow cascades in the map (1..kMaxCascades)
    int shadow_light = 0;                           // packed light index the shadow map belongs to
    bool ao = false;                                // multiplies by the AO texture
    bool fog = false;
    bool hdr = false;                               // writes an HDR-encoded value, not the final color
    // Point and spot shadows from the atlas: the first nps point lights and
    // the first nss spot lights (in packed order) have one.
    int nps = 0, nss = 0;
    bool local_soft = false;                        // 4 taps, else 1
    PostKey post;                                   // tonemap, grading, grain (shaders_post.h)
    bool gi = false;                                // probe GI (shaders.h emit_gi_lookup) replaces the hemisphere diffuse ambient
    bool operator==(const HqLitKey& o) const {
        return post == o.post && cascades == o.cascades && gi == o.gi && nd == o.nd && np == o.np && ns == o.ns && pbr == o.pbr && shadow == o.shadow &&
               shadow_light == o.shadow_light && ao == o.ao && fog == o.fog && hdr == o.hdr &&
               nps == o.nps && nss == o.nss && local_soft == o.local_soft;
    }
};

// Scene geometry (position, normal attributes as in shaders.h).
//   lit:    u_mat (rgb sRGB albedo, w roughness), u_pbr (x metallic), u_cam, u_sky, u_gnd,
//           u_fog (rgb fog color, w density), binding 0 storage buffer for lights and shadow data;
//           shadow map: sampler u_shadow; ao: sampler u_ao; post as in shaders.h.
void hq_lit_vertex(char* out, size_t cap, int cascades);   // cascades 0: no sun shadow
void hq_lit_fragment(char* out, size_t cap, const HqLitKey& k);
void hq_unlit_vertex(char* out, size_t cap);
void hq_unlit_fragment(char* out, size_t cap, bool fog, bool hdr, const PostKey& post);
void hq_sky_fragment(char* out, size_t cap, bool hdr, const PostKey& post, CloudKey clouds = CloudKey());         // vertex: shaders::sky_vertex

// Depth-only geometry passes: the shadow map (u_shm in fgl matrices) and the
// linear depth for SSAO (u_pp.x = 1 / far).
void hq_shadow_vertex(char* out, size_t cap);
void hq_shadow_fragment(char* out, size_t cap);
void pp_white_fragment(char* out, size_t cap);                 // fills with white (clears a shadow tile)
// Point and spot shadow casters into the atlas: radial distance to the light
// (u_lp: xyz light position, w 1 / range), packed in 24 bits.
void hq_local_shadow_vertex(char* out, size_t cap);
void hq_local_shadow_fragment(char* out, size_t cap);
// The six cube faces of a point light shadow, in the order the lit shader
// picks them: +x, -x, +y, -y, +z, -z (forward), with these up vectors.
void cube_face(int face, float fwd[3], float up[3]);
void hq_depth_vertex(char* out, size_t cap);
void hq_depth_fragment(char* out, size_t cap);
void hq_normal_vertex(char* out, size_t cap);                  // view space normal * 0.5 + 0.5
void hq_normal_fragment(char* out, size_t cap);

// Fullscreen passes (one triangle; u_p = 1/target size xy, 1/source size zw).
void pp_vertex(char* out, size_t cap);
void pp_skymask_fragment(char* out, size_t cap);               // sampler u_depth: discards geometry pixels
void pp_ssao_fragment(char* out, size_t cap);                  // samplers u_depth, u_normal
void pp_gtao_fragment(char* out, size_t cap);                  // as pp_ssao_fragment, plus u_gt.x (frame rotation)
void pp_ao_accum_fragment(char* out, size_t cap);              // samplers u_cur, u_hist; u_q.x blend
void pp_copy_fragment(char* out, size_t cap);                  // sampler u_src
void hq_material_vertex(char* out, size_t cap);                // roughness / metallic G-buffer (u_mm.xy per item)
void hq_material_fragment(char* out, size_t cap);
void pp_ssrmask_fragment(char* out, size_t cap);               // samplers u_depth, u_mat; u_q.x roughness cutoff
void pp_ssr_fragment(char* out, size_t cap);
// The SSR result blurred along u_dir (5 taps), weighted by depth similarity: denoises the jittered march.
void pp_ssrblur_fragment(char* out, size_t cap);                   // samplers u_depth, u_normal, u_mat, u_scene
void pp_blur_fragment(char* out, size_t cap);                  // u_dir: xy texel step
void pp_bright_fragment(char* out, size_t cap, bool karis);    // u_q.x threshold; karis: weighted by 1 / (1 + luma)
void pp_down_fragment(char* out, size_t cap, bool wide);       // wide: 13 taps, else a 4x4 box
void pp_up_fragment(char* out, size_t cap);                    // sampler u_fine, u_coarse
void pp_composite_fragment(char* out, size_t cap, bool bloom, const PostKey& post, bool chromatic, bool ssr);
                                                               // sampler u_scene, u_bloom; u_q.x strength; u_ca.x fringe (chromatic)
void pp_taa_fragment(char* out, size_t cap);                    // samplers u_curl, u_curn, u_depth, u_hist (see the .cpp)
void pp_smaa_edge_fragment(char* out, size_t cap);             // sampler u_src: r / g = edge left / top (discards the rest)
void pp_smaa_mask_fragment(char* out, size_t cap);             // sampler u_edges: discards the edge pixels
void pp_smaa_weights_fragment(char* out, size_t cap, bool vertical);   // samplers u_edges, u_area
void pp_smaa_blend_fragment(char* out, size_t cap);            // samplers u_src, u_wh, u_wv
void pp_fxaa_mask_fragment(char* out, size_t cap);             // sampler u_n: copies, discards the edge pixels
void pp_fxaa_fragment(char* out, size_t cap);                  // samplers u_n (nearest), u_l (linear): the same image

}  // namespace fe::gfx::shaders
