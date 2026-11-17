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
#include "mof_check.h"

#include <math.h>
#include <string.h>

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
    CHECK(!graphics_set_text(&g, "aa", "taa", err, sizeof err) && strstr(err, "allowed: off, fxaa"));
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

}  // namespace

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
    return finish_tests("gfx_stack_tests");
}
