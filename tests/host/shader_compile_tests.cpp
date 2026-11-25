// Compiles every shader the renderers generate, for every variant key, with
// the same FGSL front end (fglsl.h) that FarcadeGL uses on the OS. A shader
// that only fails for one mix of features (say SSAO on with the draft post
// chain) shows up here instead of as "back to the CPU preview" on a card.
#include "gfx/shaders.h"
#include "gfx/shaders_hq.h"
#include "gfx/shaders_post.h"
#include "gfx/shader_file.h"
#include "fglsl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace fe::gfx::shaders;

namespace {

int g_programs = 0, g_failed = 0;
char* g_vs;
char* g_fs;
fglsl_program_t* g_prog;

// Compiles g_vs + g_fs; prints the error and the offending fragment line.
void check(const char* what, size_t cap) {
    g_programs++;
    size_t vl = strlen(g_vs), fl = strlen(g_fs);
    if (vl + 1 >= cap || fl + 1 >= cap) {
        if (g_failed++ < 40) printf("FAIL %s: source fills its %zu byte buffer (vertex %zu, fragment %zu)\n", what, cap, vl, fl);
        return;
    }
    // FE_DUMP_DIR=<dir>: every program as <dir>/NNNN.vs and .fs (Minimal-OS's nv_sass_tests runs them
    // through the NVIDIA shader compiler and its simulator: NV_SASS_SHADER_DIR=<dir>).
    // FE_DUMP_MATCH=<text>: only programs whose name contains it; FE_DUMP_EVERY=<n>: one in n of those.
    static int g_matched = 0;
    const char* match = getenv("FE_DUMP_MATCH");
    const char* every = getenv("FE_DUMP_EVERY");
    bool dump = !match || strstr(what, match);
    if (dump && every && atoi(every) > 1) dump = (g_matched++ % atoi(every)) == 0;
    if (const char* dir = getenv("FE_DUMP_DIR"); dir && dump) {
        char path[512];
        const char* parts[2] = { g_vs, g_fs };
        for (int k = 0; k < 2; k++) {
            snprintf(path, sizeof path, "%s/%04d.%s", dir, g_programs, k ? "fs" : "vs");
            if (FILE* f = fopen(path, "wb")) { fputs(parts[k], f); fclose(f); }
        }
        snprintf(path, sizeof path, "%s/%04d.name", dir, g_programs);
        if (FILE* f = fopen(path, "wb")) { fputs(what, f); fclose(f); }
    }
    if (fglsl_compile(g_prog, g_vs, g_fs) == 0) return;
    if (g_failed++ >= 40) return;
    printf("FAIL %s: %s\n", what, g_prog->error);
    if (getenv("FE_DUMP_SHADER")) printf("---- vertex\n%s\n---- fragment\n%s\n", g_vs, g_fs);
}

// Materials (shader_file.h): the sway sample's displace hook and a surface hook that does some work, spliced
// into the generated shaders the way the renderers do.
char* g_sway = nullptr;
// A displace hook without displace_normal: the renderer works the normal out from it (two more calls).
const char* const kWave =
    "uniform vec4 u_time;\n"
    "vec3 displace(vec3 p, vec3 n, vec2 uv) {\n"
    "    return p + n * (0.05 * sin(p.x * 3.0 + u_time.x) * cos(p.z * 2.0));\n"
    "}\n";
const char* const kSurface =
    "uniform vec4 u_time;\n"
    "vec3 surface(vec3 albedo, vec2 uv) {\n"
    "    float stripe = step(0.5, fract(uv.x * 8.0 + u_time.x * 0.1));\n"
    "    return mix(albedo, albedo * vec3(1.2, 0.9, 0.7), stripe);\n"
    "}\n";

// Splices hooks into g_vs / g_fs: displace (the sway), surface, texture. False: it does not fit.
bool splice(size_t cap, bool displace, bool surface, bool tex, bool wave = false, bool cut = false, bool two = false) {
    return fe::gfx::hook_vertex(g_vs, cap, wave ? kWave : (displace ? g_sway : nullptr), tex || surface || cut) &&
           fe::gfx::hook_fragment(g_fs, cap, surface ? kSurface : nullptr, tex) &&
           fe::gfx::hook_material(g_fs, cap, cut, two);
}

void fail_splice(const char* what) {
    g_programs++;
    if (g_failed++ < 40) printf("FAIL %s: the hooks do not fit in the source buffer\n", what);
}

// Every post chain key worth telling apart.
int post_keys(PostKey* out) {
    int n = 0;
    for (int tm = 0; tm < 4; tm++)
        for (int bits = 0; bits < 16; bits++) {
            PostKey k;
            k.tonemap = (uint8_t)tm;
            k.grade = bits & 1;
            k.grain = (bits >> 1) & 1;
            k.draft = (bits >> 2) & 1;
            k.hdr_codec = (bits >> 3) & 1;
            out[n++] = k;
        }
    return n;
}

void name_post(char* s, size_t cap, const PostKey& p) {
    snprintf(s, cap, "tonemap %d%s%s%s%s", p.tonemap, p.grade ? " grade" : "", p.grain ? " grain" : "",
             p.draft ? " draft" : "", p.hdr_codec ? " hdr_codec" : "");
}

void cpu_tier(const PostKey* posts, int np) {
    char what[256], pn[96];
    for (int pi = 0; pi < np; pi++) {
        name_post(pn, sizeof pn, posts[pi]);
        for (int nd = 0; nd <= 2; nd++)
            for (int npl = 0; npl <= 2; npl++)
                for (int ns = 0; ns <= 2; ns++)
                    for (int gi = 0; gi < 2; gi++) {
                        if (nd + npl + ns > kMaxLights) continue;
                        lit_vertex(g_vs, kSourceBytes);
                        lit_fragment(g_fs, kSourceBytes, nd, npl, ns, posts[pi], gi != 0);
                        snprintf(what, sizeof what, "lit %d/%d/%d%s, %s", nd, npl, ns, gi ? " gi" : "", pn);
                        check(what, kSourceBytes);
                    }
        unlit_vertex(g_vs, kSourceBytes);
        unlit_fragment(g_fs, kSourceBytes, posts[pi]);
        snprintf(what, sizeof what, "unlit, %s", pn);
        check(what, kSourceBytes);
        // Hooked base shaders (the base shaders sample u_texture already): the most lights, with and without GI.
        for (int m = 1; m < 4; m++) {
            bool d = m & 1, sf = (m >> 1) & 1;
            for (int gi = 0; gi < 2; gi++) {
                lit_vertex(g_vs, kSourceBytes);
                lit_fragment(g_fs, kSourceBytes, 2, 2, 2, posts[pi], gi != 0);
                snprintf(what, sizeof what, "lit 2/2/2%s%s%s, %s", gi ? " gi" : "", d ? " +displace" : "", sf ? " +surface" : "", pn);
                if (splice(kSourceBytes, d, sf, false)) check(what, kSourceBytes); else fail_splice(what);
                lit_vertex(g_vs, kSourceBytes);
                lit_fragment(g_fs, kSourceBytes, 2, 2, 2, posts[pi], gi != 0);
                snprintf(what, sizeof what, "lit 2/2/2%s%s%s +cutout +two-sided, %s", gi ? " gi" : "", d ? " +displace" : "", sf ? " +surface" : "", pn);
                if (splice(kSourceBytes, d, sf, false, false, true, true)) check(what, kSourceBytes); else fail_splice(what);
            }
            unlit_vertex(g_vs, kSourceBytes);
            unlit_fragment(g_fs, kSourceBytes, posts[pi]);
            snprintf(what, sizeof what, "unlit%s%s, %s", d ? " +displace" : "", sf ? " +surface" : "", pn);
            if (splice(kSourceBytes, d, sf, false)) check(what, kSourceBytes); else fail_splice(what);
        }
        // The sky with every cloud quality and layer count (gfx/clouds.h).
        for (int cq = 0; cq <= 3; cq++)
            for (int cl = 1; cl <= (cq ? 3 : 1); cl++) {
                fe::gfx::CloudKey ck;
                ck.quality = (uint8_t)cq;
                ck.layers = (uint8_t)cl;
                sky_vertex(g_vs, kSourceBytes);
                sky_fragment(g_fs, kSourceBytes, posts[pi], ck);
                snprintf(what, sizeof what, "sky, clouds %d x%d, %s", cq, cl, pn);
                check(what, kSourceBytes);
            }
    }
}

void gpu_tier_lit(const PostKey* posts, int np) {
    char what[384], pn[96];
    for (int pi = 0; pi < np; pi++) {
        name_post(pn, sizeof pn, posts[pi]);
        // Every light kind alone and together, plus two of each; the other
        // tonemaps change only the post chain, which the rest tests cover.
        if (posts[pi].tonemap != 0) continue;
        static const int mixes[][3] = { {0,0,0}, {1,0,0}, {0,1,0}, {0,0,1}, {1,1,1}, {2,2,2} };
        for (const auto& m : mixes) {
            HqLitKey k;
            k.post = posts[pi];
            k.nd = m[0];
            k.np = m[1];
            k.ns = m[2];
            if (k.nd + k.np + k.ns > kMaxLights) continue;
            for (int bits = 0; bits < 128; bits++) {
                k.pbr = bits & 1;
                k.ao = (bits >> 1) & 1;
                k.fog = (bits >> 2) & 1;
                k.hdr = (bits >> 3) & 1;
                k.gi = (bits >> 4) & 1;
                k.local_soft = (bits >> 5) & 1;
                bool local = (bits >> 6) & 1;
                k.nps = local ? k.np : 0;
                k.nss = local ? k.ns : 0;
                for (int shadow = 0; shadow < 3; shadow++) {
                    k.shadow = shadow;
                    int max_c = shadow && k.nd ? kMaxCascades : 1;
                    for (int c = 1; c <= max_c; c++) {
                        k.cascades = c;
                        k.shadow_light = 0;
                        hq_lit_vertex(g_vs, kHqSourceBytes, shadow ? c : 0);
                        hq_lit_fragment(g_fs, kHqSourceBytes, k);
                        snprintf(what, sizeof what,
                                 "hq lit %d/%d/%d%s%s%s%s%s shadow %d x%d local %d/%d%s, %s", k.nd, k.np, k.ns,
                                 k.pbr ? " pbr" : "", k.ao ? " ao" : "", k.fog ? " fog" : "", k.hdr ? " hdr" : "",
                                 k.gi ? " gi" : "", shadow, c, k.nps, k.nss, k.local_soft ? " soft" : "", pn);
                        check(what, kHqSourceBytes);
                        // Materials (texture, the sway's displace, a surface hook) on the heaviest shadows of each
                        // mix with the full post chain: what the tier falls back from when they do not fit.
                        if (shadow == 2 && c == max_c && posts[pi].grade && posts[pi].grain) {
                            static const bool mats[4][3] = { {false, false, true}, {true, false, false}, {true, true, true}, {false, true, false} };
                            for (const auto& mt : mats) {
                                hq_lit_vertex(g_vs, kHqSourceBytes, c);
                                hq_lit_fragment(g_fs, kHqSourceBytes, k);
                                char w2[448];
                                snprintf(w2, sizeof w2, "%s%s%s%s", what, mt[0] ? " +displace" : "", mt[1] ? " +surface" : "", mt[2] ? " +texture" : "");
                                if (splice(kHqSourceBytes, mt[0], mt[1], mt[2])) check(w2, kHqSourceBytes); else fail_splice(w2);
                            }
                            hq_lit_vertex(g_vs, kHqSourceBytes, c);
                            hq_lit_fragment(g_fs, kHqSourceBytes, k);
                            char w3[448];
                            snprintf(w3, sizeof w3, "%s +displace (normals worked out) +texture", what);
                            if (splice(kHqSourceBytes, true, false, true, true)) check(w3, kHqSourceBytes); else fail_splice(w3);
                            // leaf cards: cut-out, two-sided, the sway and the texture together
                            hq_lit_vertex(g_vs, kHqSourceBytes, c);
                            hq_lit_fragment(g_fs, kHqSourceBytes, k);
                            snprintf(w3, sizeof w3, "%s +displace +texture +cutout +two-sided", what);
                            if (splice(kHqSourceBytes, true, false, true, false, true, true)) check(w3, kHqSourceBytes); else fail_splice(w3);
                        }
                    }
                }
            }
        }
    }
}

void gpu_tier_rest(const PostKey* posts, int np) {
    char what[256], pn[96];
    // The geometry passes with the sway's displace hook.
    {
        void (*vg[5])(char*, size_t) = { hq_shadow_vertex, hq_local_shadow_vertex, hq_depth_vertex, hq_normal_vertex, hq_material_vertex };
        void (*fg[5])(char*, size_t) = { hq_shadow_fragment, hq_local_shadow_fragment, hq_depth_fragment, hq_normal_fragment, hq_material_fragment };
        static const char* const names[5] = { "shadow", "local shadow", "depth", "normal", "material" };
        for (int i = 0; i < 5; i++) {
            vg[i](g_vs, kHqSourceBytes);
            fg[i](g_fs, kHqSourceBytes);
            snprintf(what, sizeof what, "hq %s +displace", names[i]);
            if (fe::gfx::hook_vertex(g_vs, kHqSourceBytes, g_sway, false)) check(what, kHqSourceBytes); else fail_splice(what);
            vg[i](g_vs, kHqSourceBytes);
            fg[i](g_fs, kHqSourceBytes);
            snprintf(what, sizeof what, "hq %s +displace (normals worked out)", names[i]);
            if (fe::gfx::hook_vertex(g_vs, kHqSourceBytes, kWave, false)) check(what, kHqSourceBytes); else fail_splice(what);
            for (int m = 1; m < 4; m++) {
                vg[i](g_vs, kHqSourceBytes);
                fg[i](g_fs, kHqSourceBytes);
                snprintf(what, sizeof what, "hq %s +cutout%s%s", names[i], (m & 1) ? " +displace" : "", (m & 2) ? " +two-sided" : "");
                if (fe::gfx::hook_vertex(g_vs, kHqSourceBytes, (m & 1) ? g_sway : nullptr, true) &&
                    fe::gfx::hook_material(g_fs, kHqSourceBytes, true, (m & 2) != 0)) check(what, kHqSourceBytes); else fail_splice(what);
            }
        }
        hq_shadow_vertex(g_vs, kHqSourceBytes);
        hq_shadow_over_fragment(g_fs, kHqSourceBytes);
        check("hq shadow over", kHqSourceBytes);
        hq_shadow_vertex(g_vs, kHqSourceBytes);
        hq_shadow_over_fragment(g_fs, kHqSourceBytes);
        if (fe::gfx::hook_vertex(g_vs, kHqSourceBytes, g_sway, false)) check("hq shadow over +displace", kHqSourceBytes); else fail_splice("hq shadow over +displace");
    }
    for (int pi = 0; pi < np; pi++) {
        name_post(pn, sizeof pn, posts[pi]);
        for (int bits = 0; bits < 4; bits++) {
            bool fog = bits & 1, hdr = (bits >> 1) & 1;
            hq_unlit_vertex(g_vs, kHqSourceBytes);
            hq_unlit_fragment(g_fs, kHqSourceBytes, fog, hdr, posts[pi]);
            snprintf(what, sizeof what, "hq unlit%s%s, %s", fog ? " fog" : "", hdr ? " hdr" : "", pn);
            check(what, kHqSourceBytes);
            for (int m = 1; m < 8; m++) {
                hq_unlit_vertex(g_vs, kHqSourceBytes);
                hq_unlit_fragment(g_fs, kHqSourceBytes, fog, hdr, posts[pi]);
                char w2[320];
                snprintf(w2, sizeof w2, "%s%s%s%s", what, (m & 1) ? " +displace" : "", (m & 2) ? " +surface" : "", (m & 4) ? " +texture" : "");
                if (splice(kHqSourceBytes, m & 1, (m >> 1) & 1, (m >> 2) & 1)) check(w2, kHqSourceBytes); else fail_splice(w2);
            }
        }
        for (int hdr = 0; hdr < 2; hdr++)
            for (int cq = 0; cq <= 3; cq++)
                for (int cl = 1; cl <= (cq ? 3 : 1); cl++) {
                    fe::gfx::CloudKey ck;
                    ck.quality = (uint8_t)cq;
                    ck.layers = (uint8_t)cl;
                    sky_vertex(g_vs, kHqSourceBytes);
                    hq_sky_fragment(g_fs, kHqSourceBytes, hdr != 0, posts[pi], ck);
                    snprintf(what, sizeof what, "hq sky%s, clouds %d x%d, %s", hdr ? " hdr" : "", cq, cl, pn);
                    check(what, kHqSourceBytes);
                }
        for (int bits = 0; bits < 8; bits++) {
            bool bloom = bits & 1, chroma = (bits >> 1) & 1, ssr = (bits >> 2) & 1;
            pp_vertex(g_vs, kHqSourceBytes);
            pp_composite_fragment(g_fs, kHqSourceBytes, bloom, posts[pi], chroma, ssr);
            snprintf(what, sizeof what, "composite%s%s%s, %s", bloom ? " bloom" : "", chroma ? " chromatic" : "",
                     ssr ? " ssr" : "", pn);
            check(what, kHqSourceBytes);
        }
    }

    typedef void (*gen_t)(char*, size_t);
    struct Pair { const char* name; gen_t vs, fs; };
    static const Pair pairs[] = {
        { "shadow", hq_shadow_vertex, hq_shadow_fragment },
        { "white", pp_vertex, pp_white_fragment },
        { "local shadow", hq_local_shadow_vertex, hq_local_shadow_fragment },
        { "depth", hq_depth_vertex, hq_depth_fragment },
        { "normal", hq_normal_vertex, hq_normal_fragment },
        { "material", hq_material_vertex, hq_material_fragment },
        { "sky mask", pp_vertex, pp_skymask_fragment },
        { "ssao", pp_vertex, pp_ssao_fragment },
        { "gtao", pp_vertex, pp_gtao_fragment },
        { "ao accumulate", pp_vertex, pp_ao_accum_fragment },
        { "copy", pp_vertex, pp_copy_fragment },
        { "ssr mask", pp_vertex, pp_ssrmask_fragment },
        { "ssr", pp_vertex, pp_ssr_fragment },
        { "ssr blur", pp_vertex, pp_ssrblur_fragment },
        { "blur", pp_vertex, pp_blur_fragment },
        { "bloom up", pp_vertex, pp_up_fragment },
        { "taa", pp_vertex, pp_taa_fragment },
        { "smaa edges", pp_vertex, pp_smaa_edge_fragment },
        { "smaa mask", pp_vertex, pp_smaa_mask_fragment },
        { "smaa blend", pp_vertex, pp_smaa_blend_fragment },
        { "fxaa mask", pp_vertex, pp_fxaa_mask_fragment },
        { "fxaa", pp_vertex, pp_fxaa_fragment },
    };
    for (const Pair& p : pairs) {
        p.vs(g_vs, kHqSourceBytes);
        p.fs(g_fs, kHqSourceBytes);
        check(p.name, kHqSourceBytes);
    }
    for (int v = 0; v < 2; v++) {
        pp_vertex(g_vs, kHqSourceBytes);
        pp_bright_fragment(g_fs, kHqSourceBytes, v != 0);
        check(v ? "bloom bright (karis)" : "bloom bright", kHqSourceBytes);
        pp_vertex(g_vs, kHqSourceBytes);
        pp_down_fragment(g_fs, kHqSourceBytes, v != 0);
        check(v ? "bloom down (wide)" : "bloom down", kHqSourceBytes);
        pp_vertex(g_vs, kHqSourceBytes);
        pp_smaa_weights_fragment(g_fs, kHqSourceBytes, v != 0);
        check(v ? "smaa weights (vertical)" : "smaa weights", kHqSourceBytes);
    }
}

}  // namespace

int main() {
    g_vs = (char*)malloc(kHqSourceBytes);
    g_fs = (char*)malloc(kHqSourceBytes);
    g_prog = (fglsl_program_t*)malloc(sizeof(fglsl_program_t));
    if (!g_vs || !g_fs || !g_prog) return 2;
    if (FILE* f = fopen("assets/samples/foliage/sway.fshader", "rb")) {
        char* text = (char*)calloc(1, 8192);
        size_t n = text ? fread(text, 1, 8191, f) : 0;
        fclose(f);
        fe::gfx::ShaderSections sec;
        char e[128];
        if (text && fe::gfx::parse_shader_sections(text, n, &sec, e, sizeof e) && sec.displace) g_sway = strdup(sec.displace);
        fe::gfx::free_shader_sections(&sec);
        free(text);
    }
    if (!g_sway) { printf("shader_compile_tests: cannot read the displace hook of assets/samples/foliage/sway.fshader\n"); return 1; }
    PostKey posts[64];
    int np = post_keys(posts);
    cpu_tier(posts, np);
    gpu_tier_rest(posts, np);
    gpu_tier_lit(posts, np);
    if (g_failed) {
        printf("shader_compile_tests: %d of %d programs failed\n", g_failed, g_programs);
        return 1;
    }
    printf("shader_compile_tests: all %d programs compile\n", g_programs);
    return 0;
}
