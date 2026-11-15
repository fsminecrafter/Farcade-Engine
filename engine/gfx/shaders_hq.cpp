#include "gfx/shaders_hq.h"
#include "core/math.h"

#include <string.h>

namespace fe::gfx::shaders {
namespace {

// A bounded text buffer: appends never overflow, they truncate.
struct Text {
    char* p;
    size_t cap, n = 0;
    Text(char* out, size_t c) : p(out), cap(c) { if (cap) p[0] = 0; }
    void add(const char* s) {
        size_t l = strlen(s);
        if (n + l + 1 > cap) l = cap > n + 1 ? cap - n - 1 : 0;
        memcpy(p + n, s, l);
        n += l;
        if (cap) p[n] = 0;
    }
    void num(int v) {
        char b[16];
        int i = 15;
        b[i] = 0;
        unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;
        do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
        if (v < 0) b[--i] = '-';
        add(b + i);
    }
    // A float literal with 5 decimals.
    void flt(float v) {
        if (v < 0) { add("-"); v = -v; }
        int whole = (int)v;
        int frac = (int)((v - (float)whole) * 100000.0f + 0.5f);
        if (frac >= 100000) { whole++; frac -= 100000; }
        num(whole);
        add(".");
        char b[6] = { (char)('0' + frac / 10000 % 10), (char)('0' + frac / 1000 % 10), (char)('0' + frac / 100 % 10),
                      (char)('0' + frac / 10 % 10), (char)('0' + frac % 10), 0 };
        add(b);
    }
};

// Post processing shared by the final-color shaders. `tone` is everything after
// exposure and tint (tonemap, contrast, saturation, vignette, linear to sRGB),
// `post` is the base tier's chain, `hdr_enc` / `hdr_dec` the 8 bit HDR codec
// (exposure and tint are applied when encoding).
const char* const kTone =
    "uniform vec4 u_post;\n"
    "uniform vec4 u_tint;\n"
    "uniform vec4 u_res;\n"
    "vec3 lin2srgb(vec3 c) {\n"
    "    return mix(c * 12.92, 1.055 * pow(c, vec3(0.4166667)) - 0.055, step(0.0031308, c));\n"
    "}\n"
    "vec3 tone(vec3 c) {\n"
    "    vec3 t = c;\n"
    "    if (u_tint.w > 1.5) t = (c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14);\n"
    "    else if (u_tint.w > 0.5) t = c / (1.0 + c);\n"
    "    t = clamp(t, 0.0, 1.0);\n"
    "    t = clamp((t - 0.18) * u_post.y + 0.18, 0.0, 1.0);\n"
    "    float luma = dot(t, vec3(0.2126, 0.7152, 0.0722));\n"
    "    t = mix(vec3(luma), t, u_post.z);\n"
    "    vec2 q = gl_FragCoord.xy / u_res.xy - 0.5;\n"
    "    float edge = clamp(length(q) * 1.4142136, 0.0, 1.0);\n"
    "    t = t * (1.0 - u_post.w * smoothstep(0.25, 1.0, edge));\n"
    "    return lin2srgb(clamp(t, 0.0, 1.0));\n"
    "}\n"
    "vec3 post(vec3 c) {\n"
    "    return tone(c * exp2(u_post.x) * u_tint.rgb);\n"
    "}\n"
    "vec3 hdr_enc(vec3 c) {\n"
    "    return sqrt(clamp(c * exp2(u_post.x) * u_tint.rgb, 0.0, 8.0) * 0.125);\n"
    "}\n"
    "vec3 hdr_dec(vec3 e) {\n"
    "    return e * e * 8.0;\n"
    "}\n";

const char* const kSrgb2Lin =
    "vec3 srgb2lin(vec3 c) {\n"
    "    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));\n"
    "}\n";

// 24 bit fixed point depth in r:g:b.
const char* const kPack =
    "vec3 pack24(float d) {\n"
    "    float v = floor(clamp(d, 0.0, 1.0) * 16777215.0 + 0.5);\n"
    "    float r = floor(v / 65536.0);\n"
    "    float rem = v - r * 65536.0;\n"
    "    float g = floor(rem / 256.0);\n"
    "    float b = rem - g * 256.0;\n"
    "    return vec3(r, g, b) * 0.003921569;\n"
    "}\n"
    "float unpack24(vec3 c) {\n"
    "    return dot(c, vec3(16711680.0, 65280.0, 255.0)) * 5.9604645e-8;\n"
    "}\n";

// (Blocks free their registers: FGSL has 32 vec4 temporaries in all.)
const char* const kPointHead =
    "    vec3 L = vec3(0.0);\n"
    "    float att = 0.0;\n"
    "    {\n"
    "        vec3 d = u_l0[i].xyz - v_pos;\n"
    "        float dist = length(d);\n"
    "        L = d / max(dist, 0.0001);\n"
    "        float x = min(dist / max(u_l0[i].w, 0.0001), 1.0);\n"
    "        att = 1.0 - x * x;\n"
    "        att = att * att;\n"
    "    }\n";

// Blinn-Phong, as the base tier, with S the shadow factor.
const char* const kBlinnTail =
    "    float ndl = max(dot(N, L), 0.0);\n"
    "    vec3 H = normalize(L + V);\n"
    "    float sp = pow(max(dot(N, H), 0.0), shin) * ndl * att * S;\n"
    "    diff += u_l2[i].rgb * (ndl * att * S);\n"
    "    spec += u_l2[i].rgb * (sp * specn);\n"
    "}\n";

// GGX / Smith-Schlick / Schlick Fresnel; diffuse is Lambert scaled by (1 - F).
// Light intensities keep the base tier's meaning (Lambert is albedo * light,
// without 1/pi), so the specular lobe is scaled by pi to match.
const char* const kPbrTail =
    "    float ndl = max(dot(N, L), 0.0);\n"
    "    vec3 H = normalize(L + V);\n"
    "    float vdh = max(dot(V, H), 0.0);\n"
    "    float dv = 0.0;\n"
    "    {\n"
    "        float ndh = max(dot(N, H), 0.0);\n"
    "        float dd = ndh * ndh * (M4.x - 1.0) + 1.0;\n"
    "        float gg = (ndl * (1.0 - M4.y) + M4.y) * (M4.z * (1.0 - M4.y) + M4.y);\n"
    "        dv = 0.25 * M4.x / (3.14159265 * dd * dd * gg);\n"
    "    }\n"
    "    float fr = 1.0 - vdh;\n"
    "    fr = fr * fr * fr * fr * fr;\n"
    "    vec3 F = F0 + (1.0 - F0) * fr;\n"
    "    vec3 rad = u_l2[i].rgb * (att * S);\n"
    "    diff += rad * ndl * (1.0 - F);\n"
    "    spec += rad * F * (dv * ndl * 3.14159265);\n"
    "}\n";

const char* const kComp[4] = { "x", "y", "z", "w" };

// The atlas lookup of one point / spot shadow, written out (a function call
// would copy its arguments into new registers). In: d (light to surface),
// a = (right, up, forward) coordinates of d in the light camera, p = (first
// tile, tan of half the fov, relative bias, 1 / range). Out: LS.<comp>.
void emit_atlas_lookup(Text& t, const char* comp, bool soft) {
    t.add("        vec2 uv = clamp(vec2(0.5, 0.5) + vec2(a.x, -a.y) * (0.5 / (max(a.z, 0.0001) * p.y)), u_la.z, 1.0 - u_la.z);\n"
          "        p.y = floor((p.x + 0.5) * u_la.y);\n"            // the tile's row
          "        uv = (vec2(p.x - p.y * u_la.x, p.y) + uv) * u_la.y;\n"
          "        p.w = length(d) * p.w * (1.0 - p.z);\n");         // the reference depth
    if (soft) {
        t.add("        p.y = u_la.z * u_la.y * 0.6667;\n"
              "        p.z = atlas_tap(uv - vec2(p.y, p.y), p.w);\n"
              "        p.z = p.z + atlas_tap(uv + vec2(p.y, -p.y), p.w);\n"
              "        p.z = p.z + atlas_tap(uv + vec2(-p.y, p.y), p.w);\n"
              "        p.z = p.z + atlas_tap(uv + vec2(p.y, p.y), p.w);\n"
              "        LS."); t.add(comp); t.add(" = p.z * 0.25;\n");
    } else {
        t.add("        LS."); t.add(comp); t.add(" = atlas_tap(uv, p.w);\n");
    }
}

// Vogel disk offsets of the soft shadow filter, rotated per pixel.
constexpr int kShadowTaps = 10;

}  // namespace

void hq_lit_vertex(char* out, size_t cap, bool shadow) {
    Text t(out, cap);
    t.add("uniform mat4 fgl_ModelViewProjection;\n"
          "uniform mat4 fgl_Model;\n"
          "uniform mat4 fgl_NormalMatrix;\n"
          "uniform vec4 u_mat;\n"
          "attribute vec3 position;\n"
          "attribute vec3 normal;\n"
          "varying vec3 v_pos;\n"
          "varying vec3 v_nrm;\n"
          "varying vec3 v_alb;\n");
    if (shadow) t.add("uniform mat4 u_shm;\nuniform vec4 u_sdir;\nuniform vec4 u_shq;\nvarying vec3 v_sh;\n");
    t.add(kSrgb2Lin);
    t.add("void main() {\n"
          "    v_pos = (fgl_Model * vec4(position, 1.0)).xyz;\n"
          "    v_nrm = (fgl_NormalMatrix * vec4(normal, 0.0)).xyz;\n"
          "    v_alb = srgb2lin(u_mat.rgb);\n");
    if (shadow) {
        // The receiver is moved along its normal by up to a few shadow texels where the
        // surface turns away from the light (normal offset bias).
        t.add("    vec3 wn = normalize(v_nrm);\n"
              "    float sdl = clamp(dot(wn, u_sdir.xyz), 0.0, 1.0);\n"
              "    v_sh = (u_shm * vec4(v_pos + wn * (u_shq.y * (1.0 - sdl)), 1.0)).xyz;\n");
    }
    t.add("    gl_Position = fgl_ModelViewProjection * vec4(position, 1.0);\n"
          "}\n");
}

void hq_lit_fragment(char* out, size_t cap, const HqLitKey& k) {
    Text t(out, cap);
    int total = k.nd + k.np + k.ns;
    t.add("uniform vec4 u_mat;\n"
          "uniform vec4 u_pbr;\n"
          "uniform vec4 u_cam;\n"
          "uniform vec4 u_sky;\n"
          "uniform vec4 u_gnd;\n"
          "uniform vec4 u_fog;\n");
    if (total > 0) {
        // the spot cone is in u_l1.w (cos outer) and u_l2.w (cos inner): no u_l3
        const char* names[3] = { "u_l0", "u_l1", "u_l2" };
        for (int i = 0; i < 3; i++) {
            t.add("uniform vec4 "); t.add(names[i]); t.add("["); t.num(total); t.add("];\n");
        }
    }
    t.add("varying vec3 v_pos;\n"
          "varying vec3 v_nrm;\n"
          "varying vec3 v_alb;\n");
    if (k.shadow) {
        t.add("uniform sampler2D u_shadow;\n"
              "uniform vec4 u_shp;\n"            // 1 / size, meters per uv, 1 / depth range, constant bias
              "uniform vec4 u_shq;\n"            // filter radius (uv), normal offset (m)
              "uniform vec4 u_lr;\n"             // light view right axis (world)
              "uniform vec4 u_lu;\n"             // light view up axis (world)
              "varying vec3 v_sh;\n");
    }
    if (k.ao) t.add("uniform sampler2D u_ao;\n");
    const bool local = k.nps + k.nss > 0;
    if (local) {
        t.add("uniform sampler2D u_atlas;\n"
              "uniform vec4 u_la;\n");          // x tiles per row, y 1 / that, z tile inset (uv)
        // per shadowed light (points, then spots): x first tile, y tan(half fov), z bias, w normal offset per meter
        t.add("uniform vec4 u_lsh["); t.num(k.nps + k.nss); t.add("];\n");
    }
    t.add(kTone);
    t.add(kPack);
    if (local) {
        // d: from the light to the (offset) surface point; s, u, f: the light
        // camera's right, up and forward; tn: tan of half its field of view.
        // Depth is the distance to the light / range.
        t.add("float atlas_tap(vec2 uv, float dref) {\n"
              "    return step(dref, unpack24(texture(u_atlas, uv).rgb));\n"
              "}\n"
              );
    }
    if (k.shadow) {
        t.add("float shadow_tap(vec2 uv, float dref) {\n"
              "    return step(dref - u_shp.w, unpack24(texture(u_shadow, uv).rgb));\n"
              "}\n"
              "float shadow_factor(vec3 N, vec3 sdir) {\n"
              "    vec2 uv = vec2(v_sh.x * 0.5 + 0.5, 0.5 - v_sh.y * 0.5);\n"
              "    float d0 = v_sh.z * 0.5 + 0.5;\n"
              "    float nz = max(dot(N, sdir), 0.2);\n"
              "    vec2 g = vec2(dot(N, u_lr.xyz), dot(N, u_lu.xyz)) * (u_shp.y * u_shp.z / nz);\n"
              "    float inside = step(0.0, uv.x) * step(uv.x, 1.0) * step(0.0, uv.y) * step(uv.y, 1.0) * step(d0, 1.0);\n");
        if (k.shadow >= 2) {
            t.add("    float ang = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
                  "    vec2 cs = vec2(cos(ang), sin(ang));\n"
                  "    float sum = 0.0;\n");
            for (int i = 0; i < kShadowTaps; i++) {
                // Vogel disk: radius sqrt((i + 0.5) / n), golden angle steps.
                float r = fsqrt(((float)i + 0.5f) / (float)kShadowTaps);
                float an = (float)i * 2.39996323f;
                float ox = fcos(an) * r, oy = fsin(an) * r;
                t.add("    {\n        vec2 o = vec2(cs.x * ");
                t.flt(ox); t.add(" - cs.y * "); t.flt(oy); t.add(", cs.y * "); t.flt(ox); t.add(" + cs.x * "); t.flt(oy);
                t.add(") * u_shq.x;\n"
                      "        sum += shadow_tap(uv + o, d0 + g.x * o.x - g.y * o.y);\n    }\n");
            }
            t.add("    float lit = sum * ");
            t.flt(1.0f / (float)kShadowTaps);
            t.add(";\n");
        } else {
            t.add("    float lit = shadow_tap(uv, d0);\n");
        }
        t.add("    return mix(1.0, lit, inside);\n"
              "}\n");
    }
    t.add("void main() {\n"
          "    vec3 col = vec3(0.0);\n"
          "    {\n"                                  // the shading in a block: its registers are free for post()
          "    vec3 N = normalize(v_nrm);\n");
    // the shadow first: it needs many registers, main has few live values yet
    if (k.shadow && k.nd > 0) {
        // In a block: an inlined function's temporaries live until the block ends.
        t.add("    float SH = 1.0;\n    {\n        SH = shadow_factor(N, -u_l1[");
        t.num(k.shadow_light);
        t.add("].xyz);\n    }\n");
    }
    // point / spot shadows too, all four in one register
    if (local) {
        t.add("    vec4 LS = vec4(1.0);\n");
        for (int j = 0; j < k.nps; j++) {
            int i = k.nd + j;
            t.add("    {\n"
                  "        vec3 d = v_pos - u_l0["); t.num(i); t.add("].xyz;\n"
                  "        d = d + N * (u_lsh["); t.num(j); t.add("].w * length(d));\n"
                  "        vec3 a = abs(d);\n"
                  "        vec3 m = vec3(step(a.y, a.x) * step(a.z, a.x), 0.0, 0.0);\n"
                  "        m.y = (1.0 - m.x) * step(a.z, a.y);\n"
                  "        m.z = 1.0 - m.x - m.y;\n"
                  "        a = step(d, vec3(0.0));\n"            // 1 where d points the negative way
                  "        vec3 f = m * (1.0 - 2.0 * a);\n"
                  "        vec4 p = vec4(u_lsh["); t.num(j); t.add("].x + dot(m, a + vec3(0.0, 2.0, 4.0)), u_lsh["); t.num(j);
            t.add("].y, u_lsh["); t.num(j); t.add("].z, 1.0 / u_l0["); t.num(i); t.add("].w);\n"
                  "        m = cross(f, vec3(0.0, 1.0 - m.y, m.y));\n"   // right
                  "        a = vec3(dot(d, m), dot(d, cross(m, f)), dot(d, f));\n");
            emit_atlas_lookup(t, kComp[j], k.local_soft);
            t.add("    }\n");
        }
        for (int j = 0; j < k.nss; j++) {
            int i = k.nd + k.np + j, sj = k.nps + j;
            // the spot's camera from its direction, as atlas_pass builds it
            t.add("    {\n"
                  "        vec3 d = v_pos - u_l0["); t.num(i); t.add("].xyz;\n"
                  "        d = d + N * (u_lsh["); t.num(sj); t.add("].w * length(d));\n"
                  "        vec3 f = u_l1["); t.num(i); t.add("].xyz;\n"
                  "        vec3 m = normalize(cross(f, vec3(0.0, 1.0 - step(0.99, abs(f.y)), step(0.99, abs(f.y)))));\n"
                  "        vec3 a = vec3(dot(d, m), dot(d, cross(m, f)), dot(d, f));\n"
                  "        vec4 p = vec4(u_lsh["); t.num(sj); t.add("].xyz, 1.0 / u_l0["); t.num(i); t.add("].w);\n");
            emit_atlas_lookup(t, kComp[sj], k.local_soft);
            t.add("    }\n");
        }
    }
    t.add("    vec3 V = normalize(u_cam.xyz - v_pos);\n"
          "    float rough = clamp(u_mat.w, 0.045, 1.0);\n"
          "    vec3 diff = vec3(0.0);\n"
          "    vec3 spec = vec3(0.0);\n");
    if (k.pbr) {
        // M4: x a^2 (GGX), y k (Smith-Schlick), z n.v, w roughness
        t.add("    vec3 F0 = vec3(0.0);\n"
              "    vec3 dalb = vec3(0.0);\n"
              "    {\n"
              "        float metal = clamp(u_pbr.x, 0.0, 1.0);\n"
              "        F0 = mix(vec3(0.04), v_alb, metal);\n"
              "        dalb = v_alb * (1.0 - metal);\n"
              "    }\n"
              "    vec4 M4 = vec4(rough * rough * rough * rough, (rough + 1.0) * (rough + 1.0) * 0.125, max(dot(N, V), 0.0001), rough);\n");
    } else {
        t.add("    float a2 = rough * rough * rough * rough;\n"
              "    float shin = clamp(2.0 / a2 - 2.0, 2.0, 512.0);\n"
              "    float specn = (shin + 8.0) * 0.03978874 * (0.04 + 0.3 * (1.0 - rough) * (1.0 - rough));\n");
    }
    const char* tail = k.pbr ? kPbrTail : kBlinnTail;
    if (k.nd > 0) {
        t.add("    for (int i = 0; i < "); t.num(k.nd); t.add("; i++) {\n"
              "    vec3 L = -u_l1[i].xyz;\n"
              "    float att = 1.0;\n");
        if (k.shadow) {
            t.add("    float S = 1.0;\n    if (i == "); t.num(k.shadow_light); t.add(") S = SH;\n");
        } else {
            t.add("    float S = 1.0;\n");
        }
        t.add(tail);
    }
    // Shadowed point lights: one-pass loops (so i is a constant index). The
    // cube face is picked with arithmetic (the shader machine has no branches)
    // and its camera built as Mat4::look_at does (see cube_face).
    for (int j = 0; j < k.nps; j++) {
        int i = k.nd + j;
        t.add("    for (int i = "); t.num(i); t.add("; i < "); t.num(i + 1); t.add("; i++) {\n");
        t.add(kPointHead);
        t.add("    float S = LS."); t.add(kComp[j]); t.add(";\n");
        t.add(tail);
    }
    if (k.np > k.nps) {
        t.add("    for (int i = "); t.num(k.nd + k.nps); t.add("; i < "); t.num(k.nd + k.np); t.add("; i++) {\n");
        t.add(kPointHead);
        t.add("    float S = 1.0;\n");
        t.add(tail);
    }
    for (int j = 0; j < k.nss; j++) {
        int i = k.nd + k.np + j;
        t.add("    for (int i = "); t.num(i); t.add("; i < "); t.num(i + 1); t.add("; i++) {\n");
        t.add(kPointHead);
        t.add("    att = att * smoothstep(u_l1[i].w, u_l2[i].w, dot(-L, u_l1[i].xyz));\n"
              "    float S = LS."); t.add(kComp[k.nps + j]); t.add(";\n");
        t.add(tail);
    }
    if (k.ns > k.nss) {
        t.add("    for (int i = "); t.num(k.nd + k.np + k.nss); t.add("; i < "); t.num(total); t.add("; i++) {\n");
        t.add(kPointHead);
        t.add("    att = att * smoothstep(u_l1[i].w, u_l2[i].w, dot(-L, u_l1[i].xyz));\n"
              "    float S = 1.0;\n");
        t.add(tail);
    }
    if (k.ao) {
        t.add("    float ao = texture(u_ao, gl_FragCoord.xy / u_res.xy).r;\n"
              "    float aod = 1.0 - (1.0 - ao) * 0.4;\n");
    } else {
        t.add("    float ao = 1.0;\n    float aod = 1.0;\n");
    }
    if (k.pbr) {
        t.add("    vec3 hemiN = mix(u_gnd.rgb, u_sky.rgb, N.y * 0.5 + 0.5);\n"
              "    vec3 R = reflect(-V, N);\n"
              "    vec3 hemiR = mix(u_gnd.rgb, u_sky.rgb, R.y * 0.5 + 0.5);\n"
              "    vec3 env = mix(hemiR, (u_gnd.rgb + u_sky.rgb) * 0.5, rough * rough);\n"
              "    vec4 rr = rough * vec4(-1.0, -0.0275, -0.572, 0.022) + vec4(1.0, 0.0425, 1.04, -0.04);\n"
              "    float a004 = min(rr.x * rr.x, exp2(-9.28 * M4.z)) * rr.x + rr.y;\n"
              "    vec2 AB = vec2(-1.04, 1.04) * a004 + rr.zw;\n"
              "    vec3 ks = F0 * AB.x + AB.y;\n"
              "    vec3 ambient = dalb * hemiN * (1.0 - ks) + env * ks;\n"
              "    col = ambient * ao + (dalb * diff + spec) * aod;\n");
    } else {
        t.add("    vec3 amb = mix(u_gnd.rgb, u_sky.rgb, N.y * 0.5 + 0.5);\n"
              "    col = v_alb * (amb * ao + diff * aod) + spec * aod;\n");
    }
    t.add("    }\n");
    if (k.fog) {
        t.add("    col = mix(u_fog.rgb, col, exp(-u_fog.w * length(u_cam.xyz - v_pos)));\n");
    }
    t.add(k.hdr ? "    gl_FragColor = vec4(hdr_enc(col), 1.0);\n}\n" : "    gl_FragColor = vec4(post(col), 1.0);\n}\n");
}

void hq_unlit_vertex(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform mat4 fgl_ModelViewProjection;\n"
          "uniform mat4 fgl_Model;\n"
          "uniform vec4 u_mat;\n"
          "attribute vec3 position;\n"
          "varying vec3 v_pos;\n"
          "varying vec3 v_alb;\n");
    t.add(kSrgb2Lin);
    t.add("void main() {\n"
          "    v_pos = (fgl_Model * vec4(position, 1.0)).xyz;\n"
          "    v_alb = srgb2lin(u_mat.rgb);\n"
          "    gl_Position = fgl_ModelViewProjection * vec4(position, 1.0);\n"
          "}\n");
}

void hq_unlit_fragment(char* out, size_t cap, bool fog, bool hdr) {
    Text t(out, cap);
    t.add("uniform vec4 u_cam;\n"
          "uniform vec4 u_fog;\n"
          "varying vec3 v_pos;\n"
          "varying vec3 v_alb;\n");
    t.add(kTone);
    t.add("void main() {\n"
          "    vec3 col = v_alb;\n");
    if (fog) t.add("    col = mix(u_fog.rgb, col, exp(-u_fog.w * length(u_cam.xyz - v_pos)));\n");
    t.add(hdr ? "    gl_FragColor = vec4(hdr_enc(col), 1.0);\n}\n" : "    gl_FragColor = vec4(post(col), 1.0);\n}\n");
}

void hq_sky_fragment(char* out, size_t cap, bool hdr) {
    Text t(out, cap);
    t.add("uniform vec4 u_fwd;\n"
          "uniform vec4 u_rx;\n"
          "uniform vec4 u_ry;\n"
          "uniform vec4 u_zen;\n"
          "uniform vec4 u_hor;\n"
          "uniform vec4 u_grd;\n"
          "uniform vec4 u_sun;\n"
          "uniform vec4 u_suncol;\n"
          "varying vec2 v_ndc;\n");
    t.add(kTone);
    t.add("void main() {\n"
          "    vec3 d = normalize(u_fwd.xyz + u_rx.xyz * v_ndc.x + u_ry.xyz * v_ndc.y);\n"
          "    float up = clamp(d.y, 0.0, 1.0);\n"
          "    vec3 above = mix(u_hor.rgb, u_zen.rgb, sqrt(up));\n"
          "    vec3 col = mix(above, u_grd.rgb, smoothstep(0.0, 0.12, -d.y));\n"
          "    col = mix(col, u_zen.rgb, u_zen.w);\n"
          "    float c = dot(d, u_sun.xyz);\n"
          "    float core = u_sun.w;\n"
          "    float rim = 1.0 - (1.0 - core) * 1.6;\n"
          "    float disc = smoothstep(rim, core, c) * step(core, 1.0);\n"
          "    float halo = pow(max(c, 0.0), 256.0) * 0.15 * step(core, 1.0);\n"
          "    col += u_suncol.rgb * (disc + halo) * (1.0 - u_zen.w);\n");
    t.add(hdr ? "    gl_FragColor = vec4(hdr_enc(col), 1.0);\n}\n" : "    gl_FragColor = vec4(post(col), 1.0);\n}\n");
}

// ------------------------------------------------------------------ depth-only passes

void hq_shadow_vertex(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform mat4 fgl_ModelViewProjection;\n"
          "attribute vec3 position;\n"
          "varying float v_d;\n"
          "void main() {\n"
          "    vec4 p = fgl_ModelViewProjection * vec4(position, 1.0);\n"
          "    v_d = p.z * 0.5 + 0.5;\n"
          "    gl_Position = p;\n"
          "}\n");
}

void hq_shadow_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("varying float v_d;\n");
    t.add(kPack);
    t.add("void main() {\n"
          "    gl_FragColor = vec4(pack24(v_d), 1.0);\n"
          "}\n");
}

void hq_local_shadow_vertex(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform mat4 fgl_ModelViewProjection;\n"
          "uniform mat4 fgl_Model;\n"
          "attribute vec3 position;\n"
          "varying vec3 v_p;\n"
          "void main() {\n"
          "    v_p = (fgl_Model * vec4(position, 1.0)).xyz;\n"
          "    gl_Position = fgl_ModelViewProjection * vec4(position, 1.0);\n"
          "}\n");
}

void hq_local_shadow_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform vec4 u_lp;\n"
          "varying vec3 v_p;\n");
    t.add(kPack);
    t.add("void main() {\n"
          "    gl_FragColor = vec4(pack24(length(v_p - u_lp.xyz) * u_lp.w), 1.0);\n"
          "}\n");
}

void cube_face(int face, float fwd[3], float up[3]) {
    for (int i = 0; i < 3; i++) fwd[i] = up[i] = 0.0f;
    int axis = face / 2;
    fwd[axis] = (face & 1) ? -1.0f : 1.0f;
    if (axis == 1) up[2] = 1.0f;        // the y faces look along y: z is up
    else up[1] = 1.0f;
}

void hq_depth_vertex(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform mat4 fgl_ModelViewProjection;\n"
          "uniform mat4 fgl_ModelView;\n"
          "uniform vec4 u_pp;\n"
          "attribute vec3 position;\n"
          "varying float v_d;\n"
          "void main() {\n"
          "    v_d = -(fgl_ModelView * vec4(position, 1.0)).z * u_pp.x;\n"
          "    gl_Position = fgl_ModelViewProjection * vec4(position, 1.0);\n"
          "}\n");
}

void hq_depth_fragment(char* out, size_t cap) {
    hq_shadow_fragment(out, cap);
}

void hq_normal_vertex(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform mat4 fgl_ModelViewProjection;\n"
          "uniform mat4 fgl_NormalMatrix;\n"
          "uniform mat4 fgl_View;\n"
          "attribute vec3 position;\n"
          "attribute vec3 normal;\n"
          "varying vec3 v_n;\n"
          "void main() {\n"
          "    v_n = (fgl_View * (fgl_NormalMatrix * vec4(normal, 0.0))).xyz;\n"
          "    gl_Position = fgl_ModelViewProjection * vec4(position, 1.0);\n"
          "}\n");
}

void hq_normal_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("varying vec3 v_n;\n"
          "void main() {\n"
          "    gl_FragColor = vec4(normalize(v_n) * 0.5 + 0.5, 1.0);\n"
          "}\n");
}

// ------------------------------------------------------------------ fullscreen passes

void pp_vertex(char* out, size_t cap) {
    Text t(out, cap);
    // u_z.x is the clip depth of the triangle: 0 (depth 0.5) unless a pass uses the depth
    // test to skip pixels (see passes.cpp, "masks").
    t.add("attribute vec3 position;\n"
          "uniform vec4 u_z;\n"
          "void main() {\n"
          "    gl_Position = vec4(position.xy, u_z.x, 1.0);\n"
          "}\n");
}

// Marks the sky: geometry pixels are discarded, so only the sky pixels write a depth
// (the pass is drawn at a nearer depth than the passes that follow, which then skip them).
void pp_skymask_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform sampler2D u_depth;\n"
          "uniform vec4 u_p;\n"
          "void main() {\n"
          "    vec3 c = texture(u_depth, gl_FragCoord.xy * u_p.xy).rgb;\n"
          "    gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);\n"
          "    if (c.r + c.g < 1.99) discard;\n"
          "}\n");
}

// Alchemy style AO (McGuire et al.): samples on a spiral around the pixel, each
// contributing max(0, v.n - bias) / (v.v + eps) for the vector v from the pixel to the
// sample's surface point; normals come from a normal pass.
void pp_ssao_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform sampler2D u_depth;\n"
          "uniform sampler2D u_normal;\n"
          "uniform vec4 u_p;\n"
          "uniform vec4 u_ao;\n"                  // radius m, strength, ortho flag, far m
          "uniform vec4 u_proj;\n");              // tan(fov/2) x, y (ortho: half extents)
    t.add(kPack);
    t.add("float depth_at(vec2 uv) {\n"
          "    return unpack24(texture(u_depth, uv).rgb);\n"
          "}\n"
          // view space: x right, y up, z forward (meters)
          "vec3 vpos(vec2 uv, float d) {\n"
          "    float s = mix(d, 1.0, u_ao.z);\n"
          "    return vec3((uv.x * 2.0 - 1.0) * u_proj.x * s, (1.0 - uv.y * 2.0) * u_proj.y * s, d);\n"
          "}\n"
          "void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
          "    float d0 = depth_at(uv) * u_ao.w;\n"
          "    vec3 P = vpos(uv, d0);\n"
          "    vec3 N = texture(u_normal, uv).rgb * 2.0 - 1.0;\n"
          "    N = vec3(N.x, N.y, -N.z);\n"
          "    float R = u_ao.x;\n"
          "    vec2 ruv = min(R / (2.0 * u_proj.xy * mix(d0, 1.0, u_ao.z)), vec2(0.12, 0.12));\n"
          "    ruv = max(ruv, u_p.xy * 2.0);\n"
          "    float ang = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
          "    vec2 cs = vec2(cos(ang), sin(ang));\n"
          "    float occ = 0.0;\n");
    for (int i = 0; i < kAoSamples; i++) {
        float h = fsqrt(((float)i + 0.5f) / (float)kAoSamples);
        float an = (float)i * 2.39996323f;
        float dx = fcos(an) * h, dy = fsin(an) * h;
        t.add("    {\n        vec2 suv = uv + vec2(cs.x * ");
        t.flt(dx); t.add(" - cs.y * "); t.flt(dy); t.add(", cs.y * "); t.flt(dx); t.add(" + cs.x * "); t.flt(dy);
        t.add(") * ruv;\n"
              "        float sd = depth_at(suv) * u_ao.w;\n"
              "        vec3 v = (vpos(suv, sd) - P) * (1.0 / R);\n"
              "        occ += max(dot(v, N) - 0.03, 0.0) / (dot(v, v) + 0.01) * clamp(2.0 - abs(sd - d0) / R, 0.0, 1.0);\n"
              "    }\n");
    }
    t.add("    float ao = pow(clamp(1.0 - occ * ");
    t.flt(1.0f / (float)kAoSamples);
    t.add(", 0.0, 1.0), u_ao.y);\n"
          "    float v16 = floor(clamp(d0 / u_ao.w, 0.0, 1.0) * 65535.0 + 0.5);\n"
          "    float hi = floor(v16 / 256.0);\n"
          "    gl_FragColor = vec4(ao, hi * 0.003921569, (v16 - hi * 256.0) * 0.003921569, 1.0);\n"
          "}\n");
}

void pp_blur_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform sampler2D u_src;\n"
          "uniform vec4 u_p;\n"
          "uniform vec4 u_dir;\n"                 // xy: uv step of one texel along the blur
          "uniform vec4 u_q;\n"                   // x: far (m)
          "float depth16(vec3 c) {\n"
          "    return (c.g * 65280.0 + c.b * 255.0) * 1.5259022e-5;\n"
          "}\n"
          "void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
          "    vec3 c0 = texture(u_src, uv).rgb;\n"
          "    float d0 = depth16(c0) * u_q.x;\n"
          "    float tol = 0.1 + 0.04 * d0;\n"
          "    float sum = c0.r * 0.4;\n"
          "    float wsum = 0.4;\n");
    static const float off[4] = { -2.0f, -1.0f, 1.0f, 2.0f };
    static const float wt[4] = { 0.15f, 0.25f, 0.25f, 0.15f };
    for (int i = 0; i < 4; i++) {
        t.add("    {\n        vec3 c = texture(u_src, uv + u_dir.xy * ");
        t.flt(off[i]);
        t.add(").rgb;\n"
              "        float w = clamp(1.0 - abs(depth16(c) * u_q.x - d0) / tol, 0.0, 1.0) * ");
        t.flt(wt[i]);
        t.add(";\n        sum += c.r * w;\n        wsum += w;\n    }\n");
    }
    t.add("    gl_FragColor = vec4(sum / wsum, c0.g, c0.b, 1.0);\n"
          "}\n");
}

void pp_bright_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform sampler2D u_src;\n"
          "uniform vec4 u_p;\n"
          "uniform vec4 u_q;\n"                   // x: threshold
          "vec3 hdr_dec(vec3 e) { return e * e * 8.0; }\n"
          "vec3 bright(vec3 e) {\n"
          "    vec3 c = hdr_dec(e);\n"
          "    float br = max(c.r, max(c.g, c.b));\n"
          "    float knee = u_q.x * 0.5 + 0.0001;\n"
          "    float soft = clamp(br - u_q.x + knee, 0.0, 2.0 * knee);\n"
          "    soft = soft * soft / (4.0 * knee);\n"
          "    return c * (max(soft, br - u_q.x) / max(br, 0.0001));\n"
          "}\n"
          "void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
          "    vec2 o = u_p.zw;\n"
          "    vec3 c = bright(texture(u_src, uv + vec2(-o.x, -o.y)).rgb) + bright(texture(u_src, uv + vec2(o.x, -o.y)).rgb)\n"
          "           + bright(texture(u_src, uv + vec2(-o.x, o.y)).rgb) + bright(texture(u_src, uv + vec2(o.x, o.y)).rgb);\n"
          "    gl_FragColor = vec4(sqrt(clamp(c * 0.25, 0.0, 8.0) * 0.125), 1.0);\n"
          "}\n");
}

void pp_down_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform sampler2D u_src;\n"
          "uniform vec4 u_p;\n"
          "void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
          "    vec2 o = u_p.zw;\n"
          "    vec3 c = texture(u_src, uv + vec2(-o.x, -o.y)).rgb + texture(u_src, uv + vec2(o.x, -o.y)).rgb\n"
          "           + texture(u_src, uv + vec2(-o.x, o.y)).rgb + texture(u_src, uv + vec2(o.x, o.y)).rgb;\n"
          "    gl_FragColor = vec4(c * 0.25, 1.0);\n"
          "}\n");
}

// out = fine + tent(coarse); u_p.zw is the coarse texel size.
void pp_up_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform sampler2D u_fine;\n"
          "uniform sampler2D u_coarse;\n"
          "uniform vec4 u_p;\n"
          "vec3 hdr_dec(vec3 e) { return e * e * 8.0; }\n"
          "void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
          "    vec2 o = u_p.zw * 0.5;\n"
          "    vec3 c = texture(u_coarse, uv + vec2(-o.x, -o.y)).rgb + texture(u_coarse, uv + vec2(o.x, -o.y)).rgb\n"
          "           + texture(u_coarse, uv + vec2(-o.x, o.y)).rgb + texture(u_coarse, uv + vec2(o.x, o.y)).rgb;\n"
          "    vec3 s = hdr_dec(texture(u_fine, uv).rgb) + hdr_dec(c * 0.25);\n"
          "    gl_FragColor = vec4(sqrt(clamp(s, 0.0, 8.0) * 0.125), 1.0);\n"
          "}\n");
}

void pp_composite_fragment(char* out, size_t cap, bool bloom) {
    Text t(out, cap);
    t.add("uniform sampler2D u_scene;\n");
    if (bloom) t.add("uniform sampler2D u_bloom;\nuniform vec4 u_q;\n");
    t.add("uniform vec4 u_p;\n");
    t.add(kTone);
    t.add("void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
          "    vec3 c = hdr_dec(texture(u_scene, uv).rgb);\n");
    if (bloom) t.add("    c += hdr_dec(texture(u_bloom, uv).rgb) * u_q.x;\n");
    t.add("    gl_FragColor = vec4(tone(c), 1.0);\n"
          "}\n");
}

// FXAA 3.11 (quality variant): luma edge detection, sub-pixel blending, and an edge end
// search (at 1, 2.5 and 5.5 pixels on each side). It is split in two passes: the mask
// pass copies the pixels without an edge (and marks them in the depth buffer), the main
// pass runs only on the others (the depth test skips the marked ones before shading).
// A shader may take 16 texture samples at most, or the host's JIT gives up on it.
void pp_fxaa_mask_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform sampler2D u_n;\n"
          "uniform vec4 u_p;\n"
          "float luma(vec3 c) {\n"
          "    return dot(c, vec3(0.299, 0.587, 0.114));\n"
          "}\n"
          "float lumann(vec2 p) {\n"
          "    return luma(texture(u_n, p).rgb);\n"
          "}\n"
          "void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
          "    vec3 cM = texture(u_n, uv).rgb;\n"
          "    float lM = luma(cM);\n"
          "    vec4 L = vec4(lumann(uv + vec2(0.0, -u_p.y)), lumann(uv + vec2(0.0, u_p.y)),\n"
          "                  lumann(uv + vec2(-u_p.x, 0.0)), lumann(uv + vec2(u_p.x, 0.0)));\n"
          "    float lMax = max(lM, max(max(L.x, L.y), max(L.z, L.w)));\n"
          "    float range = lMax - min(lM, min(min(L.x, L.y), min(L.z, L.w)));\n"
          "    gl_FragColor = vec4(cM, 1.0);\n"
          "    if (range >= max(0.0833, lMax * 0.166)) discard;\n"
          "}\n");
}

void pp_fxaa_fragment(char* out, size_t cap) {
    Text t(out, cap);
    // Registers are scarce (see pp_ssao_fragment): neighbours live in vec4s.
    t.add("uniform sampler2D u_n;\n"
          "uniform sampler2D u_l;\n"
          "uniform vec4 u_p;\n"
          "float luma(vec3 c) {\n"
          "    return dot(c, vec3(0.299, 0.587, 0.114));\n"
          "}\n"
          "float lumal(vec2 p) {\n"
          "    return luma(texture(u_l, p).rgb);\n"
          "}\n"
          "float lumann(vec2 p) {\n"
          "    return luma(texture(u_n, p).rgb);\n"
          "}\n"
          "void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
          "    float lM = lumann(uv);\n"
          // x north, y south, z west, w east
          "    vec4 L = vec4(lumann(uv + vec2(0.0, -u_p.y)), lumann(uv + vec2(0.0, u_p.y)),\n"
          "                  lumann(uv + vec2(-u_p.x, 0.0)), lumann(uv + vec2(u_p.x, 0.0)));\n"
          "    float range = max(lM, max(max(L.x, L.y), max(L.z, L.w))) - min(lM, min(min(L.x, L.y), min(L.z, L.w)));\n"
          // x nw, y ne, z sw, w se
          "    vec4 C = vec4(lumann(uv + vec2(-u_p.x, -u_p.y)), lumann(uv + vec2(u_p.x, -u_p.y)),\n"
          "                  lumann(uv + vec2(-u_p.x, u_p.y)), lumann(uv + vec2(u_p.x, u_p.y)));\n"
          "    float horz = 0.0;\n"
          "    float subpix = 0.0;\n"
          "    {\n"
          "        float lNS = L.x + L.y;\n"
          "        float lWE = L.z + L.w;\n"
          "        float lAvg = (2.0 * (lNS + lWE) + (C.x + C.y + C.z + C.w)) * 0.0833333;\n"
          "        float sc = clamp(abs(lAvg - lM) / max(range, 0.0001), 0.0, 1.0);\n"
          "        float sb = (3.0 - 2.0 * sc) * sc * sc;\n"
          "        subpix = sb * sb * 0.75;\n"
          "        float edgeH = abs(-2.0 * L.z + (C.x + C.z)) + abs(-2.0 * lM + lNS) * 2.0 + abs(-2.0 * L.w + (C.y + C.w));\n"
          "        float edgeV = abs(-2.0 * L.x + (C.x + C.y)) + abs(-2.0 * lM + lWE) * 2.0 + abs(-2.0 * L.y + (C.z + C.w));\n"
          "        horz = step(edgeV, edgeH);\n"
          "    }\n"
          "    float stepLen = 0.0;\n"
          "    float lAvgL = 0.0;\n"
          "    float gradScaled = 0.0;\n"
          "    {\n"
          "        float l1 = mix(L.z, L.x, horz);\n"
          "        float l2 = mix(L.w, L.y, horz);\n"
          "        float steep = step(abs(l2 - lM), abs(l1 - lM));\n"
          "        gradScaled = 0.25 * max(abs(l1 - lM), abs(l2 - lM));\n"
          "        stepLen = mix(u_p.x, u_p.y, horz) * mix(1.0, -1.0, steep);\n"
          "        lAvgL = 0.5 * (mix(l2, l1, steep) + lM);\n"
          "    }\n"
          "    vec2 off = mix(vec2(0.0, u_p.y), vec2(u_p.x, 0.0), horz);\n"
          "    vec4 pp = vec4(0.0);\n"
          "    vec2 e = vec2(0.0);\n"
          "    vec2 dn = vec2(0.0);\n"
          "    {\n"
          "        vec2 posB = uv + mix(vec2(stepLen * 0.5, 0.0), vec2(0.0, stepLen * 0.5), horz);\n"
          "        pp = vec4(posB - off, posB + off);\n"
          "        e = vec2(lumal(pp.xy), lumal(pp.zw)) - lAvgL;\n"
          "        dn = step(gradScaled, abs(e));\n"
          "    }\n");
    static const float q[2] = { 1.5f, 3.0f };
    for (int i = 0; i < 2; i++) {
        t.add("    if (dn.x < 0.5) { pp.xy -= off * ");
        t.flt(q[i]);
        t.add("; e.x = lumal(pp.xy) - lAvgL; dn.x = step(gradScaled, abs(e.x)); }\n"
              "    if (dn.y < 0.5) { pp.zw += off * ");
        t.flt(q[i]);
        t.add("; e.y = lumal(pp.zw) - lAvgL; dn.y = step(gradScaled, abs(e.y)); }\n");
    }
    t.add("    float po = 0.0;\n"
          "    {\n"
          "        float distN = mix(uv.y - pp.y, uv.x - pp.x, horz);\n"
          "        float distP = mix(pp.w - uv.y, pp.z - uv.x, horz);\n"
          "        float dirN = step(distN, distP);\n"
          "        float good = 1.0 - step(0.0, mix(e.y, e.x, dirN) * (lM - lAvgL));\n"
          "        po = max((0.5 - mix(distP, distN, dirN) / max(distN + distP, 0.000001)) * good, subpix);\n"
          "    }\n"
          "    gl_FragColor = vec4(texture(u_l, uv + mix(vec2(po * stepLen, 0.0), vec2(0.0, po * stepLen), horz)).rgb, 1.0);\n"
          "}\n");
}

}  // namespace fe::gfx::shaders
