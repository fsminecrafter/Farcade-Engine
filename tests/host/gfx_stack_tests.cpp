// Tests of the renderer's upgrade work: feature levels, the post stack
// (tonemaps, auto exposure, grading, dynamic resolution), cascaded shadows and
// the CPU global illumination. The older tests are in gfx_tests.cpp.
#include "gfx/renderer.h"
#include "gfx/feature_level.h"
#include "gfx/meshgen.h"
#include "gfx/post_stack.h"
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

}  // namespace

int main() {
    test_feature_levels();
    test_tonemaps();
    test_grading_and_grain();
    test_auto_exposure_meter();
    test_auto_exposure_render();
    test_dynamic_resolution();
    test_bloom_and_lens();
    return finish_tests("gfx_stack_tests");
}
