#include "gfx/post_stack.h"
#include "core/math.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace fe::gfx {

PostFrame PostFrame::make(const PostProcess& p, float auto_ev, uint32_t frame, bool draft, bool hdr_codec) {
    PostFrame f;
    f.key = shaders::post_key_for(p, draft, hdr_codec);
    f.exposure_ev = p.exposure + auto_ev;
    f.lift[0] = p.lift.x; f.lift[1] = p.lift.y; f.lift[2] = p.lift.z;
    auto inv = [](float g) { return 1.0f / (g < 0.05f ? 0.05f : g); };
    f.inv_gamma[0] = inv(p.gamma.x); f.inv_gamma[1] = inv(p.gamma.y); f.inv_gamma[2] = inv(p.gamma.z);
    f.gain[0] = p.gain.x; f.gain[1] = p.gain.y; f.gain[2] = p.gain.z;
    f.grain[0] = fclamp(p.film_grain, 0.0f, 1.0f) * 0.12f;
    // a different noise pattern every frame (the offsets stay small so the hash keeps its precision)
    f.grain[1] = (float)((frame * 37u) % 977u);
    f.grain[2] = (float)((frame * 61u) % 983u);
    f.chromatic = fclamp(p.chromatic_aberration, 0.0f, 1.0f);
    f.bloom_radius = fclamp(p.bloom_radius, 0.0f, 1.0f);
    return f;
}

// ------------------------------------------------------------------ auto exposure

namespace {

const float* srgb_lut() {
    static float lut[256];
    static bool done = false;
    if (!done) {
        for (int i = 0; i < 256; i++) {
            float s = (float)i / 255.0f;
            lut[i] = s <= 0.04045f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
        }
        done = true;
    }
    return lut;
}

// The tonemap as a table over log-spaced scene values, to invert by search.
struct InverseTone {
    static constexpr int N = 512;
    int tm = -1;
    float scene[N], display[N];
    void build(Tonemap t) {
        if ((int)t == tm) return;
        tm = (int)t;
        for (int k = 0; k < N; k++) {
            scene[k] = exp2f(-14.0f + 20.0f * (float)k / (float)(N - 1));
            display[k] = shaders::tonemap_channel(t, scene[k]);
        }
    }
    // The scene value whose tonemapped value is d (the lowest such for a flat top).
    float invert(float d) const {
        if (d <= display[0]) return scene[0];
        if (d >= display[N - 1]) return scene[N - 1];
        int lo = 0, hi = N - 1;
        while (hi - lo > 1) {
            int mid = (lo + hi) / 2;
            if (display[mid] < d) lo = mid; else hi = mid;
        }
        float span = display[hi] - display[lo];
        float t = span > 1e-9f ? (d - display[lo]) / span : 0.0f;
        return scene[lo] * powf(scene[hi] / scene[lo], t);
    }
};

}  // namespace

float AutoExposure::meter(const uint32_t* px, int w, int h, bool rgba, Tonemap tm, float applied_ev) {
    if (!px || w < 1 || h < 1) return 0;
    static InverseTone inv;
    inv.build(tm);
    const float* lut = srgb_lut();
    const int gx = w < 64 ? w : 64, gy = h < 36 ? h : 36;
    double sum = 0, wsum = 0;
    for (int j = 0; j < gy; j++) {
        int y = (int)(((float)j + 0.5f) / (float)gy * (float)h);
        float vy = ((float)j + 0.5f) / (float)gy - 0.5f;
        for (int i = 0; i < gx; i++) {
            int x = (int)(((float)i + 0.5f) / (float)gx * (float)w);
            float vx = ((float)i + 0.5f) / (float)gx - 0.5f;
            uint32_t p = px[(size_t)y * (size_t)w + (size_t)x];
            int r = rgba ? (int)(p & 0xFF) : (int)((p >> 16) & 0xFF);
            int g = (int)((p >> 8) & 0xFF);
            int b = rgba ? (int)((p >> 16) & 0xFF) : (int)(p & 0xFF);
            float yd = 0.2126f * lut[r] + 0.7152f * lut[g] + 0.0722f * lut[b];
            float ys = inv.invert(yd);
            float wgt = 1.0f / (1.0f + 3.0f * (vx * vx + vy * vy) * 4.0f);   // 1 at the centre, 0.25 at the corners
            sum += (double)wgt * (double)log2f(ys);
            wsum += wgt;
        }
    }
    return (float)(sum / wsum) - applied_ev;
}

float AutoExposure::target_for(float log2_lum, float min_ev, float max_ev) {
    if (max_ev < min_ev) max_ev = min_ev;
    float t = log2f(kKey) - log2_lum;
    return t < min_ev ? min_ev : (t > max_ev ? max_ev : t);
}

void AutoExposure::step(float target, float speed, float dt) {
    if (!primed) { ev = target; primed = true; return; }
    if (speed < 0.01f) speed = 0.01f;
    if (dt < 0) dt = 0;
    float k = 1.0f - expf(-speed * dt);
    ev += (target - ev) * k;
}

// ------------------------------------------------------------------ dynamic resolution

float DynamicResolution::update(float frame_ms, float target_ms, float min_scale, float max_scale) {
    if (min_scale < 0.25f) min_scale = 0.25f;
    if (max_scale > 1.0f) max_scale = 1.0f;
    if (max_scale < min_scale) max_scale = min_scale;
    if (target_ms < 0.05f) target_ms = 0.05f;
    if (frame_ms > 0.0f) {
        float ratio = sqrtf(target_ms / frame_ms);
        if (frame_ms > target_ms) {
            scale *= ratio < 0.85f ? 0.85f : ratio;
        } else if (frame_ms < target_ms * 0.85f) {
            scale *= ratio > 1.03f ? 1.03f : ratio;
        }
    }
    if (scale < min_scale) scale = min_scale;
    if (scale > max_scale) scale = max_scale;
    return scale;
}

int DynamicResolution::internal_size(int full, float scale) {
    if (scale >= 0.995f || full <= 16) return full;
    int v = (int)(scale * (float)full + 4.0f) / 8 * 8;
    if (v < 16) v = 16;
    return v > full ? full : v;
}

// ------------------------------------------------------------------ SMAA area, TAA jitter

void smaa_area(int cb, int ca, int d1, int d2, float* tb, float* ta) {
    *tb = *ta = 0;
    auto height = [](int c) { return c == 1 ? -0.5f : (c == 2 ? 0.5f : 0.0f); };   // y positive = below the boundary
    float hl = height(cb & 3), hr = height(ca & 3);
    if (hl == 0 && hr == 0) return;                     // an isolated edge: nothing to smooth
    float len = (float)(d1 + d2 + 1);
    auto profile = [&](float pos) {
        float t = pos / len;
        if (hl * hr < 0) return hl + (hr - hl) * t;                                  // Z: one straight line
        float a = 1.0f - 2.0f * t, b = 2.0f * t - 1.0f;                              // U / L: ramps to the middle
        return hl * (a > 0 ? a : 0.0f) + hr * (b > 0 ? b : 0.0f);
    };
    // the average height of the contour over the pixel (two samples are exact for these piecewise linear shapes)
    float y = 0.5f * (profile((float)d1 + 0.25f) + profile((float)d1 + 0.75f));
    if (y > 0) *tb = y; else *ta = -y;
}

void smaa_area_texture(uint32_t out[kSmaaAreaSize * kSmaaAreaSize]) {
    for (int cb = 0; cb < 4; cb++)
        for (int ca = 0; ca < 4; ca++)
            for (int d2 = 0; d2 < 8; d2++)
                for (int d1 = 0; d1 < 8; d1++) {
                    float tb, ta;
                    smaa_area(cb, ca, d1, d2, &tb, &ta);
                    uint32_t r = (uint32_t)(fclamp(tb * 2.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
                    uint32_t g = (uint32_t)(fclamp(ta * 2.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
                    out[(ca * 8 + d2) * kSmaaAreaSize + cb * 8 + d1] = (r << 16) | (g << 8);
                }
}

void taa_jitter(uint32_t frame, float* x, float* y) {
    auto halton = [](uint32_t i, uint32_t base) {
        float f = 1.0f, r = 0.0f;
        while (i > 0) { f /= (float)base; r += f * (float)(i % base); i /= base; }
        return r;
    };
    uint32_t i = frame % 8u + 1u;
    *x = halton(i, 2) - 0.5f;
    *y = halton(i, 3) - 0.5f;
}

// ------------------------------------------------------------------ upscale

void upscale_xrgb_to_rgba(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw, int dh) {
    auto swz = [](uint32_t p) { return 0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu); };
    if (sw == dw && sh == dh) {
        uint32_t n = (uint32_t)dw * (uint32_t)dh;
        for (uint32_t i = 0; i < n; i++) dst[i] = swz(src[i]);
        return;
    }
    // Bilinear, 8 bit weights; the horizontal taps are the same on every row.
    struct Tap { int x0, x1; uint32_t w; };
    Tap* cache = (Tap*)malloc(sizeof(Tap) * (size_t)dw);
    if (!cache) return;
    for (int x = 0; x < dw; x++) {
        float fx = ((float)x + 0.5f) * (float)sw / (float)dw - 0.5f;
        if (fx < 0) fx = 0;
        int x0 = (int)fx;
        if (x0 > sw - 1) x0 = sw - 1;
        int x1 = x0 + 1 < sw ? x0 + 1 : x0;
        cache[x].x0 = x0;
        cache[x].x1 = x1;
        cache[x].w = (uint32_t)((fx - (float)x0) * 256.0f);
    }
    for (int y = 0; y < dh; y++) {
        float fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f;
        if (fy < 0) fy = 0;
        int y0 = (int)fy;
        if (y0 > sh - 1) y0 = sh - 1;
        int y1 = y0 + 1 < sh ? y0 + 1 : y0;
        uint32_t wy = (uint32_t)((fy - (float)y0) * 256.0f);
        const uint32_t* r0 = src + (size_t)y0 * (size_t)sw;
        const uint32_t* r1 = src + (size_t)y1 * (size_t)sw;
        uint32_t* o = dst + (size_t)y * (size_t)dw;
        for (int x = 0; x < dw; x++) {
            const Tap& t = cache[x];
            uint32_t a = r0[t.x0], b = r0[t.x1], c = r1[t.x0], d = r1[t.x1];
            uint32_t wx = t.w, ix = 256 - wx, iy = 256 - wy;
            // rb and g separately: two multiplies per pair
            uint32_t rb0 = ((a & 0xFF00FFu) * ix + (b & 0xFF00FFu) * wx) >> 8;
            uint32_t rb1 = ((c & 0xFF00FFu) * ix + (d & 0xFF00FFu) * wx) >> 8;
            uint32_t g0 = (((a >> 8) & 0xFFu) * ix + ((b >> 8) & 0xFFu) * wx) >> 8;
            uint32_t g1 = (((c >> 8) & 0xFFu) * ix + ((d >> 8) & 0xFFu) * wx) >> 8;
            uint32_t rb = ((rb0 & 0xFF00FFu) * iy + (rb1 & 0xFF00FFu) * wy) >> 8;
            uint32_t g = (g0 * iy + g1 * wy) >> 8;
            uint32_t p = (rb & 0xFF00FFu) | ((g & 0xFFu) << 8);
            o[x] = swz(p);
        }
    }
    free(cache);
}

}  // namespace fe::gfx
