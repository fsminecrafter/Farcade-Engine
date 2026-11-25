#include "gfx/clouds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe::gfx {

namespace {

constexpr int kN = 32;                  // noise texels per period
constexpr int kSlice = kN + 2;          // with the border
// A vertical strip of 33 slices: slice z at rows z * 34, slice 32 repeats slice 0, so the slice above z is
// always the next one (no wrap in the shader).
constexpr int kAtlasW = kSlice, kAtlasH = kSlice * (kN + 1);
constexpr int kWeather = 128;
constexpr int STEPS_LOW = 20;
constexpr int STEPS_MED = 16;
constexpr int STEPS_ULTRA = 12;







struct Steps { int march, light; bool detail, octave2; };
Steps steps_for(CloudKey k) {
    // Light towards the sun: 0 = estimated from the height in the layer and the local density, else that
    // many samples of the cloud shape (each costs as much as a step along the ray).
    Steps s{ STEPS_LOW, 0, false, false };
    if (k.quality == 2) s = { STEPS_MED, 0, true, false };
    if (k.quality >= 3) s = { STEPS_ULTRA, 1, true, true };
    // The budget is shared by the layers (FGSL unrolls every step into the program).
    int per = s.march / (k.layers ? k.layers : 1);
    s.march = per < 4 ? 4 : per;
    return s;
}

// ---- noise (tileable)

uint32_t hash3(int x, int y, int z, uint32_t seed) {
    uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ (uint32_t)z * 0xcb1ab31fu ^ seed * 0x9e3779b9u;
    h ^= h >> 13; h *= 0x85ebca6bu; h ^= h >> 16; h *= 0xc2b2ae35u; h ^= h >> 16;
    return h;
}
float unit(uint32_t h) { return (float)(h >> 8) * (1.0f / 16777216.0f); }
int wrap(int v, int p) { v %= p; return v < 0 ? v + p : v; }
float fade(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }
float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Gradient noise with `cells` lattice cells per period (p in [0, 1)), about -1..1.
float perlin3(float x, float y, float z, int cells, uint32_t seed) {
    x *= (float)cells; y *= (float)cells; z *= (float)cells;
    int x0 = (int)x, y0 = (int)y, z0 = (int)z;
    float fx = x - (float)x0, fy = y - (float)y0, fz = z - (float)z0;
    float v[8];
    for (int i = 0; i < 8; i++) {
        int dx = i & 1, dy = (i >> 1) & 1, dz = (i >> 2) & 1;
        uint32_t h = hash3(wrap(x0 + dx, cells), wrap(y0 + dy, cells), wrap(z0 + dz, cells), seed);
        // One of 12 edge directions.
        static const float g[12][3] = { { 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 }, { 1, 0, 1 }, { -1, 0, 1 },
                                        { 1, 0, -1 }, { -1, 0, -1 }, { 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 } };
        const float* gg = g[h % 12];
        v[i] = gg[0] * (fx - (float)dx) + gg[1] * (fy - (float)dy) + gg[2] * (fz - (float)dz);
    }
    float u = fade(fx), w = fade(fy), s = fade(fz);
    float a = lerp(lerp(v[0], v[1], u), lerp(v[2], v[3], u), w);
    float b = lerp(lerp(v[4], v[5], u), lerp(v[6], v[7], u), w);
    return lerp(a, b, s);
}

// 1 - distance to the nearest feature point (one per cell), 0..1.
float worley3(float x, float y, float z, int cells, uint32_t seed) {
    x *= (float)cells; y *= (float)cells; z *= (float)cells;
    int x0 = (int)x, y0 = (int)y, z0 = (int)z;
    float best = 9.0f;
    for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int cx = x0 + dx, cy = y0 + dy, cz = z0 + dz;
                uint32_t h = hash3(wrap(cx, cells), wrap(cy, cells), wrap(cz, cells), seed);
                float px = (float)cx + unit(h), py = (float)cy + unit(h * 747796405u + 1u), pz = (float)cz + unit(h * 2891336453u + 7u);
                float ex = px - x, ey = py - y, ez = pz - z;
                float d = ex * ex + ey * ey + ez * ez;
                if (d < best) best = d;
            }
    float d = fsqrt(best);
    return fclamp(1.0f - d, 0.0f, 1.0f);
}

float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
uint32_t pack(float r, float g, float b) {
    return ((uint32_t)(clamp01(r) * 255.0f + 0.5f) << 16) | ((uint32_t)(clamp01(g) * 255.0f + 0.5f) << 8) | (uint32_t)(clamp01(b) * 255.0f + 0.5f);
}

}  // namespace

uint32_t* clouds_noise_atlas(int* w, int* h) {
    uint32_t* vox = (uint32_t*)malloc(sizeof(uint32_t) * kN * kN * kN);
    float* raw = (float*)malloc(sizeof(float) * kN * kN * kN * 3);
    uint32_t* img = (uint32_t*)calloc((size_t)kAtlasW * kAtlasH, sizeof(uint32_t));
    if (!vox || !img || !raw) { free(vox); free(img); free(raw); return nullptr; }
    for (int z = 0; z < kN; z++)
        for (int y = 0; y < kN; y++)
            for (int x = 0; x < kN; x++) {
                float px = ((float)x + 0.5f) / kN, py = ((float)y + 0.5f) / kN, pz = ((float)z + 0.5f) / kN;
                // Shape: billowy Perlin-Worley (low frequencies).
                float pn = perlin3(px, py, pz, 4, 1) * 0.6f + perlin3(px, py, pz, 8, 2) * 0.3f + perlin3(px, py, pz, 16, 3) * 0.15f;
                pn = pn * 0.5f + 0.5f;
                float wl = worley3(px, py, pz, 4, 11) * 0.625f + worley3(px, py, pz, 8, 12) * 0.25f + worley3(px, py, pz, 16, 13) * 0.125f;
                float shape = clamp01((pn - (1.0f - wl)) / (1.0f - (1.0f - wl) + 1e-4f) * 0.5f + wl * 0.5f);
                // Detail: finer Worley.
                float det = worley3(px, py, pz, 8, 21) * 0.625f + worley3(px, py, pz, 16, 22) * 0.25f + worley3(px, py, pz, 32, 23) * 0.125f;
                // A second shape octave.
                float oct = perlin3(px, py, pz, 8, 31) * 0.5f + 0.5f;
                raw[((z * kN + y) * kN + x) * 3 + 0] = shape;
                raw[((z * kN + y) * kN + x) * 3 + 1] = det;
                raw[((z * kN + y) * kN + x) * 3 + 2] = oct;
            }
    // Each channel stretched to the whole 0..1 range (the coverage threshold is then a share of the sky).
    float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
    for (int i = 0; i < kN * kN * kN * 3; i++) { lo[i % 3] = fminf_(lo[i % 3], raw[i]); hi[i % 3] = fmaxf_(hi[i % 3], raw[i]); }
    for (int i = 0; i < kN * kN * kN; i++) {
        float c[3];
        for (int k = 0; k < 3; k++) c[k] = (raw[i * 3 + k] - lo[k]) / fmaxf_(hi[k] - lo[k], 1e-6f);
        vox[i] = pack(c[0], c[1], c[2]);
    }
    free(raw);
    // Slices into the atlas, each with a border that repeats the opposite edge (bilinear filtering wraps).
    for (int z = 0; z <= kN; z++) {
        int oy = z * kSlice, sz = z % kN;
        for (int y = -1; y <= kN; y++)
            for (int x = -1; x <= kN; x++)
                img[(oy + y + 1) * kAtlasW + x + 1] = vox[(sz * kN + wrap(y, kN)) * kN + wrap(x, kN)];
    }
    free(vox);
    *w = kAtlasW;
    *h = kAtlasH;
    return img;
}

uint32_t* clouds_weather_map(int* w, int* h) {
    uint32_t* img = (uint32_t*)malloc(sizeof(uint32_t) * kWeather * kWeather);
    if (!img) return nullptr;
    for (int y = 0; y < kWeather; y++)
        for (int x = 0; x < kWeather; x++) {
            float px = ((float)x + 0.5f) / kWeather, py = ((float)y + 0.5f) / kWeather;
            float a = perlin3(px, py, 0.25f, 4, 41) * 0.55f + perlin3(px, py, 0.25f, 8, 42) * 0.3f + perlin3(px, py, 0.25f, 16, 43) * 0.15f;
            float b = perlin3(px, py, 0.75f, 3, 51) * 0.6f + perlin3(px, py, 0.75f, 6, 52) * 0.4f;
            img[y * kWeather + x] = pack(a * 0.75f + 0.5f, b * 0.75f + 0.5f, 0);
        }
    *w = kWeather;
    *h = kWeather;
    return img;
}

void clouds_frame(const Clouds& c, CloudKey key, const Vec3& camera, float time, const Vec3& sun_dir, const Vec3& sun_color,
                  const Vec3& sky_ambient, uint32_t frame, CloudFrame* out) {
    CloudFrame f;
    f.key = key;
    float thick = fmaxf_(c.thickness, 10.0f), scale = fmaxf_(c.scale, 0.05f);
    float cov = fclamp(c.coverage, 0.0f, 1.0f), dist = fmaxf_(c.distance, 500.0f);
    // cl0: base, thickness, coverage = clamp(weather * z + w)
    f.cl0[0] = c.height; f.cl0[1] = thick; f.cl0[2] = cov * 1.3f; f.cl0[3] = cov * 0.35f;
    // cl1: colour, noise texels per meter (one 32 texel period is 8 km at scale 1)
    f.cl1[0] = c.color.x; f.cl1[1] = c.color.y; f.cl1[2] = c.color.z; f.cl1[3] = 1.0f / (250.0f * scale);
    // cl2: the wind's drift (kept within a few noise periods so the floats stay precise), layer stride
    float period = 32.0f * 250.0f * scale * 4.0f;
    for (int i = 0; i < 3; i++) {
        float v = (&c.wind.x)[i] * time;
        v -= period * (float)(int)(v / period);
        f.cl2[i] = v;
    }
    f.cl2[3] = thick + fmaxf_(c.layer_spacing, 0.0f);
    // cl3: sun glow, fade distance, its fade slope (1.8 / distance: gone by it, full up to 45 %), jitter
    f.cl3[0] = fclamp(c.sun_glow, 0.0f, 1.0f); f.cl3[1] = dist; f.cl3[2] = 1.8f / dist;
    f.cl3[3] = (float)(frame % 64u) * 0.618034f;
    // cpos: camera, coverage map per meter (it repeats every 30 km at scale 1)
    f.cpos[0] = camera.x; f.cpos[1] = camera.y; f.cpos[2] = camera.z; f.cpos[3] = 1.0f / (30000.0f * scale);
    // csky: the ambient light (sky colour * ambient), extinction per meter
    float amb = fclamp(c.ambient, 0.0f, 2.0f);
    f.csky[0] = sky_ambient.x * amb; f.csky[1] = sky_ambient.y * amb; f.csky[2] = sky_ambient.z * amb;
    f.csky[3] = 0.0035f * fclamp(c.density, 0.0f, 3.0f);
    // csun: sun colour, the estimate's slant factor (low sun: longer path through the cloud)
    f.csun[0] = sun_color.x; f.csun[1] = sun_color.y; f.csun[2] = sun_color.z;
    f.csun[3] = 1.0f / fmaxf_(sun_dir.y, 0.25f);
    *out = f;
}

namespace {

// The atlas lookup, written out where it is used: vec3 P (world meters, wind applied) at F texels per
// meter into vec4 R. Two slices of the strip and a lerp.
void noise_code(char* out, size_t cap, const char* P, const char* F, const char* R, const char* in) {
    snprintf(out, cap,
             "%s{\n"
             "%s    vec3 q = fract(%s * (%s * 0.03125)) * 32.0;\n"
             "%s    float z = floor(q.z);\n"
             "%s    vec4 a = q.xyxy * vec4(0.029411765, 0.00089126560, 0.029411765, 0.00089126560) + vec4(0.044117647, 0.0013368984, 0.044117647, 0.031639929);\n"
             "%s    a = a + z * vec4(0.0, 0.030303030, 0.0, 0.030303030);\n"
             "%s    %s = mix(texture(u_cnoise, a.xy), texture(u_cnoise, a.zw), q.z - z);\n"
             "%s}\n",
             in, in, P, F, in, in, in, in, R, in);
}

}  // namespace

size_t clouds_shader(char* out, size_t cap, CloudKey key, int part) {
    if (!key.quality || cap < 64) { if (cap) out[0] = 0; return 0; }
    Steps s = steps_for(key);
    int layers = key.layers < 1 ? 1 : (key.layers > 3 ? 3 : key.layers);
    char* o = out;
    size_t left = cap;
    auto add = [&](const char* text) {
        size_t n = strlen(text);
        if (n >= left) n = left - 1;
        memcpy(o, text, n);
        o += n;
        left -= n;
        *o = 0;
    };
    char num[640], code[1200];
    // FGSL: vec4 SIMD (a whole-vector op is one instruction, a constructor of mixed parts one move per part),
    // every variable a register until its block ends, loops unrolled into at most 2048 instructions. So the
    // state lives in vectors that advance with one add per step, and constants are folded per layer.
    if (part == 0) {
        add("uniform vec4 u_cl0;\n"            // base, thickness, coverage scale and bias
            "uniform vec4 u_cl1;\n"            // colour, noise texels per meter
            "uniform vec4 u_cl2;\n"            // wind offset (m), layer stride
            "uniform vec4 u_cl3;\n"            // sun glow, fade distance, fade slope, jitter
            "uniform vec4 u_cpos;\n"           // camera, coverage map per meter
            "uniform vec4 u_csky;\n"           // ambient light, extinction per meter
            "uniform vec4 u_csun;\n"           // sun colour, slant factor
            "uniform sampler2D u_cnoise;\n"
            "uniform sampler2D u_cweather;\n"
            "float cl_hg(float c, float g) {\n"
            "    float x = 1.0 + g * g - 2.0 * g * c;\n"
            "    return (1.0 - g * g) / (x * sqrt(x));\n"
            "}\n");
        return (size_t)(o - out);
    }
    // part 1: a block in main(): reads `d` and v_ndc, blends over `col`.
    add("    {\n"
        "    vec4 acc = vec4(0.0, 0.0, 0.0, 1.0);\n"                       // rgb light, a transmittance
        "    vec4 lt = vec4(dot(d, u_sun.xyz), 0.0, 0.0, d.y);\n"           // cos to the sun, phase, jitter, ray y
        "    lt.y = mix(cl_hg(lt.x, 0.2), cl_hg(lt.x, 0.75), u_cl3.x) * 1.2;\n"
        "    lt.z = fract(sin(dot(v_ndc, vec2(12.9898, 78.233)) + u_cl3.w) * 43758.5453);\n"
        "    if (abs(lt.w) < 0.0001) lt.w = 0.0001;\n"
        "    lt.w = 1.0 / lt.w;\n");
    for (int k = 0; k < layers; k++) {
        // lay: x the layer's base, y the step length, z extinction over a step, w where the march starts
        // (the stretch inside the slab, cut at the fade distance, jittered).
        snprintf(num, sizeof num,
                 "    {\n"
                 "    vec4 lay = vec4(u_cl0.x + u_cl2.w * %d.0, 0.0, 0.0, 0.0);\n"
                 "    {\n"
                 "    vec2 tt = (vec2(lay.x, lay.x + u_cl0.y) - u_cpos.y) * lt.w;\n"
                 "    tt = vec2(max(min(tt.x, tt.y), 0.0), min(max(tt.x, tt.y), u_cl3.y));\n"
                 "    lay.y = max(tt.y - tt.x, 0.0) * %.8f;\n"
                 "    lay.w = tt.x + lay.y * lt.z;\n"
                 "    }\n", k, 1.0 / (double)s.march);
        add(num);
        // pos: world point with the wind applied (xyz), distance along the ray (w); hk: height in the layer
        // = pos.y * hk.x + hk.y; stp: one step.
        add("    lay.z = u_csky.w * lay.y;\n"
            "    vec4 pos = vec4(u_cpos.xyz + u_cl2.xyz + d * lay.w, lay.w);\n"
            "    vec4 stp = vec4(d * lay.y, lay.y);\n"
            "    vec2 hk = vec2(1.0, -lay.x - u_cl2.y) * (1.0 / u_cl0.y);\n");
        snprintf(num, sizeof num, "    for (int i = 0; i < %d; i++) {\n", s.march);
        add(num);
        add("        vec4 sm = texture(u_cweather, pos.xz * u_cpos.w);\n"     // x coverage, y height, z fade, w density
            "        sm.x = clamp(sm.x * u_cl0.z + u_cl0.w, 0.0, 1.0);\n"
            "        sm.y = pos.y * hk.x + hk.y;\n"
            "        sm.z = clamp(1.8 - pos.w * u_cl3.z, 0.0, 1.0) * clamp(sm.y * 10.0, 0.0, 1.0) * clamp(1.6 - sm.y * 1.6, 0.0, 1.0);\n"
            "        vec4 n = vec4(0.0);\n");
        noise_code(code, sizeof code, "pos.xyz", "u_cl1.w", "n", "        ");
        add(code);
        if (s.octave2) add("        n.r = dot(n.rb, vec2(0.8, 0.2));\n");
        add("        sm.w = clamp((n.r * sm.z + sm.x - 1.0) / max(sm.x, 0.05), 0.0, 1.0);\n"
            "        if (sm.w > 0.002) {\n");
        if (s.detail) {
            noise_code(code, sizeof code, "pos.xyz", "(u_cl1.w * 5.0)", "n", "            ");
            add(code);
            add("            sm.w = clamp(sm.w - n.g * 0.3 * (1.0 - sm.w), 0.0, 1.0);\n");
        }
        // Towards the sun: estimated from the depth in the layer and the local density (thick, dense clouds
        // are dark at the bottom), plus a real sample of the cloud above on Ultra.
        add("            float od = (1.0 - sm.y) * (sm.x * 0.7 + sm.w * 0.9) * u_csun.w * u_cl0.y * u_csky.w * 1.6;\n");
        if (s.light) {
            add("            {\n"
                "            vec3 lp = pos.xyz + u_sun.xyz * (u_cl0.y * 0.25);\n"
                "            float lh = lp.y * hk.x + hk.y;\n");
            noise_code(code, sizeof code, "lp", "u_cl1.w", "n", "            ");
            add(code);
            add("            n.r = n.r * clamp(lh * 10.0, 0.0, 1.0) * clamp(1.6 - lh * 1.6, 0.0, 1.0);\n"
                "            od = od * 0.6 + clamp((n.r + sm.x - 1.0) / max(sm.x, 0.05), 0.0, 1.0) * u_cl0.y * u_csky.w * 0.25;\n"
                "            }\n");
        }
        // Single scattering (Beer-Lambert, powder, phase) plus a soft, isotropic part for the light that
        // scattered many times inside (keeps the sides away from the sun white-grey, not dark).
        add("            sm.z = exp(-od * 0.25) * 0.35;\n"
            "            od = exp(-od);\n"
            "            vec3 lit = u_csun.rgb * (od * (0.45 + 0.55 * clamp(sm.w * 2.5 + (1.0 - od) * 2.0, 0.0, 1.0)) * lt.y + sm.z)\n"
            "                     + u_csky.rgb * (0.2 + 0.8 * sm.y);\n"
            "            od = exp(-sm.w * lay.z);\n"
            "            acc.rgb = acc.rgb + lit * u_cl1.rgb * (acc.a * (1.0 - od));\n"
            "            acc.a = acc.a * od;\n"
            "        }\n"
            "        pos = pos + stp;\n"
            "    }\n"
            "    }\n");
    }
    const char* dbg = getenv("FE_CLOUD_DEBUG");
    if (dbg && dbg[0] == '1') add("    col = texture(u_cnoise, v_ndc * 0.5 + 0.5).rgb;\n    }\n");
    else if (dbg && dbg[0] == '2') add("    col = vec3(1.0 - acc.a);\n    }\n");
    else if (dbg && dbg[0] == '3') add("    col = texture(u_cweather, v_ndc * 0.5 + 0.5).rgb;\n    }\n");
    else add("    col = col * acc.a + acc.rgb;\n"
             "    }\n");
    return (size_t)(o - out);
}

}  // namespace fe::gfx
