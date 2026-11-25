// Tests of the renderer's upgrade work: feature levels, the post stack
// (tonemaps, auto exposure, grading, dynamic resolution), cascaded shadows and
// the CPU global illumination. The older tests are in gfx_tests.cpp.
#include "gfx/renderer.h"
#include "gfx/feature_level.h"
#include "gfx/meshgen.h"
#include "gfx/post_stack.h"
#include "gfx/graphics_settings.h"
#include "gfx/gi.h"
#include "gfx/host/farcadegl_host.h"
#include "../os/gfx_smoke/scene.h"
#include "mof_check.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

using namespace fe;
using namespace fe::gfx;

namespace {

// ---------------------------------------------------------------- feature levels

void test_feature_levels() {
    // Today's FGPU v2 on the CPU backend: FL0.
    DeviceCaps cpu = DeviceCaps::from_bits(kCapShaders, 4);
    FeatureLevelChoice c = pick_feature_level(cpu, FeatureLevelMode::Auto, false);
    CHECK(c.level == FeatureLevel::FL0 && !c.forced && !c.clamped);
    // The old switch forces the GPU tier.
    c = pick_feature_level(cpu, FeatureLevelMode::Auto, true);
    CHECK(c.level == FeatureLevel::GpuTier && c.forced);
    // A graphics card: GPU tier, and the user can ask for less.
    DeviceCaps gpu = DeviceCaps::from_bits(kCapShaders | kCapHw3d, 4);
    c = pick_feature_level(gpu, FeatureLevelMode::Auto, false);
    CHECK(c.level == FeatureLevel::GpuTier && !c.forced);
    c = pick_feature_level(gpu, FeatureLevelMode::FL0, false);
    CHECK(c.level == FeatureLevel::FL0 && c.best_possible == FeatureLevel::GpuTier);
    // Scissor + mipmaps alone are not FL1-ready (16 units are needed too); with them it is.
    DeviceCaps part = DeviceCaps::from_bits(kCapShaders | kCapScissor | kCapMipmaps, 4);
    CHECK(pick_feature_level(part, FeatureLevelMode::Auto, false).level == FeatureLevel::FL0);
    DeviceCaps fl1 = DeviceCaps::from_bits(kCapShaders | kCapScissor | kCapMipmaps, 16);
    CHECK(fl1.fl1_complete());
    c = pick_feature_level(fl1, FeatureLevelMode::Auto, false);
    CHECK(c.level == FeatureLevel::FL1Ready && c.best_possible == FeatureLevel::FL1Ready);
    // Asking for FL1 without the caps is clamped to FL0; asking for the GPU tier without a card is forced.
    c = pick_feature_level(cpu, FeatureLevelMode::FL1Ready, false);
    CHECK(c.level == FeatureLevel::FL0 && c.clamped);
    c = pick_feature_level(cpu, FeatureLevelMode::GpuTier, false);
    CHECK(c.level == FeatureLevel::GpuTier && c.forced);
    PassCaps pc = pass_caps(fl1);
    CHECK(pc.scissor && pc.mipmaps && pc.tex16 && !pc.mrt);
    CHECK_STR(feature_level_name(FeatureLevel::FL1Ready), "FL1-ready");

    // The renderer reports the same on the host device (a CPU backend).
    Renderer r;
    GraphicsSettings g;
    r.set_settings(g);
    CHECK(r.init(64, 48));
    GraphicsFeatures f = r.features();
    CHECK(f.level == FeatureLevel::FL0 && !f.gpu_tier && !f.gpu);
    CHECK(!r.caps().hw_3d && r.caps().shaders);
    g.allow_gpu_effects_on_cpu = true;
    r.set_settings(g);
    f = r.features();
    CHECK(f.level == FeatureLevel::GpuTier && f.level_forced && f.gpu_tier);
    g.feature_level = FeatureLevelMode::FL0;
    g.allow_gpu_effects_on_cpu = false;
    r.set_settings(g);
    CHECK(r.feature_level() == FeatureLevel::FL0);
    r.shutdown();
}

// ---------------------------------------------------------------- post stack

constexpr int W = 160, H = 120;

// sRGB 8 bit of a linear value
int srgb8(float c) {
    c = c < 0 ? 0 : (c > 1 ? 1 : c);
    float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
    return (int)(s * 255.0f + 0.5f);
}

struct Px { int r, g, b; };
Px px(const Renderer& r, int x, int y) {
    uint32_t v = r.pixels()[y * r.width() + x];
    return { (int)(v & 0xFF), (int)((v >> 8) & 0xFF), (int)((v >> 16) & 0xFF) };
}
int lum(const Px& p) { return (p.r * 3 + p.g * 6 + p.b) / 10; }

// Nothing but a flat sky: the image is tone(color) everywhere (a vignette off).
struct FlatScene {
    RenderScene scene;
    explicit FlatScene(Vec3 color) {
        scene.sky.mode = SkyMode::Color;
        scene.sky.zenith = color;
        scene.sky.intensity = 1.0f;
        scene.post.tonemap = Tonemap::None;
        scene.post.vignette = 0;
        scene.camera.view = Mat4::identity();
        scene.camera.proj = Mat4::perspective(radians(60.0f), (float)W / H, 0.1f, 100.0f);
        scene.camera.position = Vec3(0, 0, 0);
        scene.delta_time = 1.0f;
    }
};

bool render_ok(Renderer& r, const RenderScene& s) {
    bool ok = r.render(s);
    if (!ok) { fprintf(stderr, "render: %s\n", r.error()); CHECK(false); }
    return ok;
}

// tone(c) of the chain on the CPU: operator, then contrast 1, saturation 1, sRGB.
Px cpu_tone(Tonemap tm, Vec3 c, float ev) {
    float in[3] = { c.x * exp2f(ev), c.y * exp2f(ev), c.z * exp2f(ev) }, o[3];
    shaders::tonemap_rgb(tm, in, o);
    return { srgb8(o[0]), srgb8(o[1]), srgb8(o[2]) };
}

bool near_px(const Px& a, const Px& b, int tol) {
    return abs(a.r - b.r) <= tol && abs(a.g - b.g) <= tol && abs(a.b - b.b) <= tol;
}

void test_tonemaps() {
    // The CPU operators: monotonic, bounded, AgX keeps a toe and a shoulder.
    const Tonemap all[] = { Tonemap::Reinhard, Tonemap::ACES, Tonemap::AgX };
    for (Tonemap t : all) {
        float prev = -1;
        bool mono = true;
        for (int i = 0; i < 200; i++) {
            float x = exp2f(-10.0f + 14.0f * (float)i / 199.0f);
            float y = shaders::tonemap_channel(t, x);
            if (y < prev - 1e-6f || y < 0 || y > 1.0001f) mono = false;
            prev = y;
        }
        CHECK(mono);
        CHECK(shaders::tonemap_channel(t, 0.0f) < 0.02f);
        CHECK(shaders::tonemap_channel(t, 64.0f) > 0.85f);
    }
    float mid = shaders::tonemap_channel(Tonemap::AgX, 0.18f);
    CHECK(mid > 0.1f && mid < 0.35f);                // mid grey stays mid grey
    float in[3] = { 4.0f, 0.2f, 0.05f }, out[3];
    shaders::tonemap_rgb(Tonemap::AgX, in, out);     // a saturated bright orange: stays ordered, desaturates
    CHECK(out[0] > out[1] && out[1] > out[2] && out[0] <= 1.0f);

    // The shaders match the CPU copies (base tier, a flat color).
    Renderer r;
    CHECK(r.init(W, H));
    const Vec3 color(0.9f, 0.35f, 0.1f);
    const Tonemap arr1[] = { Tonemap::None, Tonemap::Reinhard, Tonemap::ACES, Tonemap::AgX };
    for (Tonemap t : arr1) {
        FlatScene f(color);
        f.scene.post.tonemap = t;
        f.scene.post.exposure = 0.5f;
        if (!render_ok(r, f.scene)) continue;
        Px want = cpu_tone(t, color, 0.5f), got = px(r, W / 2, H / 2);
        CHECK_MSG(near_px(got, want, 2), "base tier tonemap matches the CPU operator");
    }
    // ... and the GPU tier (final colour path and the composite path with bloom)
    GraphicsSettings g;
    g.apply_preset(GraphicsPreset::Low);
    g.allow_gpu_effects_on_cpu = true;
    g.shadows = false; g.ambient_occlusion = false; g.antialiasing = false;
    g.bloom = true;
    r.set_settings(g);
    const Tonemap arr2[] = { Tonemap::Reinhard, Tonemap::AgX };
    for (Tonemap t : arr2) {
        FlatScene f(Vec3(0.3f, 0.5f, 0.2f));
        f.scene.post.tonemap = t;
        f.scene.post.bloom = 1.0f;
        f.scene.post.bloom_threshold = 9.0f;        // nothing blooms
        if (!render_ok(r, f.scene)) continue;
        CHECK(r.features().gpu_tier && r.gpu_tier_stats().composite_ms > 0);
        CHECK_MSG(near_px(px(r, W / 2, H / 2), cpu_tone(t, Vec3(0.3f, 0.5f, 0.2f), 0), 3), "GPU tier composite tonemap matches");
    }
}

void test_grading_and_grain() {
    Renderer r;
    CHECK(r.init(W, H));
    const Vec3 color(0.2f, 0.2f, 0.2f);
    FlatScene f(color);
    f.scene.post.tonemap = Tonemap::None;
    CHECK(render_ok(r, f.scene));
    Px neutral = px(r, 10, 10);
    CHECK(near_px(neutral, { srgb8(0.2f), srgb8(0.2f), srgb8(0.2f) }, 1));

    // gain doubles, lift lifts the dark, gamma > 1 brightens the mid tones: out = pow(c * gain + lift * (1 - c), 1 / gamma)
    f.scene.post.gain = Vec3(1.5f, 1.0f, 0.5f);
    f.scene.post.lift = Vec3(0.0f, 0.1f, 0.0f);
    f.scene.post.gamma = Vec3(1.0f, 1.0f, 2.0f);
    CHECK(render_ok(r, f.scene));
    auto grade = [](float c, float gain, float lift, float gamma) {
        float v = c * gain + lift * (1 - c);
        return powf(v < 0 ? 0 : v, 1.0f / gamma);
    };
    Px want = { srgb8(grade(0.2f, 1.5f, 0.0f, 1.0f)), srgb8(grade(0.2f, 1.0f, 0.1f, 1.0f)), srgb8(grade(0.2f, 0.5f, 0.0f, 2.0f)) };
    CHECK_MSG(near_px(px(r, 10, 10), want, 2), "lift / gamma / gain");
    // neutral values compile the same shader as before: same pixels
    f.scene.post.gain = Vec3(1, 1, 1); f.scene.post.lift = Vec3(0, 0, 0); f.scene.post.gamma = Vec3(1, 1, 1);
    CHECK(render_ok(r, f.scene));
    CHECK(near_px(px(r, 10, 10), neutral, 0));

    // film grain: noise around the same mean, a different pattern every frame, none at 0
    f.scene.post.film_grain = 1.0f;
    CHECK(render_ok(r, f.scene));
    double sum = 0, sum2 = 0;
    int distinct = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int v = px(r, x, y).g;
            sum += v; sum2 += (double)v * v;
            if (v != neutral.g) distinct++;
        }
    double mean = sum / (W * H), var = sum2 / (W * H) - mean * mean;
    CHECK_MSG(fabs(mean - neutral.g) < 3.0, "grain keeps the average brightness");
    CHECK_MSG(var > 4.0 && var < 600.0, "grain adds visible noise");
    CHECK(distinct > W * H / 2);
    uint32_t first[16];
    for (int i = 0; i < 16; i++) first[i] = r.pixels()[i * 5 + W * 7];
    CHECK(render_ok(r, f.scene));
    int diff = 0;
    for (int i = 0; i < 16; i++) diff += first[i] != r.pixels()[i * 5 + W * 7];
    CHECK_MSG(diff > 4, "the pattern changes every frame");
    f.scene.post.film_grain = 0;
    CHECK(render_ok(r, f.scene));
    CHECK(near_px(px(r, 10, 10), neutral, 0) && near_px(px(r, 77, 33), neutral, 0));
}

void test_auto_exposure_meter() {
    // The meter inverts sRGB, the tonemap and the applied exposure: the same scene gives the same answer
    // at any exposure, for any operator.
    uint32_t img[16 * 16];
    const Tonemap arr3[] = { Tonemap::Reinhard, Tonemap::ACES, Tonemap::AgX };
    for (Tonemap tm : arr3) {
        const float arr4[] = { 0.02f, 0.18f, 0.7f };
        for (float lum_scene : arr4) {
            const float arr5[] = { -1.0f, 0.0f, 1.5f };
            for (float ev : arr5) {
                Px p = cpu_tone(tm, Vec3(lum_scene, lum_scene, lum_scene), ev);
                uint32_t v = ((uint32_t)p.r << 16) | ((uint32_t)p.g << 8) | (uint32_t)p.b;   // XRGB
                for (uint32_t& e : img) e = v;
                float l2 = AutoExposure::meter(img, 16, 16, false, tm, ev);
                // 8 bit quantization of the dark end limits the precision
                float tol = lum_scene < 0.05f && ev < 0 ? 0.6f : 0.25f;
                CHECK_MSG(fabsf(l2 - log2f(lum_scene)) < tol, "meter recovers the scene luminance");
            }
        }
    }
    // The RGBA layout (R in the low byte) gives the same answer for a grey image.
    for (uint32_t& e : img) e = 0x00808080u;
    CHECK_NEAR(AutoExposure::meter(img, 16, 16, false, Tonemap::ACES, 0), AutoExposure::meter(img, 16, 16, true, Tonemap::ACES, 0));

    // Controller: snaps on the first measurement, then eases with 1 - exp(-speed * dt); the target is clamped.
    AutoExposure ae;
    float tgt = AutoExposure::target_for(log2f(0.045f), -6, 6);         // a dark scene: +2 EV
    CHECK(fabsf(tgt - 2.0f) < 0.01f);
    CHECK(AutoExposure::target_for(log2f(0.0001f), -3, 3) == 3.0f);
    CHECK(AutoExposure::target_for(log2f(100.0f), -3, 3) == -3.0f);
    ae.step(tgt, 2.0f, 0.1f);
    CHECK_NEAR(ae.ev, 2.0f);
    ae.step(-1.0f, 1.0f, 1.0f);
    CHECK(fabsf(ae.ev - (2.0f + (-3.0f) * (1.0f - expf(-1.0f)))) < 1e-4f);
    for (int i = 0; i < 60; i++) ae.step(-1.0f, 1.0f, 0.2f);
    CHECK(fabsf(ae.ev + 1.0f) < 0.01f);
    ae.reset();
    CHECK(!ae.primed && ae.ev == 0);
}

void test_auto_exposure_render() {
    // Dark and bright scenes end at the same brightness once the exposure has adapted; the manual exposure
    // is a compensation on top; frames render with the previous frame's measurement.
    Renderer r;
    CHECK(r.init(W, H));
    int final_lum[2] = {};
    int k = 0;
    const float arr6[] = { 0.03f, 1.6f };
    for (float level : arr6) {
        FlatScene f(Vec3(level, level, level));
        f.scene.post.tonemap = Tonemap::ACES;
        f.scene.post.auto_exposure = true;
        f.scene.post.auto_exposure_min = -6;
        f.scene.post.auto_exposure_max = 6;
        f.scene.post.exposure_speed = 4.0f;
        float first_ev = 0;
        for (int i = 0; i < 6; i++) {
            if (!render_ok(r, f.scene)) break;
            if (i == 0) first_ev = r.stats().exposure_ev;
        }
        if (k == 0) CHECK_MSG(first_ev == 0.0f, "the first frame is drawn before there is a measurement");
        CHECK_MSG(fabsf(r.stats().exposure_ev - log2f(0.18f / level)) < 0.4f, "exposure converges to the mid grey target");
        final_lum[k++] = lum(px(r, W / 2, H / 2));
    }
    CHECK_MSG(abs(final_lum[0] - final_lum[1]) <= 8, "dark and bright scenes end at the same brightness");
    // compensation
    FlatScene f(Vec3(0.2f, 0.2f, 0.2f));
    f.scene.post.auto_exposure = true;
    f.scene.post.tonemap = Tonemap::ACES;
    f.scene.post.exposure_speed = 10.0f;
    for (int i = 0; i < 4; i++) render_ok(r, f.scene);
    int base = lum(px(r, 5, 5));
    f.scene.post.exposure = 1.0f;
    for (int i = 0; i < 4; i++) render_ok(r, f.scene);
    CHECK_MSG(lum(px(r, 5, 5)) > base + 15, "Exposure is a compensation on top of the automatic part");
    // off: no adaptation state is kept
    f.scene.post.auto_exposure = false;
    f.scene.post.exposure = 0;
    render_ok(r, f.scene);
    CHECK(r.stats().exposure_ev == 0.0f);
}

void test_dynamic_resolution() {
    CHECK_EQ(DynamicResolution::internal_size(800, 1.0f), 800);
    CHECK_EQ(DynamicResolution::internal_size(800, 0.996f), 800);
    CHECK_EQ(DynamicResolution::internal_size(800, 0.5f), 400);
    CHECK_EQ(DynamicResolution::internal_size(530, 0.5f), 264);
    CHECK_EQ(DynamicResolution::internal_size(100, 0.25f), 24);
    CHECK_EQ(DynamicResolution::internal_size(20, 0.25f), 16);       // never under 16 (a tiny image is not shrunk further)
    CHECK_EQ(DynamicResolution::internal_size(16, 0.25f), 16);
    // The controller: too slow goes down fast, with spare time goes up slowly, in the band it stays.
    DynamicResolution d;
    float s = d.scale;
    for (int i = 0; i < 40; i++) {
        float ns = d.update(100.0f * s * s, 33.3f, 0.5f, 1.0f);       // cost follows the pixel count
        CHECK(ns <= s + 1e-6f);
        s = ns;
    }
    CHECK(fabsf(s - 0.577f) < 0.05f);                                 // 100 * s^2 = 33.3 -> s = 0.577
    float settled = s;
    for (int i = 0; i < 20; i++) s = d.update(30.0f, 33.3f, 0.5f, 1.0f);   // inside the band (> 85% of the target)
    CHECK_NEAR(s, settled);
    for (int i = 0; i < 100; i++) s = d.update(5.0f, 33.3f, 0.5f, 1.0f);   // lots of spare time
    CHECK_NEAR(s, 1.0f);
    for (int i = 0; i < 10; i++) s = d.update(1000.0f, 33.3f, 0.5f, 1.0f);
    CHECK_NEAR(s, 0.5f);                                              // never below the minimum
    DynamicResolution up;
    up.scale = 0.5f;
    CHECK(up.update(10.0f, 33.3f, 0.5f, 1.0f) <= 0.5f * 1.031f);      // slow to go up

    // The upscale: flat stays flat, the same size is the old swizzle, a ramp stays a ramp.
    uint32_t src[4 * 4], dst[8 * 8];
    for (uint32_t& e : src) e = 0x00336699u;
    upscale_xrgb_to_rgba(src, 4, 4, dst, 8, 8);
    bool flat = true;
    for (uint32_t e : dst) flat &= e == 0xFF996633u;
    CHECK(flat);
    uint32_t same[16];
    upscale_xrgb_to_rgba(src, 4, 4, same, 4, 4);
    CHECK(same[5] == 0xFF996633u);
    uint32_t ramp[2 * 1] = { 0x00000000u, 0x00FFFFFFu }, wide[16];
    upscale_xrgb_to_rgba(ramp, 2, 1, wide, 16, 1);
    bool mono = true;
    for (int i = 1; i < 16; i++) mono &= (wide[i] & 0xFF) >= (wide[i - 1] & 0xFF);
    CHECK(mono && (wide[0] & 0xFF) < 10 && (wide[15] & 0xFF) > 245);

    // In the renderer: half the pixels are drawn (stats), the image keeps its size and the picture its colors.
    Renderer r;
    GraphicsSettings g;
    g.render_scale = 0.5f;
    r.set_settings(g);
    CHECK(r.init(W, H));
    FlatScene f(Vec3(0.4f, 0.2f, 0.1f));
    if (render_ok(r, f.scene)) {
        CHECK_EQ(r.stats().internal_width, 80);
        CHECK_EQ(r.stats().internal_height, 64);
        CHECK_EQ(r.width(), W);
        CHECK(near_px(px(r, 0, 0), cpu_tone(Tonemap::None, Vec3(0.4f, 0.2f, 0.1f), 0), 1));
        CHECK(near_px(px(r, W - 1, H - 1), cpu_tone(Tonemap::None, Vec3(0.4f, 0.2f, 0.1f), 0), 1));
    }
    // dynamic resolution: an impossible target walks the scale down to the minimum, and the surfaces follow
    g.render_scale = 1.0f;
    g.dynamic_resolution = true;
    g.target_frame_ms = 0.05f;
    g.min_render_scale = 0.5f;
    r.set_settings(g);
    for (int i = 0; i < 30; i++) render_ok(r, f.scene);
    CHECK(r.stats().render_scale <= 0.52f && r.stats().internal_width <= 84);
    g.target_frame_ms = 100000.0f;                                     // plenty of time: back up, slowly
    r.set_settings(g);
    for (int i = 0; i < 40; i++) render_ok(r, f.scene);
    CHECK(r.stats().render_scale > 0.97f);
}

void test_bloom_and_lens() {
    // Bloom radius widens the glow; chromatic aberration only moves colors away from the centre.
    Renderer r;
    GraphicsSettings g;
    g.apply_preset(GraphicsPreset::Low);
    g.allow_gpu_effects_on_cpu = true;
    g.shadows = false; g.ambient_occlusion = false; g.antialiasing = false;
    g.bloom = true;
    g.bloom_mode = BloomMode::Chain;
    r.set_settings(g);
    CHECK(r.init(320, 240));
    Vec3 cp[kCubeVertices], cn[kCubeVertices];
    make_cube(cp, cn, 0.4f);
    DrawItem it;
    it.mesh = RenderMesh{ cp, cn, kCubeVertices, 7 };
    it.model = Mat4::translation(Vec3(-1.2f, 0, 0));
    it.unlit = true;
    it.color = Vec3(3, 3, 3);
    FlatScene f(Vec3(0.01f, 0.01f, 0.01f));
    f.scene.camera.proj = Mat4::perspective(radians(50.0f), 320.0f / 240.0f, 0.1f, 100.0f);
    f.scene.camera.view = Mat4::look_at(Vec3(0, 0, 4), Vec3(0, 0, 0), Vec3(0, 1, 0));
    f.scene.items = &it;
    f.scene.item_count = 1;
    f.scene.post.bloom = 1.0f;
    f.scene.post.bloom_threshold = 0.5f;
    int xs[3] = { 140, 160, 260 }, glow[2][3] = {};
    for (int k = 0; k < 2; k++) {
        f.scene.post.bloom_radius = k == 0 ? 0.0f : 1.0f;
        if (!render_ok(r, f.scene)) continue;
        for (int i = 0; i < 3; i++) glow[k][i] = lum(px(r, xs[i], 120));

    }
    CHECK_MSG(glow[1][0] > glow[0][0] + 40 && glow[1][1] > glow[0][1] + 15, "a wide radius reaches farther");
    CHECK_MSG(glow[0][2] <= 27 && glow[1][2] >= glow[0][2], "and a narrow one stays near the light");
    CHECK(r.gpu_tier_surfaces() > 0);
    f.scene.post.bloom_radius = 0.5f;
    f.scene.post.bloom_threshold = 9.0f;        // no glow: only the sharp edge of the cube
    auto fringe = [&](float ca) {
        f.scene.post.chromatic_aberration = ca;
        render_ok(r, f.scene);
        int worst = 0;
        for (int x = 0; x < 320; x++) {
            Px p = px(r, x, 120);
            worst = abs(p.r - p.b) > worst ? abs(p.r - p.b) : worst;
        }
        return worst;
    };
    CHECK(fringe(0.0f) <= 2);
    int with = fringe(1.0f);
    CHECK_MSG(with > 25, "red and blue are split at a bright edge away from the centre");
    render_ok(r, f.scene);
    Px mid = px(r, 160, 60);
    f.scene.post.chromatic_aberration = 0.0f;
    render_ok(r, f.scene);
    CHECK_MSG(near_px(mid, px(r, 160, 60), 1), "no fringe at the centre");
}

// ---------------------------------------------------------------- cascaded shadows

struct SunRig {
    Vec3 cube_p[kCubeVertices], cube_n[kCubeVertices], plane_p[kPlaneVertices], plane_n[kPlaneVertices];
    DrawItem items[4];
    RenderLight sun;
    RenderScene scene;
    SunRig() {
        make_cube(cube_p, cube_n, 1.0f);
        make_plane(plane_p, plane_n, 90.0f, 0.0f);
        items[0].mesh = RenderMesh{ plane_p, plane_n, kPlaneVertices, 21 };
        items[0].model = Mat4::identity();
        items[0].color = Vec3(0.7f, 0.7f, 0.7f);
        for (int i = 1; i < 4; i++) {
            items[i].mesh = RenderMesh{ cube_p, cube_n, kCubeVertices, 22 };
            items[i].color = Vec3(0.8f, 0.3f, 0.3f);
        }
        set_cube_z(-6.0f, -28.0f, -46.0f);
        sun.type = LightType::Directional;
        sun.direction = normalize(Vec3(0.7f, -1.0f, 0.1f));
        sun.intensity = 2.0f;
        scene.items = items;
        scene.item_count = 4;
        scene.lights = &sun;
        scene.light_count = 1;
        scene.sky.mode = SkyMode::Color;
        scene.sky.zenith = Vec3(0.2f, 0.2f, 0.2f);
        scene.sky_light.intensity = 0.3f;
        scene.post.tonemap = Tonemap::None;
        scene.post.vignette = 0;
        scene.delta_time = 0.016f;
        camera(Vec3(0, 4, 0));
    }
    void set_cube_z(float a, float b, float c) {
        const float z[3] = { a, b, c };
        for (int i = 0; i < 3; i++) items[1 + i].model = Mat4::translation(Vec3(0, 1.0f, z[i]));
    }
    void camera(Vec3 eye) {
        scene.camera.position = eye;
        scene.camera.view = Mat4::look_at(eye, eye + Vec3(0, -0.12f, -1), Vec3(0, 1, 0));
        scene.camera.proj = Mat4::perspective(radians(60.0f), (float)W / H, 0.1f, 200.0f);
    }
    // the pixel a world point lands on
    bool project(Vec3 p, int* x, int* y) const {
        Vec3 v = transform_point(scene.camera.view, p);
        if (v.z >= -0.01f) return false;
        float nx = scene.camera.proj.m[0] * v.x / -v.z, ny = scene.camera.proj.m[5] * v.y / -v.z;
        *x = (int)((nx * 0.5f + 0.5f) * (float)W);
        *y = (int)((0.5f - ny * 0.5f) * (float)H);
        return *x >= 0 && *x < W && *y >= 0 && *y < H;
    }
};

GraphicsSettings sun_only(int cascades) {
    GraphicsSettings g;
    g.apply_preset(GraphicsPreset::Low);
    g.allow_gpu_effects_on_cpu = true;
    g.shadows = true;
    g.shadow_resolution = 1024;
    g.soft_shadows = false;
    g.shadow_distance = 60.0f;
    g.shadow_cascades = cascades;
    g.pbr = false;
    return g;
}

void test_cascades() {
    Renderer r;
    r.set_settings(sun_only(3));
    CHECK(r.init(W * 2, H * 2));
    SunRig rig;
    // frame sizes: render at 2x so the far shadows have a few pixels
    auto lum_at = [&](Vec3 p) {
        int x, y;
        if (!rig.project(p, &x, &y)) return -1;
        Px c = px(r, x * 2 + 1, y * 2 + 1);
        return lum(c);
    };
    if (!render_ok(r, rig.scene)) return;
    const GpuTierStats st = r.gpu_tier_stats();
    CHECK_EQ(st.cascades, 3);
    CHECK_EQ(st.cascades_drawn, 3);
    CHECK(st.cascade_texel[0] > 0 && st.cascade_texel[0] < st.cascade_texel[1] && st.cascade_texel[1] < st.cascade_texel[2]);
    // a shadow beside every cube, near, middle and far: dark next to the lit ground
    float zs[3] = { -6.0f, -28.0f, -46.0f };
    for (int i = 0; i < 3; i++) {
        int shadow = lum_at(Vec3(1.8f, 0.0f, zs[i])), lit = lum_at(Vec3(-3.5f, 0.0f, zs[i]));
        CHECK_MSG(shadow >= 0 && lit > 0 && shadow < lit * 0.75f, "a cube's shadow reaches the ground at every distance");
    }
    // the same frame again: nothing is redrawn
    CHECK(render_ok(r, rig.scene));
    CHECK_EQ(r.gpu_tier_stats().cascades_drawn, 0);
    CHECK_EQ(r.gpu_tier_stats().cascades_reused, 3);
    CHECK(r.gpu_tier_stats().shadow_reused);
    // a tiny camera move stays inside the same texel snapped fit; a far cube moving does not touch the near cascade
    rig.camera(Vec3(0.0001f, 4, 0));
    CHECK(render_ok(r, rig.scene));
    CHECK_MSG(r.gpu_tier_stats().cascades_drawn == 0, "texel snapping: a tiny move redraws nothing");
    rig.set_cube_z(-6.0f, -28.0f, -47.0f);
    CHECK(render_ok(r, rig.scene));
    CHECK_MSG(r.gpu_tier_stats().cascades_drawn >= 1 && r.gpu_tier_stats().cascades_drawn < 3,
              "moving a far caster leaves the near cascades alone");
    rig.set_cube_z(-6.0f, -28.0f, -46.0f);
    CHECK(render_ok(r, rig.scene));

    // one cascade is the old single map: its texel is bigger than the near cascade's
    GraphicsSettings g1 = sun_only(1);
    r.set_settings(g1);
    CHECK(render_ok(r, rig.scene));
    CHECK_EQ(r.gpu_tier_stats().cascades, 1);
    float single = r.gpu_tier_stats().cascade_texel[0];
    CHECK_MSG(single > st.cascade_texel[0] * 1.5f, "the near cascade is sharper than a single map");
    int s1 = lum_at(Vec3(1.8f, 0.0f, -6.0f)), l1 = lum_at(Vec3(-3.5f, 0.0f, -6.0f));
    CHECK(s1 < l1 * 0.75f);

    // far cascades keep their fit while the camera drifts: fewer redraws than with the cache off
    int drawn[2] = {};
    for (int cache = 0; cache < 2; cache++) {
        GraphicsSettings g = sun_only(3);
        g.cascade_cache = cache != 0;
        r.set_settings(g);
        rig.camera(Vec3(0, 4, 0));
        CHECK(render_ok(r, rig.scene));
        for (int f = 0; f < 8; f++) {
            rig.camera(Vec3(0.4f * (float)(f + 1), 4, 0));
            if (!render_ok(r, rig.scene)) break;
            drawn[cache] += r.gpu_tier_stats().cascades_drawn;
        }
        // and the shadows are still right while cached
        int sh = lum_at(Vec3(1.8f, 0.0f, -28.0f)), lt = lum_at(Vec3(-3.5f, 0.0f, -28.0f));
        CHECK_MSG(sh >= 0 && sh < lt * 0.75f, "a cached far cascade still shadows");
    }
    CHECK_MSG(drawn[1] < drawn[0], "the cache redraws the far cascades less often");
    CHECK(drawn[0] <= 8 * 3);
}

// ---------------------------------------------------------------- settings as text and choices

void test_settings_text() {
    // every property reads back what was set, by name
    GraphicsSettings g;
    char v[64], err[160];
    // the effects options (engine/fx) are properties too: text, presets, ranges
    {
        GraphicsSettings f;
        CHECK(graphics_set_text(&f, "particle_density", "0.5", err, sizeof err) && f.fx_particle_density == 0.5f);
        CHECK(graphics_set_text(&f, "particles", "off") && !f.fx_particles);
        CHECK(graphics_set_text(&f, "sprites", "off") && !f.fx_sprites);
        CHECK(graphics_set_text(&f, "particle_depth_test", "off") && !f.fx_depth_test);
        CHECK(graphics_set_text(&f, "audio_reverb", "off") && !f.fx_reverb);
        CHECK(!graphics_set_text(&f, "particle_density", "5", err, sizeof err));
        size_t tn = 0;
        char* txt = graphics_settings_to_text(f, &tn);
        GraphicsSettings back;
        CHECK(txt && graphics_settings_from_text(&back, txt, tn) == 0 && !back.fx_particles && back.fx_particle_density == 0.5f && !back.fx_sprites && !back.fx_reverb);
        free(txt);
        GraphicsSettings lo, hi;
        lo.apply_preset(GraphicsPreset::Low);
        hi.apply_preset(GraphicsPreset::Ultra);
        CHECK(lo.fx_particle_density < hi.fx_particle_density && !lo.fx_depth_test && hi.fx_depth_test && lo.fx_particles && lo.fx_sprites);
    }
    CHECK(graphics_set_text(&g, "aa", "off", err, sizeof err));
    CHECK(graphics_get_text(g, "aa", v, sizeof v) && !strcmp(v, "off") && !g.antialiasing);
    CHECK(g.preset == GraphicsPreset::Custom);
    CHECK(graphics_set_text(&g, "aa", "fxaa"));
    CHECK(g.antialiasing);
    CHECK(graphics_set_text(&g, "shadows", "single"));
    CHECK(g.shadows && g.shadow_cascades == 1);
    CHECK(graphics_set_text(&g, "shadows", "cascades") && g.shadows && g.shadow_cascades >= 2);
    CHECK(graphics_set_text(&g, "shadow_cascades", "2") && g.shadow_cascades == 2);
    CHECK(graphics_set_text(&g, "shadows", "off") && !g.shadows);
    CHECK(graphics_set_text(&g, "bloom", "simple") && g.bloom && g.bloom_mode == BloomMode::Simple);
    CHECK(graphics_set_text(&g, "bloom", "chain") && g.bloom_mode == BloomMode::Chain);
    CHECK(graphics_set_text(&g, "bloom", "off") && !g.bloom);
    CHECK(graphics_set_text(&g, "shadow_resolution", "4096") && g.shadow_resolution == 4096);
    CHECK(graphics_set_text(&g, "render_scale", "0.5") && g.render_scale == 0.5f);
    CHECK(graphics_set_text(&g, "dynamic_resolution", "on") && g.dynamic_resolution);
    // invalid names and values are refused with a reason, and change nothing
    GraphicsSettings before = g;
    CHECK(!graphics_set_text(&g, "no_such_thing", "1", err, sizeof err) && strstr(err, "unknown graphics setting"));
    CHECK(!graphics_set_text(&g, "aa", "msaa", err, sizeof err) && strstr(err, "allowed: off, fxaa, smaa, taa"));
    CHECK(!graphics_set_text(&g, "render_scale", "7", err, sizeof err) && strstr(err, "0.25 to 1"));
    CHECK(!graphics_set_text(&g, "render_scale", "abc"));
    CHECK(!graphics_set_text(&g, "shadow_cascades", "2.5"));
    CHECK(!graphics_set_text(&g, "pbr", "maybe"));
    CHECK(memcmp(&before, &g, sizeof g) == 0);
    // presets by name; Low is the cheap base tier
    CHECK(graphics_apply_preset_name(&g, "low") && g.preset == GraphicsPreset::Low);
    CHECK(!g.shadows && !g.bloom && !g.antialiasing && !g.ambient_occlusion && !g.pbr);
    CHECK(!graphics_apply_preset_name(&g, "nope"));
    // all properties have help and a sane range
    for (int i = 0; i < graphics_property_count(); i++) {
        const GraphicsProperty& p = graphics_property(i);
        CHECK(p.name && p.help && graphics_property_find(p.name) == i);
        if (p.kind == GraphicsPropKind::Choice) CHECK(p.choices && p.choices[0] && p.choices[1]);
    }

    // text round trip for every preset and for custom values
    char t1[2048], t2[2048];
    const GraphicsPreset presets[] = { GraphicsPreset::Low, GraphicsPreset::Medium, GraphicsPreset::High, GraphicsPreset::Ultra };
    for (GraphicsPreset pr : presets) {
        GraphicsSettings a;
        a.apply_preset(pr);
        a.render_scale = 0.75f;                     // not part of a preset
        size_t n = graphics_settings_to_text(a, t1, sizeof t1);
        CHECK(n > 100 && n < sizeof t1 && !strncmp(t1, "farcade-graphics 1\n", 19));
        GraphicsSettings b;
        CHECK_EQ(graphics_settings_from_text(&b, t1, n), 0);
        graphics_settings_to_text(b, t2, sizeof t2);
        CHECK_STR(t1, t2);
        CHECK(b.preset == pr);
    }
    // a changed value under a preset label makes it Custom, and survives the round trip
    {
        GraphicsSettings a;
        a.apply_preset(GraphicsPreset::High);
        a.shadow_distance = 63.5f;
        size_t n = graphics_settings_to_text(a, t1, sizeof t1);
        GraphicsSettings b;
        CHECK_EQ(graphics_settings_from_text(&b, t1, n), 0);
        CHECK(b.preset == GraphicsPreset::Custom && b.shadow_distance == 63.5f);
    }
    // unknown fields are ignored, missing ones keep the defaults, bad values are counted
    const char* partial = "farcade-graphics 1\n# a comment\nfuture_feature 12\naa off\nrender_scale 0.6\nbloom wobbly\nshadow_resolution 1024\n\nlonely\n";
    GraphicsSettings d;
    int ignored = graphics_settings_from_text(&d, partial, strlen(partial));
    CHECK_EQ(ignored, 3);                           // future_feature, bloom wobbly, lonely
    CHECK(!d.antialiasing && d.render_scale == 0.6f && d.shadow_resolution == 1024);
    GraphicsSettings def;
    CHECK(d.bloom == def.bloom && d.pbr == def.pbr && d.shadow_cascades == def.shadow_cascades && d.local_shadows == def.local_shadows);
    CHECK(d.preset == GraphicsPreset::Custom);
    CHECK_EQ(graphics_settings_from_text(&d, "", 0), 0);
    CHECK(d.preset == def.preset);
    CHECK_EQ(graphics_settings_from_text(&d, nullptr, 0), -1);
    // truncation is safe and reported
    char tiny[16];
    size_t need = graphics_settings_to_text(def, tiny, sizeof tiny);
    CHECK(need >= sizeof tiny && tiny[15] == 0);
}

void test_bloom_modes() {
    // Simple ignores the radius (the first bloom); Chain uses it; both are selectable after the chain exists.
    Renderer r;
    GraphicsSettings g;
    g.apply_preset(GraphicsPreset::Low);
    g.allow_gpu_effects_on_cpu = true;
    g.shadows = false; g.ambient_occlusion = false; g.antialiasing = false;
    g.bloom = true;
    r.set_settings(g);
    CHECK(r.init(160, 120));
    Vec3 cp[kCubeVertices], cn[kCubeVertices];
    make_cube(cp, cn, 0.4f);
    DrawItem it;
    it.mesh = RenderMesh{ cp, cn, kCubeVertices, 8 };
    it.model = Mat4::translation(Vec3(-1.0f, 0, 0));
    it.unlit = true;
    it.color = Vec3(3, 3, 3);
    FlatScene f(Vec3(0.01f, 0.01f, 0.01f));
    f.scene.camera.view = Mat4::look_at(Vec3(0, 0, 4), Vec3(0, 0, 0), Vec3(0, 1, 0));
    f.scene.items = &it;
    f.scene.item_count = 1;
    f.scene.post.bloom = 1.0f;
    f.scene.post.bloom_threshold = 0.5f;
    uint32_t img[3][160 * 120];
    int k = 0;
    const BloomMode modes[3] = { BloomMode::Simple, BloomMode::Simple, BloomMode::Chain };
    for (BloomMode m : modes) {
        g.bloom_mode = m;
        r.set_settings(g);
        f.scene.post.bloom_radius = k == 0 ? 0.0f : 1.0f;
        if (render_ok(r, f.scene)) memcpy(img[k], r.pixels(), sizeof img[k]);
        k++;
    }
    CHECK_MSG(memcmp(img[0], img[1], sizeof img[0]) == 0, "simple bloom ignores the radius");
    CHECK_MSG(memcmp(img[1], img[2], sizeof img[1]) != 0, "the chain differs from the simple bloom");
}

// ---------------------------------------------------------------- probe GI

// A ground plane, a big red wall at x = +4, a sun from the left (so the wall is in shade ... and the sun lights the ground).
struct GiRoom {
    Vec3 cube_p[kCubeVertices], cube_n[kCubeVertices], plane_p[kPlaneVertices], plane_n[kPlaneVertices];
    DrawItem items[3];
    RenderLight sun;
    GiInput in;
    GiRoom() {
        make_cube(cube_p, cube_n, 1.0f);
        make_plane(plane_p, plane_n, 10.0f, 0.0f);
        items[0].mesh = RenderMesh{ plane_p, plane_n, kPlaneVertices, 31 };
        items[0].model = Mat4::identity();
        items[0].color = Vec3(0.8f, 0.8f, 0.8f);
        items[1].mesh = RenderMesh{ cube_p, cube_n, kCubeVertices, 32 };       // the red wall: 0.4 x 4 x 8
        items[1].model = Mat4::trs(Vec3(4.5f, 2.0f, 0), Quat::axis_angle(Vec3(0, 1, 0), 0), Vec3(0.2f, 2.0f, 4.0f));
        items[1].color = Vec3(0.9f, 0.1f, 0.05f);
        items[2].mesh = RenderMesh{ cube_p, cube_n, kCubeVertices, 33 };       // a light blue box on the other side
        items[2].model = Mat4::trs(Vec3(-4.5f, 1.0f, 0), Quat::axis_angle(Vec3(0, 1, 0), 0), Vec3(0.5f, 1.0f, 1.0f));
        items[2].color = Vec3(0.1f, 0.2f, 0.9f);
        sun.type = LightType::Directional;
        sun.direction = normalize(Vec3(0.8f, -0.6f, 0.1f));     // travels towards +x: lights the faces that look at -x
        sun.intensity = 2.0f;
        in.items = items;
        in.item_count = 3;
        in.lights = &sun;
        in.light_count = 1;
        in.sky_color = Vec3(0.3f, 0.4f, 0.6f);
        in.ground_color = Vec3(0.1f, 0.1f, 0.1f);
        in.camera = Vec3(0, 1.5f, 0);
    }
};

void test_gi_voxels_and_probes() {
    GiRoom room;
    GiParams gp;
    // voxelization: conservative and deterministic
    GiSystem a, b;
    a.update(room.in, gp, true);
    b.update(room.in, gp, true);
    CHECK(a.stats().voxels > 400 && a.stats().revoxelized);
    CHECK_EQ(a.stats().voxels, b.stats().voxels);
    static uint32_t ta[GiSystem::kTexW * GiSystem::kTexH], tb[GiSystem::kTexW * GiSystem::kTexH];
    a.encode(ta);
    b.encode(tb);
    CHECK_MSG(memcmp(ta, tb, sizeof ta) == 0, "the same scene gives the same probes");
    CHECK(a.occupied(Vec3(0.2f, 0.05f, 0.3f)));                    // the ground
    CHECK(a.occupied(Vec3(4.5f, 2.0f, 0.0f)));                     // inside the wall
    CHECK(!a.occupied(Vec3(0.0f, 3.0f, 0.0f)));                    // air
    CHECK(!a.occupied(Vec3(2.0f, 0.8f, 2.0f)));
    // every voxel the wall's surface passes through is marked (a thin plane tilted: no gaps along a line)
    int gaps = 0;
    for (int i = 0; i < 40; i++) if (!a.occupied(Vec3(-9.5f + 0.5f * (float)i, 0.1f, 1.7f))) gaps++;
    CHECK_EQ(gaps, 0);

    // colour bleeding: next to the red wall the ambient is red; away from it, not
    Vec3 near_wall = a.ambient(Vec3(3.0f, 1.0f, 0), Vec3(1, 0, 0));
    Vec3 far_side = a.ambient(Vec3(-2.0f, 1.0f, 0), Vec3(1, 0, 0));
    CHECK_MSG(near_wall.x > near_wall.z * 1.5f && near_wall.x > far_side.x, "light bounced off the red wall");
    Vec3 near_blue = a.ambient(Vec3(-3.0f, 1.0f, 0), Vec3(-1, 0, 0));
    CHECK_MSG(near_blue.z > near_blue.x, "and off the blue box");
    // multi-bounce: more passes only add light
    Vec3 once = a.ambient(Vec3(1.0f, 0.5f, 0), Vec3(0, 1, 0));
    CHECK(once.x > 0.01f && once.y > 0.01f && once.z > 0.01f);

    // convergence: from a wrong state (a red sky), a fraction of the probes per frame with hysteresis settles
    // on the answer a long run gives (multi-bounce keeps adding a little for a while)
    GiParams slow = gp;
    slow.update = GiUpdate::Fast;
    slow.hysteresis = 0.8f;
    GiInput other = room.in;
    other.sky_color = Vec3(1.0f, 0.2f, 0.2f);
    GiSystem d, ref;
    d.update(other, slow, true);
    ref.update(room.in, slow, true);
    for (int i = 0; i < 400; i++) ref.update(room.in, slow);
    Vec3 target = ref.ambient(Vec3(0, 1.0f, 0), Vec3(0, 1, 0));
    Vec3 start = d.ambient(Vec3(0, 1.0f, 0), Vec3(0, 1, 0));
    float prev_err = fabsf(start.x - target.x) + fabsf(start.y - target.y) + fabsf(start.z - target.z);
    float first_err = prev_err;
    int decreasing = 0;
    for (int i = 0; i < 240; i++) {
        d.update(room.in, slow);
        if (i % 24 == 23) {
            Vec3 v = d.ambient(Vec3(0, 1.0f, 0), Vec3(0, 1, 0));
            float err = fabsf(v.x - target.x) + fabsf(v.y - target.y) + fabsf(v.z - target.z);
            if (err < prev_err) decreasing++;
            prev_err = err;
        }
    }
    CHECK_MSG(first_err > 0.2f && decreasing >= 8 && prev_err < 0.06f * (target.x + target.y + target.z), "probes converge");
    CHECK(d.stats().probes_updated <= GiSystem::kProbeCount / 8 * 2 + 1);

    // an open sky with no geometry: the probes reproduce the hemisphere (L1 is exact for a linear gradient)
    GiInput empty = room.in;
    empty.item_count = 0;
    GiSystem e;
    e.update(empty, gp, true);
    Vec3 up = e.ambient(Vec3(0, 1.5f, 0), Vec3(0, 1, 0)), down = e.ambient(Vec3(0, 1.5f, 0), Vec3(0, -1, 0));
    // the probes hold diffuse irradiance: (s + g) / 2 +- (s - g) / 3 for a linear gradient
    CHECK(fabsf(up.z - 0.5167f) < 0.05f && fabsf(down.z - 0.1833f) < 0.05f);

    // a probe inside a closed box sees back faces: it is invalid and falls back to the hemisphere, not black
    Vec3 inside_p[kCubeVertices], inside_n[kCubeVertices];
    make_cube(inside_p, inside_n, 1.0f);
    DrawItem box;
    box.mesh = RenderMesh{ inside_p, inside_n, kCubeVertices, 34 };
    box.model = Mat4::trs(Vec3(0, 1.5f, 0), Quat::axis_angle(Vec3(0, 1, 0), 0), Vec3(3.0f, 3.0f, 3.0f));
    GiInput boxed = empty;
    boxed.items = &box;
    boxed.item_count = 1;
    boxed.camera = Vec3(0, 1.5f, 0);
    GiSystem f;
    f.update(boxed, gp, true);
    CHECK_MSG(f.stats().probes_invalid > 0, "probes inside geometry are invalidated");
    Vec3 in_box = f.ambient(Vec3(0, 1.5f, 0), Vec3(0, 1, 0));
    CHECK(in_box.x + in_box.y + in_box.z > 0.1f);

    // cost: a forced full update and a steady frame
    GiSystem g;
    g.update(room.in, gp, true);
    double full = g.stats().ms;
    double steady = 0;
    for (int i = 0; i < 20; i++) { g.update(room.in, gp); steady += g.stats().ms; }
    steady /= 20;
    printf("gi: first frame %.1f ms, steady %.2f ms/frame, %d surface voxels\n", full, steady, g.stats().voxels);
    CHECK_MSG(steady < 12.0, "steady GI update stays within a few core-ms");
}

void test_gi_render() {
    // The probes feed the shaders: the ground beside a red wall turns red, in the base tier and the GPU tier;
    // the hemisphere option stays as it was.
    GiRoom room;
    Renderer r;
    GraphicsSettings g;
    g.apply_preset(GraphicsPreset::Low);
    r.set_settings(g);
    CHECK(r.init(W, H));
    RenderScene scene;
    scene.items = room.items;
    scene.item_count = 3;
    scene.lights = &room.sun;
    scene.light_count = 1;
    scene.sky.mode = SkyMode::Color;
    scene.sky.zenith = Vec3(0.2f, 0.3f, 0.5f);
    scene.sky_light.sky_color = Vec3(0.3f, 0.4f, 0.6f);
    scene.sky_light.ground_color = Vec3(0.1f, 0.1f, 0.1f);
    scene.sky_light.intensity = 1.0f;
    scene.post.tonemap = Tonemap::None;
    scene.post.vignette = 0;
    scene.delta_time = 0.016f;
    scene.camera.position = Vec3(0, 3.0f, 9.0f);
    scene.camera.view = Mat4::look_at(scene.camera.position, Vec3(0, 0.5f, 0), Vec3(0, 1, 0));
    scene.camera.proj = Mat4::perspective(radians(50.0f), (float)W / H, 0.1f, 100.0f);
    auto ground_px = [&](Vec3 p) {
        Vec3 v = transform_point(scene.camera.view, p);
        float nx = scene.camera.proj.m[0] * v.x / -v.z, ny = scene.camera.proj.m[5] * v.y / -v.z;
        return px(r, (int)((nx * 0.5f + 0.5f) * (float)W), (int)((0.5f - ny * 0.5f) * (float)H));
    };
    // hemisphere
    if (!render_ok(r, scene)) return;
    Px hemi_near = ground_px(Vec3(3.4f, 0.0f, 1.0f)), hemi_far = ground_px(Vec3(-1.0f, 0.0f, 1.0f));
    CHECK_EQ(r.stats().gi_ms > 0.0 ? 1 : 0, 0);
    CHECK(!r.features().gi);
    // probes (base tier): the first frame converges all probes
    g.gi_mode = GiMode::Probes;
    r.set_settings(g);
    for (int i = 0; i < 3; i++) if (!render_ok(r, scene)) return;
    CHECK(r.features().gi && !r.features().gpu_tier && r.stats().gi_ms >= 0.0);
    Px gi_near = ground_px(Vec3(3.4f, 0.0f, 1.0f)), gi_far = ground_px(Vec3(-1.0f, 0.0f, 1.0f));
    CHECK_MSG(gi_near.r - gi_near.b > hemi_near.r - hemi_near.b + 6, "red bounce next to the wall (base tier)");
    CHECK_MSG(abs((gi_far.r - gi_far.b) - (hemi_far.r - hemi_far.b)) < 40, "the far side is not painted red");
    // the GPU tier (PBR shading) reads the same probes
    g.allow_gpu_effects_on_cpu = true;
    g.shadows = false; g.ambient_occlusion = false; g.bloom = false; g.antialiasing = false;
    g.pbr = true;
    g.gi_mode = GiMode::Hemisphere;
    r.set_settings(g);
    CHECK(render_ok(r, scene));
    Px t_hemi = ground_px(Vec3(3.4f, 0.0f, 1.0f));
    g.gi_mode = GiMode::Probes;
    r.set_settings(g);
    for (int i = 0; i < 2; i++) if (!render_ok(r, scene)) return;
    CHECK(r.features().gpu_tier && r.features().gi);
    Px t_gi = ground_px(Vec3(3.4f, 0.0f, 1.0f));
    CHECK_MSG(t_gi.r - t_gi.b > t_hemi.r - t_hemi.b + 6, "red bounce in the GPU tier");
    // switching back is the old image, bit for bit
    g.gi_mode = GiMode::Hemisphere;
    r.set_settings(g);
    CHECK(render_ok(r, scene));
    Px back = ground_px(Vec3(3.4f, 0.0f, 1.0f));
    CHECK(near_px(back, t_hemi, 0));
}

// ---------------------------------------------------------------- aa / ao / ssr choices

// A white cube on a dark floor, turned so its edges are diagonal; unlit, no tonemap: edge pixels are pure geometry.
struct EdgeRig {
    Vec3 cube_p[kCubeVertices], cube_n[kCubeVertices];
    DrawItem it;
    FlatScene flat;
    EdgeRig() : flat(Vec3(0.02f, 0.02f, 0.02f)) {
        make_cube(cube_p, cube_n, 0.8f);
        it.mesh = RenderMesh{ cube_p, cube_n, kCubeVertices, 41 };
        it.model = Mat4::rotation(Quat::axis_angle(Vec3(0, 0, 1), 0.5f)) * Mat4::rotation(Quat::axis_angle(Vec3(0, 1, 0), 0.4f));
        it.unlit = true;
        it.color = Vec3(0.9f, 0.9f, 0.9f);
        flat.scene.items = &it;
        flat.scene.item_count = 1;
        flat.scene.camera.view = Mat4::look_at(Vec3(0, 0, 4), Vec3(0, 0, 0), Vec3(0, 1, 0));
        flat.scene.post.tonemap = Tonemap::None;
    }
};

GraphicsSettings tier_only(const char* aa, const char* ao = "off", const char* ssr = "off") {
    GraphicsSettings g;
    g.apply_preset(GraphicsPreset::Low);
    g.allow_gpu_effects_on_cpu = true;
    graphics_set_text(&g, "aa", aa);
    graphics_set_text(&g, "ao", ao);
    graphics_set_text(&g, "ssr", ssr);
    g.shadows = false; g.bloom = false; g.pbr = true;
    return g;
}

int lumi(uint32_t v) { return (int)(((v & 0xFF) * 3 + ((v >> 8) & 0xFF) * 6 + ((v >> 16) & 0xFF)) / 10); }

void test_aa_choices() {
    // the property table: aa off | fxaa | smaa | taa, ao off | ssao | gtao, ssr off | on
    GraphicsSettings g;
    char v[32], err[160];
    const char* aas[] = { "off", "fxaa", "smaa", "taa" };
    for (const char* a : aas) { CHECK(graphics_set_text(&g, "aa", a) && graphics_get_text(g, "aa", v, sizeof v) && !strcmp(v, a)); }
    CHECK(graphics_set_text(&g, "aa", "smaa") && g.antialiasing && g.aa_mode == AAMode::SMAA);
    CHECK(graphics_set_text(&g, "aa", "off") && !g.antialiasing && g.aa_mode == AAMode::SMAA);     // the mode is remembered
    const char* aos[] = { "off", "ssao", "gtao" };
    for (const char* a : aos) { CHECK(graphics_set_text(&g, "ao", a) && graphics_get_text(g, "ao", v, sizeof v) && !strcmp(v, a)); }
    CHECK(graphics_set_text(&g, "ssr", "on") && g.ssr && graphics_get_text(g, "ssr", v, sizeof v) && !strcmp(v, "on"));
    CHECK(!graphics_set_text(&g, "ssr", "maybe", err, sizeof err) && strstr(err, "allowed: off, on"));
    // presets: Low and Medium keep the old techniques, High takes SMAA, Ultra everything
    const GraphicsPreset lows[] = { GraphicsPreset::Low, GraphicsPreset::Medium };
    for (GraphicsPreset p : lows) {
        GraphicsSettings a;
        a.apply_preset(p);
        CHECK(!a.ssr && a.ao_mode == AOMode::SSAO && a.aa_mode == AAMode::FXAA);
    }
    GraphicsSettings hi, ul;
    hi.apply_preset(GraphicsPreset::High);
    ul.apply_preset(GraphicsPreset::Ultra);
    CHECK(hi.aa_mode == AAMode::SMAA && !hi.ssr && ul.aa_mode == AAMode::TAA && ul.ao_mode == AOMode::GTAO && ul.ssr);
    // FXAA stays available after SMAA and TAA exist, and the text round-trips every mode
    for (const char* a : aas) {
        GraphicsSettings x;
        graphics_set_text(&x, "aa", a);
        graphics_set_text(&x, "ao", "gtao");
        graphics_set_text(&x, "ssr", "on");
        char t1[2048], t2[2048];
        size_t n = graphics_settings_to_text(x, t1, sizeof t1);
        GraphicsSettings y;
        CHECK_EQ(graphics_settings_from_text(&y, t1, n), 0);
        graphics_settings_to_text(y, t2, sizeof t2);
        CHECK_STR(t1, t2);
        CHECK(y.ssr && y.ao_mode == AOMode::GTAO);
    }
    // Low is bit-identical to the cheap path of before: the smoke scene checksum of the base tier did not change
    SmokeScene sm;
    sm.build();
    Renderer r;
    GraphicsSettings low;
    low.apply_preset(GraphicsPreset::Low);
    r.set_settings(low);
    CHECK(r.init(kSmokeWidth, kSmokeHeight));
    if (render_ok(r, sm.scene)) CHECK_MSG(checksum(r.pixels(), kSmokeWidth * kSmokeHeight) == 0x214c411eu, "Low stays bit-identical");
    // the new modes need the GPU tier: on the CPU backend they change nothing unless it is forced
    GraphicsSettings cpu = low;
    graphics_set_text(&cpu, "aa", "taa");
    graphics_set_text(&cpu, "ao", "gtao");
    graphics_set_text(&cpu, "ssr", "on");
    r.set_settings(cpu);
    CHECK(!r.features().gpu_tier && !r.features().ssr);
    if (render_ok(r, sm.scene)) CHECK(checksum(r.pixels(), kSmokeWidth * kSmokeHeight) == 0x214c411eu);
}

void test_smaa() {
    // the area model
    float tb, ta;
    smaa_area(0, 0, 3, 3, &tb, &ta);
    CHECK(tb == 0 && ta == 0);                                  // an isolated edge is left alone
    smaa_area(1, 2, 0, 7, &tb, &ta);                            // a Z: up at the start, down at the end
    float zsum = 0;
    for (int d1 = 0; d1 < 8; d1++) { smaa_area(1, 2, d1, 7 - d1, &tb, &ta); zsum += tb + ta; CHECK(tb + ta <= 0.5f + 1e-4f); }
    CHECK(zsum > 0.5f);
    smaa_area(1, 1, 0, 6, &tb, &ta);                            // a U / L ramps: the largest share is at the ends
    float end_share = tb + ta;
    smaa_area(1, 1, 3, 3, &tb, &ta);
    CHECK(end_share > tb + ta);
    float a1, b1, a2, b2;                                       // mirror: the same shape the other way round
    smaa_area(1, 0, 1, 5, &a1, &b1);
    smaa_area(0, 1, 5, 1, &a2, &b2);
    CHECK_NEAR(a1 + b1, a2 + b2);
    uint32_t tex[kSmaaAreaSize * kSmaaAreaSize];
    smaa_area_texture(tex);
    CHECK(tex[0] == 0);                                         // cross 0 / 0: nothing
    int nonzero = 0;
    for (uint32_t t : tex) nonzero += t != 0;
    CHECK(nonzero > 100);

    // the pass: smooth edges, flat areas untouched
    Renderer r;
    r.set_settings(tier_only("off"));
    CHECK(r.init(W, H));
    EdgeRig rig;
    if (!render_ok(r, rig.flat.scene)) return;
    uint32_t off[W * H];
    memcpy(off, r.pixels(), sizeof off);
    GraphicsSettings g = tier_only("smaa");
    r.set_settings(g);
    CHECK(r.features().gpu_tier && r.features().aa_mode == AAMode::SMAA);
    if (!render_ok(r, rig.flat.scene)) return;
    CHECK(r.gpu_tier_stats().smaa_ms > 0);
    int changed = 0, flat_changed = 0, new_levels = 0;
    for (int y = 2; y < H - 2; y++)
        for (int x = 2; x < W - 2; x++) {
            uint32_t a = off[y * W + x], b = r.pixels()[y * W + x];
            if (a == b) continue;
            changed++;
            // near an edge of the aliased image?
            int mx = 0;
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -2; dx <= 2; dx++) { int d = abs(lumi(off[(y + dy) * W + x + dx]) - lumi(a)); mx = d > mx ? d : mx; }
            if (mx < 40) flat_changed++;
            int lb = lumi(b);
            if (lb > 40 && lb < 190) new_levels++;
        }
    CHECK_MSG(changed > 20, "SMAA changes the edge pixels");
    CHECK_MSG(flat_changed == 0, "and nothing away from an edge");
    CHECK_MSG(new_levels > 15, "edge pixels get intermediate values");
    // FXAA still works next to it, and gives a different (also smoothed) result
    r.set_settings(tier_only("fxaa"));
    if (render_ok(r, rig.flat.scene)) {
        CHECK(r.gpu_tier_stats().fxaa_ms > 0 && r.gpu_tier_stats().smaa_ms == 0);
        CHECK(memcmp(off, r.pixels(), sizeof off) != 0);
    }
}

// the mean absolute difference of two images (luma)
double image_diff(const uint32_t* a, const uint32_t* b, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += abs(lumi(a[i]) - lumi(b[i]));
    return s / n;
}

void test_taa() {
    Renderer r;
    CHECK(r.init(W, H));
    EdgeRig rig;
    r.set_settings(tier_only("off"));
    if (!render_ok(r, rig.flat.scene)) return;
    static uint32_t ref[W * H], prev[W * H], cur[W * H];
    memcpy(ref, r.pixels(), sizeof ref);
    GraphicsSettings g = tier_only("taa");
    r.set_settings(g);
    // a still scene: the jitter averages the edge (anti-aliased), and the picture settles
    for (int i = 0; i < 20; i++) {
        if (!render_ok(r, rig.flat.scene)) return;
        if (i == 18) memcpy(prev, r.pixels(), sizeof prev);
    }
    memcpy(cur, r.pixels(), sizeof cur);
    CHECK(r.gpu_tier_stats().taa_ms > 0);
    int mid = 0;
    for (int i = 0; i < W * H; i++) { int l = lumi(cur[i]); if (l > 40 && l < 190) mid++; }
    int mid_ref = 0;
    for (int i = 0; i < W * H; i++) { int l = lumi(ref[i]); if (l > 40 && l < 190) mid_ref++; }
    CHECK_MSG(mid > mid_ref + 10, "TAA gives edge pixels intermediate values");
    CHECK_MSG(image_diff(prev, cur, W * H) < 1.2, "a still scene is stable from frame to frame");
    CHECK_MSG(image_diff(ref, cur, W * H) < 6.0, "and stays close to the unfiltered picture");
    // a moving camera: reprojection keeps the history on the objects (no smear): close to the unfiltered picture
    double worst = 0;
    for (int f = 0; f < 14; f++) {
        float x = 0.03f * (float)f;
        rig.flat.scene.camera.position = Vec3(x, 0, 4);
        rig.flat.scene.camera.view = Mat4::look_at(Vec3(x, 0, 4), Vec3(x, 0, 0), Vec3(0, 1, 0));
        if (!render_ok(r, rig.flat.scene)) return;
        if (f >= 6) {
            memcpy(cur, r.pixels(), sizeof cur);
            Renderer r2;
            r2.set_settings(tier_only("off"));
            r2.init(W, H);
            r2.render(rig.flat.scene);
            double d = image_diff(cur, r2.pixels(), W * H);
            worst = d > worst ? d : worst;
        }
    }
    CHECK_MSG(worst < 7.0, "reprojection keeps a moving image sharp (no ghost trail)");
    // a camera cut: the history is not trusted outside the screen
    rig.flat.scene.camera.view = Mat4::look_at(Vec3(0, 0, 4), Vec3(0, 0, 0), Vec3(0, 1, 0));
    CHECK(render_ok(r, rig.flat.scene));

    // TAA drives the upscale: drawn at 60%, resolved at full size, closer to the converged full size TAA picture
    // than a plain upscale of the same internal image
    rig.flat.scene.camera.position = Vec3(0, 0, 4);
    rig.flat.scene.camera.view = Mat4::look_at(Vec3(0, 0, 4), Vec3(0, 0, 0), Vec3(0, 1, 0));
    r.set_settings(tier_only("taa"));
    for (int i = 0; i < 24; i++) if (!render_ok(r, rig.flat.scene)) return;
    static uint32_t best[W * H];
    memcpy(best, r.pixels(), sizeof best);
    GraphicsSettings up = tier_only("taa");
    up.render_scale = 0.6f;
    r.set_settings(up);
    for (int i = 0; i < 24; i++) if (!render_ok(r, rig.flat.scene)) return;
    CHECK(r.stats().taa_upscale && r.stats().internal_width < W && r.width() == W);
    static uint32_t taa_up[W * H];
    memcpy(taa_up, r.pixels(), sizeof taa_up);
    GraphicsSettings plain = tier_only("fxaa");
    plain.render_scale = 0.6f;
    r.set_settings(plain);
    CHECK(render_ok(r, rig.flat.scene));
    CHECK(!r.stats().taa_upscale && r.stats().internal_width < W);
    double e_taa = image_diff(taa_up, best, W * H), e_plain = image_diff(r.pixels(), best, W * H);
    CHECK_MSG(e_taa < e_plain, "temporal upscale beats the bilinear upscale");
    // TAA at scale 1 and the other modes give no taa_upscale
    r.set_settings(tier_only("taa"));
    CHECK(render_ok(r, rig.flat.scene));
    CHECK(!r.stats().taa_upscale);
}

void test_gtao_and_ssr() {
    // GTAO darkens a crease like SSAO does, and accumulates over still frames (less noise)
    Vec3 cp[kCubeVertices], cn[kCubeVertices], pp[kPlaneVertices], pn[kPlaneVertices];
    make_cube(cp, cn, 0.5f);
    make_plane(pp, pn, 20.0f, 0.0f);
    DrawItem items[2];
    items[0].mesh = RenderMesh{ cp, cn, kCubeVertices, 51 };
    items[0].model = Mat4::translation(Vec3(0, 0.5f, 0));
    items[1].mesh = RenderMesh{ pp, pn, kPlaneVertices, 52 };
    items[1].model = Mat4::identity();
    RenderScene scene;
    scene.items = items;
    scene.item_count = 2;
    scene.sky.mode = SkyMode::Color;
    scene.sky.zenith = Vec3(0.1f, 0.1f, 0.1f);
    scene.sky_light.intensity = 1.0f;
    scene.sky_light.sky_color = Vec3(1, 1, 1);
    scene.sky_light.ground_color = Vec3(1, 1, 1);
    scene.post.tonemap = Tonemap::None;
    scene.post.vignette = 0;
    scene.delta_time = 0.016f;
    scene.camera.position = Vec3(2.5f, 2.5f, 3.5f);
    scene.camera.view = Mat4::look_at(scene.camera.position, Vec3(0, 0.3f, 0), Vec3(0, 1, 0));
    scene.camera.proj = Mat4::perspective(radians(50.0f), (float)W / H, 0.1f, 100.0f);
    auto at = [&](Renderer& r, Vec3 p) {
        Vec3 v = transform_point(scene.camera.view, p);
        float nx = scene.camera.proj.m[0] * v.x / -v.z, ny = scene.camera.proj.m[5] * v.y / -v.z;
        return lumi(r.pixels()[(int)((0.5f - ny * 0.5f) * (float)H) * W + (int)((nx * 0.5f + 0.5f) * (float)W)]);
    };
    Renderer r;
    CHECK(r.init(W * 2, H * 2));
    // (render at 2x: the helper above reads the 1x position, so scale it)
    auto at2 = [&](Vec3 p) {
        Vec3 v = transform_point(scene.camera.view, p);
        float nx = scene.camera.proj.m[0] * v.x / -v.z, ny = scene.camera.proj.m[5] * v.y / -v.z;
        return lumi(r.pixels()[(int)((0.5f - ny * 0.5f) * (float)(H * 2)) * (W * 2) + (int)((nx * 0.5f + 0.5f) * (float)(W * 2))]);
    };
    (void)at;
    const Vec3 crease(0.56f, 0, 0.56f), open(-3.0f, 0, 3.0f);
    r.set_settings(tier_only("off", "off"));
    if (!render_ok(r, scene)) return;
    int c0 = at2(crease), o0 = at2(open);
    r.set_settings(tier_only("off", "gtao"));
    for (int i = 0; i < 6; i++) if (!render_ok(r, scene)) return;
    int c1 = at2(crease), o1 = at2(open);
    CHECK_MSG(c1 < c0 - 8, "GTAO darkens the crease between cube and floor");
    CHECK_MSG(abs(o1 - o0) <= 8, "and leaves the open floor");
    CHECK(r.gpu_tier_stats().ssao_ms > 0 && r.gpu_tier_surfaces() > 6);
    // the same frame again: the accumulated result is stable (history on)
    int c2 = 0;
    for (int i = 0; i < 4; i++) { render_ok(r, scene); c2 = at2(crease); }
    CHECK(abs(c2 - c1) <= 6);

    // SSR: a smooth metal floor reflects a bright cube; a rough floor does not change
    items[1].roughness = 0.05f;
    items[1].metallic = 1.0f;
    items[1].color = Vec3(0.9f, 0.9f, 0.9f);
    items[0].unlit = true;
    items[0].color = Vec3(3.0f, 0.5f, 0.2f);
    scene.sky_light.intensity = 0.2f;
    r.set_settings(tier_only("off", "off", "off"));
    if (!render_ok(r, scene)) return;
    Vec3 below(0.0f, 0.0f, 0.9f);                               // the floor in front of the cube: it mirrors the cube's front face
    int off_l = at2(below);
    uint32_t off_px[W * H * 4];
    memcpy(off_px, r.pixels(), sizeof off_px);
    r.set_settings(tier_only("off", "off", "on"));
    if (!render_ok(r, scene)) return;
    CHECK(r.features().ssr && r.gpu_tier_stats().ssr_ms > 0);
    int on_l = at2(below);
    CHECK_MSG(on_l > off_l + 12, "the cube is reflected in the smooth floor");
    items[1].roughness = 0.95f;
    items[1].metallic = 0.0f;
    r.set_settings(tier_only("off", "off", "off"));
    render_ok(r, scene);
    memcpy(off_px, r.pixels(), sizeof off_px);
    r.set_settings(tier_only("off", "off", "on"));
    render_ok(r, scene);
    int diff = 0;
    for (int y = 0; y < H * 2; y++) for (int x = 0; x < W * 2; x++) {
        // the floor pixels only (bottom part of the image)
        if (y > H * 2 * 3 / 4 && off_px[y * W * 2 + x] != r.pixels()[y * W * 2 + x]) diff++;
    }
    CHECK_MSG(diff == 0, "a rough floor gets no reflection");
}

void test_new_modes_everything() {
    // all of it at once: no leak, resize, and switching modes back and forth
    fe_fgpu_host_stats_t base, s1, s2;
    fe_fgpu_host_stats(&base);
    Renderer r;
    GraphicsSettings g;
    g.apply_preset(GraphicsPreset::Ultra);
    g.shadow_resolution = 1024;
    g.allow_gpu_effects_on_cpu = true;
    r.set_settings(g);
    CHECK(r.init(W, H));
    EdgeRig rig;
    rig.flat.scene.post.bloom = 0.5f;
    const char* modes[] = { "taa", "smaa", "fxaa", "off", "taa" };
    for (int i = 0; i < 5; i++) {
        graphics_set_text(&g, "aa", modes[i]);
        graphics_set_text(&g, "ao", i % 2 ? "gtao" : "ssao");
        r.set_settings(g);
        for (int k = 0; k < 3; k++) if (!render_ok(r, rig.flat.scene)) return;
        if (i == 1) fe_fgpu_host_stats(&s1);
    }
    const int sizes[][2] = { { 97, 61 }, { 33, 17 }, { 200, 150 }, { W, H } };
    for (auto& sz : sizes) {
        CHECK(r.resize(sz[0], sz[1]));
        rig.flat.scene.camera.proj = Mat4::perspective(radians(60.0f), (float)sz[0] / (float)sz[1], 0.1f, 100.0f);
        CHECK(render_ok(r, rig.flat.scene));
        CHECK(render_ok(r, rig.flat.scene));
    }
    fe_fgpu_host_stats(&s2);
    CHECK(s2.buffers == s1.buffers || s2.buffers >= 0);
    r.shutdown();
    fe_fgpu_host_stats(&s2);
    CHECK_EQ(s2.surfaces, base.surfaces);
    CHECK_EQ(s2.programs, base.programs);
    CHECK_EQ(s2.buffers, base.buffers);
}

}  // namespace

void test_ssaa_and_vsync() {
    // settings: ssaa off | 2x | 4x, vsync off | double | triple | adaptive, refresh_hz 30..240
    GraphicsSettings g;
    char v[32];
    CHECK(g.ssaa == SSAAMode::Off && g.vsync == VsyncMode::Double && g.refresh_hz == 60);
    const char* ss[] = { "off", "2x", "4x" };
    for (const char* a : ss) { CHECK(graphics_set_text(&g, "ssaa", a) && graphics_get_text(g, "ssaa", v, sizeof v) && !strcmp(v, a)); }
    const char* vs[] = { "off", "double", "triple", "adaptive" };
    for (const char* a : vs) { CHECK(graphics_set_text(&g, "vsync", a) && graphics_get_text(g, "vsync", v, sizeof v) && !strcmp(v, a)); }
    CHECK(graphics_set_text(&g, "refresh_hz", "144") && g.refresh_hz == 144);
    CHECK(!graphics_set_text(&g, "refresh_hz", "500"));
    // the area filter: a 2x image becomes the exact 2 x 2 average; a 1.5x one keeps the overall mean
    uint32_t src[16], dst[4];
    for (int i = 0; i < 16; i++) src[i] = (i % 4 < 2) == (i / 4 < 2) ? 0x00FFFFFFu : 0x00000000u;
    src[0] = 0x00FF0000u;                               // XRGB red in one sample of the first block
    downsample_xrgb_to_rgba(src, 4, 4, dst, 2, 2);
    CHECK((dst[0] & 0xFFu) == 255 && ((dst[0] >> 8) & 0xFFu) == 191 && ((dst[0] >> 16) & 0xFFu) == 191);
    CHECK((dst[1] & 0xFFFFFFu) == 0 && (dst[2] & 0xFFFFFFu) == 0 && (dst[3] & 0xFFFFFFu) == 0xFFFFFFu);
    uint32_t big[9 * 9], small[6 * 6];
    for (int i = 0; i < 81; i++) big[i] = (i & 1) ? 0x00FFFFFFu : 0;
    downsample_xrgb_to_rgba(big, 9, 9, small, 6, 6);
    long sum = 0;
    for (int i = 0; i < 36; i++) sum += small[i] & 0xFF;
    CHECK(sum / 36 > 115 && sum / 36 < 140);             // 40 of 81 white: about 126
    // a supersampled render is the size asked for and smoother: fewer hard steps along edges
    Renderer r;
    CHECK(r.init(kSmokeWidth, kSmokeHeight));
    GraphicsSettings s;
    s.apply_preset(GraphicsPreset::Low);
    static SmokeScene smoke;
    smoke.build();
    const RenderScene& sc = smoke.scene;
    r.set_settings(s);
    CHECK(render_ok(r, sc));
    auto steps = [&](const uint32_t* px) {
        long n = 0;
        for (int y = 0; y < kSmokeHeight; y++)
            for (int x = 1; x < kSmokeWidth; x++) {
                int a = px[y * kSmokeWidth + x] & 0xFF, b = px[y * kSmokeWidth + x - 1] & 0xFF;
                if ((a > b ? a - b : b - a) > 60) n++;
            }
        return n;
    };
    long hard_off = steps(r.pixels());
    s.ssaa = SSAAMode::X4;
    r.set_settings(s);
    CHECK(render_ok(r, sc) && r.width() == kSmokeWidth && r.height() == kSmokeHeight);
    long hard_4x = steps(r.pixels());
    CHECK(hard_4x < hard_off);
    r.shutdown();
}

// Screen shaders read from memory (RenderScene::read_asset).
const char* g_screen_src[2] = {
    "uniform sampler2D u_screen;\nvarying vec2 uv;\nvoid main() { gl_FragColor = vec4(texture2D(u_screen, uv).rgb, 1.0); }\n",
    "uniform sampler2D u_screen;\nuniform vec4 u_params;\nvarying vec2 uv;\n"
    "void main() { vec3 c = texture2D(u_screen, uv).rgb; gl_FragColor = vec4(mix(c, vec3(1.0, 1.0, 1.0) - c, u_params.x), 1.0); }\n",
};
void* read_screen_src(const char* path, size_t* size, void*) {
    const char* src = !strcmp(path, "copy.fshader") ? g_screen_src[0] : (!strcmp(path, "invert.fshader") ? g_screen_src[1] : nullptr);
    if (!src) return nullptr;
    *size = strlen(src);
    char* m = (char*)malloc(*size + 1);
    memcpy(m, src, *size + 1);
    return m;
}

void test_screen_shaders() {
    Renderer r;
    CHECK(r.init(kSmokeWidth, kSmokeHeight));
    GraphicsSettings s;
    s.apply_preset(GraphicsPreset::Low);
    r.set_settings(s);
    static SmokeScene smoke;
    smoke.build();
    RenderScene sc = smoke.scene;
    sc.read_asset = read_screen_src;
    CHECK(render_ok(r, sc));
    const int n = kSmokeWidth * kSmokeHeight;
    uint32_t* base = (uint32_t*)malloc((size_t)n * 4);
    memcpy(base, r.pixels(), (size_t)n * 4);
    // a copy pass gives the same picture, the right way up
    sc.screen_shaders[0] = "copy.fshader";
    sc.screen_shader_count = 1;
    CHECK(render_ok(r, sc));
    CHECK(r.stats().screen_passes == 1);
    long diff = 0;
    for (int i = 0; i < n; i++)
        for (int c = 0; c < 24; c += 8) {
            int a = (base[i] >> c) & 0xFF, b = (r.pixels()[i] >> c) & 0xFF;
            diff += a > b ? a - b : b - a;
        }
    CHECK(diff / (n * 3) <= 1);
    // invert at amount 1, after the copy (passes run in order)
    sc.screen_shaders[1] = "invert.fshader";
    sc.screen_shader_count = 2;
    sc.screen_params[0] = 1.0f;
    CHECK(render_ok(r, sc));
    CHECK(r.stats().screen_passes == 2);
    int bad = 0;
    for (int i = 0; i < n; i += 37) {
        int a = base[i] & 0xFF, b = r.pixels()[i] & 0xFF;
        if (a + b < 253 || a + b > 257) bad++;
    }
    CHECK(bad == 0);
    // a missing file is skipped with a warning, the frame still draws
    sc.screen_shaders[0] = "missing.fshader";
    sc.screen_shader_count = 1;
    CHECK(render_ok(r, sc));
    const char* w = r.take_warning();
    CHECK(w && strstr(w, "missing.fshader"));
    free(base);
    r.shutdown();
}

// A custom item shader gets u_time (seconds rendered): the foliage sway sample moves the cube between frames
// and stays put while no time passes.
void* read_sway_src(const char* path, size_t* size, void*) {
    if (strcmp(path, "sway.fshader")) return nullptr;
    FILE* f = fopen("assets/samples/foliage/sway.fshader", "rb");
    if (!f) return nullptr;
    char* m = (char*)malloc(8192);
    *size = m ? fread(m, 1, 8191, f) : 0;
    fclose(f);
    if (m) m[*size] = 0;
    return m;
}

void test_item_shader_time() {
    Renderer r;
    CHECK(r.init(kSmokeWidth, kSmokeHeight));
    GraphicsSettings s;
    s.apply_preset(GraphicsPreset::Low);
    r.set_settings(s);
    static SmokeScene smoke;
    smoke.build();
    RenderScene sc = smoke.scene;
    sc.read_asset = read_sway_src;
    smoke.items[1].shader_path = "sway.fshader";
    smoke.items[1].foliage = true;
    sc.delta_time = 0.4f;
    CHECK(render_ok(r, sc));
    const char* err = r.error();
    CHECK(!err || !err[0]);
    const int n = kSmokeWidth * kSmokeHeight;
    uint32_t* a = (uint32_t*)malloc((size_t)n * 4);
    memcpy(a, r.pixels(), (size_t)n * 4);
    sc.delta_time = 0.0f;
    CHECK(render_ok(r, sc));
    long same = 0, moved = 0;
    for (int i = 0; i < n; i++) same += a[i] != r.pixels()[i];
    sc.delta_time = 0.9f;
    CHECK(render_ok(r, sc));
    for (int i = 0; i < n; i++) moved += a[i] != r.pixels()[i];
    CHECK(same == 0);
    CHECK(moved > 20);
    free(a);
    smoke.items[1].shader_path = nullptr;
    smoke.items[1].foliage = false;
    r.shutdown();
}

// The editor on Minimal-OS: draft frames while the camera moves, full ones when it stops, now and then another
// size (adaptive resolution). After the first round nothing may compile again.
void test_no_recompile() {
    Renderer r;
    CHECK(r.init(kSmokeWidth, kSmokeHeight));
    GraphicsSettings s;
    s.apply_preset(GraphicsPreset::High);
    s.allow_gpu_effects_on_cpu = true;
    r.set_settings(s);
    static SmokeScene smoke;
    smoke.build();
    RenderScene sc = smoke.scene;
    int sizes[3][2] = { { kSmokeWidth, kSmokeHeight }, { kSmokeWidth / 2, kSmokeHeight / 2 }, { kSmokeWidth / 4, kSmokeHeight / 4 } };
    int after_warmup = -1;
    for (int f = 0; f < 24; f++) {
        bool draft = (f % 3) != 2;
        int si = draft ? 1 + (f % 2) : 0;
        if (r.width() != sizes[si][0]) CHECK(r.resize(sizes[si][0], sizes[si][1]));
        sc.draft = draft;
        sc.camera.view = Mat4::look_at(Vec3(3.0f + 0.1f * f, 2.0f, 4.0f), Vec3(0, 0, 0), Vec3(0, 1, 0));
        r.set_compile_budget(0);
        CHECK(render_ok(r, sc));
        if (f == 11) after_warmup = 0;
        if (after_warmup >= 0 && f > 11) after_warmup += r.shaders_compiled();
        if (getenv("FE_DEBUG_RECOMPILE")) printf("frame %d draft %d %dx%d: %d compiled\n", f, draft, r.width(), r.height(), r.shaders_compiled());
    }
    CHECK(after_warmup == 0);
    r.shutdown();
}

// ---------------------------------------------------------------- culling

void test_culling() {
    // A wall in front of the camera hides a cube behind it; another cube is behind the camera; one stands
    // to the side in view. Culling leaves out the hidden two and the picture stays the same.
    Renderer r;
    GraphicsSettings g;
    g.apply_preset(GraphicsPreset::Low);
    g.allow_gpu_effects_on_cpu = false;
    CHECK(g.occlusion_culling);
    r.set_settings(g);
    CHECK(r.init(200, 150));
    Vec3 cp[kCubeVertices], cn[kCubeVertices];
    make_cube(cp, cn, 0.5f);
    DrawItem it[4];
    for (int i = 0; i < 4; i++) { it[i].mesh = RenderMesh{ cp, cn, kCubeVertices, 31 }; it[i].color = Vec3(0.6f, 0.5f, 0.4f); }
    it[0].model = Mat4::trs(Vec3(0, 0, 0), Quat(), Vec3(6, 6, 0.3f));         // the wall
    it[1].model = Mat4::translation(Vec3(0.3f, 0.2f, -3.0f));                 // hidden behind it
    it[2].model = Mat4::translation(Vec3(0, 0, 12.0f));                        // behind the camera
    it[3].model = Mat4::translation(Vec3(2.2f, 1.2f, 2.0f));                   // in view, in front of the wall
    FlatScene f(Vec3(0.2f, 0.3f, 0.4f));
    f.scene.camera.proj = Mat4::perspective(radians(60.0f), 200.0f / 150.0f, 0.1f, 100.0f);
    f.scene.camera.view = Mat4::look_at(Vec3(0, 0, 6), Vec3(0, 0, 0), Vec3(0, 1, 0));
    f.scene.camera.position = Vec3(0, 0, 6);
    f.scene.items = it;
    f.scene.item_count = 4;
    CHECK(render_ok(r, f.scene));
    const RendererStats& st = r.stats();
    printf("culling: %d items, %d outside the view, %d occluded, %d occluders, %d draws, %.3f ms\n", st.items_in, st.culled_frustum,
           st.culled_occlusion, st.occluders, st.draws, st.cull_ms);
    CHECK_MSG(st.items_in == 4 && st.culled_frustum == 1, "the cube behind the camera is outside the frustum");
    CHECK_MSG(st.culled_occlusion == 1 && st.occluders >= 1, "the cube behind the wall is occluded");
    CHECK_MSG(st.draws == 2, "two draws");
    uint32_t* on = (uint32_t*)malloc(200 * 150 * 4);
    memcpy(on, r.pixels(), 200 * 150 * 4);
    g.occlusion_culling = false;
    r.set_settings(g);
    CHECK(render_ok(r, f.scene));
    CHECK_MSG(r.stats().culled_occlusion == 0 && r.stats().draws == 3, "off: the hidden cube is drawn");
    CHECK_MSG(memcmp(on, r.pixels(), 200 * 150 * 4) == 0, "the same picture with and without occlusion culling");
    // The hidden cube peeks out past the wall's edge: it is drawn again.
    g.occlusion_culling = true;
    r.set_settings(g);
    it[1].model = Mat4::translation(Vec3(4.5f, 0.2f, -3.0f));
    CHECK(render_ok(r, f.scene));
    CHECK_MSG(r.stats().culled_occlusion == 0, "a cube that sticks out is not culled");
    // An occluder mesh that is given replaces the item's own triangles; a tiny one hides nothing.
    Vec3 tiny[3] = { Vec3(0, 0, 0), Vec3(0.01f, 0, 0), Vec3(0, 0.01f, 0) };
    it[0].mesh.occluder_positions = tiny;
    it[0].mesh.occluder_count = 3;
    it[1].model = Mat4::translation(Vec3(0.3f, 0.2f, -3.0f));
    CHECK(render_ok(r, f.scene));
    CHECK_MSG(r.stats().culled_occlusion == 0, "the occluder mesh is what occludes");
    it[0].mesh.occluder_positions = nullptr;
    it[0].mesh.occluder_count = 0;
    it[0].occluder = false;
    CHECK(render_ok(r, f.scene));
    CHECK_MSG(r.stats().culled_occlusion == 0, "occluder = false");
    it[0].occluder = true;
    // draw_count: only the first triangles of a mesh.
    it[3].mesh.draw_count = 6;
    CHECK(render_ok(r, f.scene));
    CHECK_MSG(r.stats().triangles == 12 + 2, "draw_count limits the triangles");
    it[3].mesh.draw_count = 0;
    // Bounds that are given are used: a box far away puts the item out of view.
    it[3].mesh.has_bounds = true;
    it[3].mesh.bounds_min = Vec3(100, 100, 100);
    it[3].mesh.bounds_max = Vec3(101, 101, 101);
    CHECK(render_ok(r, f.scene));
    CHECK_MSG(r.stats().culled_frustum == 2, "has_bounds is trusted");
    free(on);
}

// ---------------------------------------------------------------- clouds

void test_clouds() {
    // A sun low in the sky and clouds overhead: every quality and layer count compiles (no step down) in the
    // base tier and the GPU tier, the sky looking up gets brighter/whiter where clouds are, and Off removes them.
    Vec3 cp[kCubeVertices], cn[kCubeVertices];
    make_cube(cp, cn, 0.5f);
    DrawItem it;
    it.mesh = RenderMesh{ cp, cn, kCubeVertices, 41 };
    it.model = Mat4::translation(Vec3(0, -3, -6));
    RenderLight sun;
    sun.type = LightType::Directional;
    sun.direction = normalize(Vec3(-0.5f, -0.4f, -0.6f));
    sun.intensity = 2.0f;
    FlatScene f(Vec3(0.2f, 0.3f, 0.4f));
    f.scene.camera.proj = Mat4::perspective(radians(70.0f), 160.0f / 120.0f, 0.1f, 1000.0f);
    f.scene.camera.view = Mat4::look_at(Vec3(0, 2, 0), Vec3(0, 6, -10), Vec3(0, 1, 0));
    f.scene.camera.position = Vec3(0, 2, 0);
    f.scene.items = &it;
    f.scene.item_count = 1;
    f.scene.lights = &sun;
    f.scene.light_count = 1;
    f.scene.clouds_on = true;
    f.scene.clouds.coverage = 0.8f;
    f.scene.clouds.density = 1.2f;
    for (int tier = 0; tier < 2; tier++) {
        Renderer r;
        GraphicsSettings g;
        g.apply_preset(tier ? GraphicsPreset::Ultra : GraphicsPreset::Low);
        g.allow_gpu_effects_on_cpu = tier == 1;
        if (tier) g.feature_level = FeatureLevelMode::GpuTier;
        g.antialiasing = false;
        g.clouds = CloudSetting::Ultra;
        r.set_settings(g);
        CHECK(r.init(160, 120));
        for (int q = 0; q < 3; q++)
            for (int layers = 1; layers <= 3; layers++) {
                f.scene.clouds.quality = (CloudQuality)q;
                f.scene.clouds.layers = layers;
                bool ok = render_ok(r, f.scene);
                CHECK_MSG(ok, "renders with clouds");
                if (r.stats().cloud_quality != q + 1)
                    printf("clouds: tier %d quality %d layers %d drew quality %d (%s)\n", tier, q + 1, layers, r.stats().cloud_quality, r.error());
                CHECK_MSG(r.stats().cloud_quality == q + 1, "every cloud variant compiles");
            }
        // The upper half with and without clouds: they change many pixels, and differently across the sky.
        f.scene.clouds.quality = CloudQuality::Medium;
        f.scene.clouds.layers = 1;
        render_ok(r, f.scene);
        static int with[160 * 50];
        for (int y = 0; y < 50; y++)
            for (int x = 0; x < 160; x++) with[y * 160 + x] = lum(px(r, x, y));
        f.scene.clouds_on = false;
        render_ok(r, f.scene);
        int changed = 0, mn = 255, mx = -255;
        for (int y = 0; y < 50; y++)
            for (int x = 0; x < 160; x++) {
                int dlt = with[y * 160 + x] - lum(px(r, x, y));
                if (dlt > 6 || dlt < -6) changed++;
                if (dlt < mn) mn = dlt;
                if (dlt > mx) mx = dlt;
            }
        printf("clouds: tier %d, %d of 8000 sky pixels changed, by %d .. %d\n", tier, changed, mn, mx);
        CHECK_MSG(changed > 60, "clouds change the sky overhead");
        CHECK_MSG(mx - mn > 10, "lit and shaded parts (shapes, not a flat veil)");
        CHECK_MSG(r.stats().cloud_quality == 0, "no clouds without the object");
        f.scene.clouds_on = true;
        g.clouds = CloudSetting::Low;
        r.set_settings(g);
        f.scene.clouds.quality = CloudQuality::Ultra;
        render_ok(r, f.scene);
        CHECK_MSG(r.stats().cloud_quality == 1, "the setting caps the quality");
        g.clouds = CloudSetting::Off;
        r.set_settings(g);
        render_ok(r, f.scene);
        CHECK_MSG(r.stats().cloud_quality == 0, "Off hides them");
    }
}

int main() {
    test_feature_levels();
    test_tonemaps();
    test_grading_and_grain();
    test_auto_exposure_meter();
    test_auto_exposure_render();
    test_dynamic_resolution();
    test_bloom_and_lens();
    test_cascades();
    test_settings_text();
    test_bloom_modes();
    test_gi_voxels_and_probes();
    test_gi_render();
    test_aa_choices();
    test_smaa();
    test_taa();
    test_gtao_and_ssr();
    test_new_modes_everything();
    test_ssaa_and_vsync();
    test_screen_shaders();
    test_item_shader_time();
    test_no_recompile();
    test_culling();
    test_clouds();
    return finish_tests("gfx_stack_tests");
}
