#include "gfx/shaders_post.h"
#include "core/math.h"

#include <math.h>
#include <string.h>

namespace fe::gfx::shaders {
namespace {

struct Out {
    char* p;
    size_t cap, n = 0;
    Out(char* o, size_t c) : p(o), cap(c) { if (cap) p[0] = 0; }
    void add(const char* s) {
        size_t l = strlen(s);
        if (n + l + 1 > cap) l = cap > n + 1 ? cap - n - 1 : 0;
        memcpy(p + n, s, l);
        n += l;
        if (cap) p[n] = 0;
    }
    void flt(float v) {
        char b[32];
        // 6 decimals are enough for the matrices below
        int neg = v < 0;
        if (neg) v = -v;
        int whole = (int)v;
        int frac = (int)((v - (float)whole) * 1000000.0f + 0.5f);
        if (frac >= 1000000) { whole++; frac -= 1000000; }
        snprintf_flt(b, sizeof b, neg, whole, frac);
        add(b);
    }
    static void snprintf_flt(char* b, size_t cap, int neg, int whole, int frac) {
        size_t i = 0;
        if (neg && i + 1 < cap) b[i++] = '-';
        char tmp[12];
        int t = 0;
        do { tmp[t++] = (char)('0' + whole % 10); whole /= 10; } while (whole && t < 11);
        while (t && i + 1 < cap) b[i++] = tmp[--t];
        if (i + 1 < cap) b[i++] = '.';
        for (int d = 100000; d >= 1 && i + 1 < cap; d /= 10) b[i++] = (char)('0' + frac / d % 10);
        b[i] = 0;
    }
};

// AgX (Troy Sobotka's look as in three.js / Godot): the sRGB to Rec.2020 matrix
// folded with the inset matrix, a log2 encoding over [-12.47393, 4.026069] EV,
// a 6th degree polynomial for the sigmoid, the outset matrix and a 2.2 power
// (the result is display linear light, to which the chain adds the sRGB curve).
// Matrices as rows of the combined 3x3 (computed once).
struct AgxMats {
    float a[9], b[9];       // a: sRGB -> inset Rec.2020, b: outset Rec.2020 -> sRGB
    AgxMats() {
        // GLSL mat3(vec3 col0, col1, col2) as in three.js, stored here as rows.
        const float s2r[9] = { 0.6274040f, 0.3292820f, 0.0433136f,       // sRGB -> Rec.2020 (rows)
                               0.0690970f, 0.9195400f, 0.0113612f,
                               0.0163916f, 0.0880132f, 0.8955950f };
        const float inset[9] = { 0.856627153315983f, 0.0951212405381588f, 0.0482516061458583f,
                                 0.137318972929847f, 0.761241990602591f, 0.101439036467562f,
                                 0.11189821299995f, 0.0767994186031903f, 0.811302368396859f };
        const float outset[9] = { 1.1271005818144368f, -0.11060664309660323f, -0.016493938717834573f,
                                  -0.1413297634984383f, 1.157823702216272f, -0.016493938717834257f,
                                  -0.14132976349843826f, -0.11060664309660294f, 1.2519364065950405f };
        const float r2s[9] = { 1.6604910f, -0.5876411f, -0.0728499f,
                               -0.1245505f, 1.1328999f, -0.0083494f,
                               -0.0181508f, -0.1005789f, 1.1187297f };
        mul(inset, s2r, a);
        mul(r2s, outset, b);
    }
    static void mul(const float x[9], const float y[9], float o[9]) {
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) o[i * 3 + j] = x[i * 3] * y[j] + x[i * 3 + 1] * y[3 + j] + x[i * 3 + 2] * y[6 + j];
    }
};
const AgxMats& agx_mats() {
    static const AgxMats m;
    return m;
}

constexpr float kAgxMinEv = -12.47393f, kAgxMaxEv = 4.026069f;

float agx_poly(float x) {
    float x2 = x * x, x4 = x2 * x2;
    return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
}

}  // namespace

PostKey post_key_for(const PostProcess& p, bool draft, bool hdr_codec) {
    PostKey k;
    k.tonemap = (uint8_t)p.tonemap;
    if (k.tonemap > (uint8_t)Tonemap::AgX) k.tonemap = (uint8_t)Tonemap::ACES;
    k.draft = draft;
    k.hdr_codec = hdr_codec;
    bool neutral = p.lift.x == 0 && p.lift.y == 0 && p.lift.z == 0 && p.gamma.x == 1 && p.gamma.y == 1 && p.gamma.z == 1 &&
                   p.gain.x == 1 && p.gain.y == 1 && p.gain.z == 1;
    k.grade = !neutral && !draft;
    k.grain = p.film_grain > 0.0f && !draft;
    return k;
}

size_t emit_post_chain(char* out, size_t cap, const PostKey& k) {
    Out t(out, cap);
    t.add("uniform vec4 u_post;\n"
          "uniform vec4 u_tint;\n");
    if (!k.draft) t.add("uniform vec4 u_res;\n");
    if (k.grade) t.add("uniform vec4 u_lift;\nuniform vec4 u_gamma;\nuniform vec4 u_gain;\n");
    if (k.grain) t.add("uniform vec4 u_grain;\n");
    t.add("vec3 lin2srgb(vec3 c) {\n"
          "    return mix(c * 12.92, 1.055 * pow(c, vec3(0.4166667)) - 0.055, step(0.0031308, c));\n"
          "}\n");
    if (k.tonemap == (uint8_t)Tonemap::AgX) {
        const AgxMats& m = agx_mats();
        t.add("vec3 agx(vec3 c) {\n"
              "    vec3 v = vec3(dot(c, vec3(");
        for (int j = 0; j < 3; j++) { t.flt(m.a[j]); t.add(j < 2 ? ", " : ")), dot(c, vec3("); }
        for (int j = 0; j < 3; j++) { t.flt(m.a[3 + j]); t.add(j < 2 ? ", " : ")), dot(c, vec3("); }
        for (int j = 0; j < 3; j++) { t.flt(m.a[6 + j]); t.add(j < 2 ? ", " : "))"); }
        t.add(");\n"
              "    v = clamp((log2(max(v, vec3(0.0000000001))) - (-12.47393)) * 0.0606061, 0.0, 1.0);\n"
              "    v = (((((15.5 * v - 40.14) * v + 31.96) * v - 6.868) * v + 0.4298) * v + 0.1191) * v - 0.00232;\n"
              "    v = pow(max(v, vec3(0.0)), vec3(2.2));\n"
              "    return clamp(vec3(dot(v, vec3(");
        for (int j = 0; j < 3; j++) { t.flt(m.b[j]); t.add(j < 2 ? ", " : ")), dot(v, vec3("); }
        for (int j = 0; j < 3; j++) { t.flt(m.b[3 + j]); t.add(j < 2 ? ", " : ")), dot(v, vec3("); }
        for (int j = 0; j < 3; j++) { t.flt(m.b[6 + j]); t.add(j < 2 ? ", " : "))"); }
        t.add("), 0.0, 1.0);\n}\n");
    }
    t.add("vec3 tone(vec3 c) {\n");
    switch ((Tonemap)k.tonemap) {
        case Tonemap::None: t.add("    vec3 t = c;\n"); break;
        case Tonemap::Reinhard: t.add("    vec3 t = c / (1.0 + c);\n"); break;
        case Tonemap::AgX: t.add("    vec3 t = agx(c);\n"); break;
        default: t.add("    vec3 t = (c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14);\n"); break;
    }
    t.add("    t = clamp(t, 0.0, 1.0);\n");
    if (k.draft) {
        t.add("    vec3 s = sqrt(t);\n"
              "    return 0.8 * s + 0.2 * sqrt(s);\n"
              "}\n");
    } else {
        t.add("    t = clamp((t - 0.18) * u_post.y + 0.18, 0.0, 1.0);\n"
              "    float luma = dot(t, vec3(0.2126, 0.7152, 0.0722));\n"
              "    t = mix(vec3(luma), t, u_post.z);\n");
        if (k.grade)
            t.add("    t = pow(max(t * u_gain.rgb + u_lift.rgb * (1.0 - t), vec3(0.0)), u_gamma.rgb);\n");
        t.add("    vec2 q = (gl_FragCoord.xy + u_res.zw) / u_res.xy - 0.5;\n"
              "    float edge = clamp(length(q) * 1.4142136, 0.0, 1.0);\n"
              "    t = t * (1.0 - u_post.w * smoothstep(0.25, 1.0, edge));\n");
        if (k.grain) {
            t.add("    vec3 s = lin2srgb(clamp(t, 0.0, 1.0));\n"
                  "    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy + u_grain.yz, vec2(0.06711056, 0.00583715)))) - 0.5;\n"
                  "    return clamp(s + n * u_grain.x * (1.0 - 0.6 * dot(s, vec3(0.3333333))), 0.0, 1.0);\n"
                  "}\n");
        } else {
            t.add("    return lin2srgb(clamp(t, 0.0, 1.0));\n"
                  "}\n");
        }
    }
    t.add("vec3 post(vec3 c) {\n"
          "    return tone(c * exp2(u_post.x) * u_tint.rgb);\n"
          "}\n");
    if (k.hdr_codec)
        t.add("vec3 hdr_enc(vec3 c) {\n"
              "    return sqrt(clamp(c * exp2(u_post.x) * u_tint.rgb, 0.0, 8.0) * 0.125);\n"
              "}\n"
              "vec3 hdr_dec(vec3 e) {\n"
              "    return e * e * 8.0;\n"
              "}\n");
    return t.n;
}

// ------------------------------------------------------------------ CPU copies

float tonemap_channel(Tonemap t, float c) {
    if (c < 0) c = 0;
    switch (t) {
        case Tonemap::None: return c > 1 ? 1 : c;
        case Tonemap::Reinhard: return c / (1.0f + c);
        case Tonemap::AgX: {
            float in[3] = { c, c, c }, o[3];
            tonemap_rgb(t, in, o);
            return o[1];
        }
        default: {
            float v = (c * (2.51f * c + 0.03f)) / (c * (2.43f * c + 0.59f) + 0.14f);
            return v < 0 ? 0 : (v > 1 ? 1 : v);
        }
    }
}

void tonemap_rgb(Tonemap t, const float in[3], float out[3]) {
    if (t != Tonemap::AgX) {
        for (int i = 0; i < 3; i++) out[i] = tonemap_channel(t, in[i]);
        return;
    }
    const AgxMats& m = agx_mats();
    float v[3];
    for (int i = 0; i < 3; i++) {
        float s = m.a[i * 3] * in[0] + m.a[i * 3 + 1] * in[1] + m.a[i * 3 + 2] * in[2];
        if (s < 1e-10f) s = 1e-10f;
        float x = (log2f(s) - kAgxMinEv) / (kAgxMaxEv - kAgxMinEv);
        x = x < 0 ? 0 : (x > 1 ? 1 : x);
        float p = agx_poly(x);
        v[i] = powf(p < 0 ? 0 : p, 2.2f);
    }
    for (int i = 0; i < 3; i++) {
        float s = m.b[i * 3] * v[0] + m.b[i * 3 + 1] * v[1] + m.b[i * 3 + 2] * v[2];
        out[i] = s < 0 ? 0 : (s > 1 ? 1 : s);
    }
}

}  // namespace fe::gfx::shaders
