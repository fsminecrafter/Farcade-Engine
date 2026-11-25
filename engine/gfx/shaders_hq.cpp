#include "gfx/shaders_hq.h"
#include <stdlib.h>
#include "gfx/shaders.h"
#include "gfx/shaders_post.h"
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

// The post chain (shaders_post.h) for this shader's choices, appended to `t`.
void add_post(Text& t, const PostKey& pk) {
    char buf[4096];
    emit_post_chain(buf, sizeof buf, pk);
    t.add(buf);
}

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
    "        vec3 d = light0(i).xyz - v_pos;\n"
    "        float dist = length(d);\n"
    "        L = d / max(dist, 0.0001);\n"
    "        float x = min(dist / max(light0(i).w, 0.0001), 1.0);\n"
    "        att = 1.0 - x * x;\n"
    "        att = att * att;\n"
    "    }\n";

// Blinn-Phong, as the base tier, with S the shadow factor.
const char* const kBlinnTail =
    "    float ndl = max(dot(N, L), 0.0);\n"
    "    vec3 H = normalize(L + V);\n"
    "    float sp = pow(max(dot(N, H), 0.0), shin) * ndl * att * S;\n"
    "    diff += light2(i).rgb * (ndl * att * S);\n"
    "    spec += light2(i).rgb * (sp * specn);\n"
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
    "    vec3 rad = light2(i).rgb * (att * S);\n"
    "    diff += rad * ndl * (1.0 - F);\n"
    "    spec += rad * F * (dv * ndl * 3.14159265);\n"
    "}\n";

const char* const kComp[4] = { "x", "y", "z", "w" };

// The atlas lookup of one point / spot shadow, written out (a function call
// would copy its arguments into new registers). In: d (light to surface),
// a = (right, up, forward) coordinates of d in the light camera, p = (first
// tile, tan of half the fov, relative bias, 1 / range). Out: LS.<comp>.
void emit_atlas_lookup(Text& t, const char* comp, bool soft) {
    t.add("        vec2 uv = clamp(vec2(0.5, 0.5) + vec2(a.x, -a.y) * (0.5 / (max(a.z, 0.0001) * p.y)), u_frame[9].z, 1.0 - u_frame[9].z);\n"
          "        p.y = floor((p.x + 0.5) * u_frame[9].y);\n"      // the tile's row
          "        uv = (vec2(p.x - p.y * u_frame[9].x, p.y) + uv) * u_frame[9].y;\n"
          "        p.w = length(d) * p.w * (1.0 - p.z);\n");         // the reference depth
    if (soft) {
        t.add("        p.y = u_frame[9].z * u_frame[9].y * 0.6667;\n"
              "        p.z = atlas_tap(uv - vec2(p.y, p.y), p.w);\n"
              "        p.z = p.z + atlas_tap(uv + vec2(p.y, -p.y), p.w);\n"
              "        p.z = p.z + atlas_tap(uv + vec2(-p.y, p.y), p.w);\n"
              "        p.z = p.z + atlas_tap(uv + vec2(p.y, p.y), p.w);\n"
              "        LS."); t.add(comp); t.add(" = p.z * 0.25;\n");
    } else {
        t.add("        LS."); t.add(comp); t.add(" = atlas_tap(uv, p.w);\n");
    }
}

// The light accessors of the lit shader (the lights start after the frame header).
void light_accessors(char* out, size_t cap) {
    Text t(out, cap);
    t.add("vec4 light0(float i) { return u_frame[");
    t.num(kFrameHeader); t.add(".0 + i * 3.0]; }\nvec4 light1(float i) { return u_frame[");
    t.num(kFrameHeader + 1); t.add(".0 + i * 3.0]; }\nvec4 light2(float i) { return u_frame[");
    t.num(kFrameHeader + 2); t.add(".0 + i * 3.0]; }\n");
}

// Vogel disk offsets of the soft shadow filter, rotated per pixel.
constexpr int kShadowTaps = 10;

}  // namespace

void hq_lit_vertex(char* out, size_t cap, int cascades) {
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
    if (cascades > 0) {
        t.add("layout(binding = 0) buffer vec4 u_frame[];\n");
        for (int c = 0; c < cascades; c++) { t.add("varying vec3 v_sh"); t.num(c); t.add(";\n"); }
    }
    t.add(kSrgb2Lin);
    t.add("void main() {\n"
          "    v_pos = (fgl_Model * vec4(position, 1.0)).xyz;\n"
          "    v_nrm = (fgl_NormalMatrix * vec4(normal, 0.0)).xyz;\n"
          "    v_alb = srgb2lin(u_mat.rgb);\n");
    if (cascades > 0) {
        // The receiver is moved along its normal by up to a few shadow texels where the
        // surface turns away from the light (normal offset bias); each cascade has its own texel size.
        t.add("    vec3 wn = normalize(v_nrm);\n"
              "    float sdl = clamp(dot(wn, u_frame[4].xyz), 0.0, 1.0);\n");
        for (int c = 0; c < cascades; c++) {
            int base = kCascadeBase + c * kCascadeStride;
            t.add("    {\n        mat4 shm = mat4(u_frame[");
            t.num(base); t.add("], u_frame["); t.num(base + 1); t.add("], u_frame["); t.num(base + 2); t.add("], u_frame["); t.num(base + 3);
            t.add("]);\n        v_sh"); t.num(c); t.add(" = (shm * vec4(v_pos + wn * (u_frame[");
            t.num(base + 4); t.add("].w * (1.0 - sdl)), 1.0)).xyz;\n    }\n");
        }
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
    const bool local = k.nps + k.nss > 0;
    if (total > 0 || k.shadow || local) {
        t.add("layout(binding = 0) buffer vec4 u_frame[];\n");
    }
    if (total > 0) {
        char hdr[256];
        light_accessors(hdr, sizeof hdr);
        t.add(hdr);
    }
    if (local) {
        t.add("vec4 local_shadow(float i) { return u_frame[");
        t.num(kFrameHeader + total * 3);
        t.add(".0 + i]; }\n");
    }
    t.add("varying vec3 v_pos;\n"
          "varying vec3 v_nrm;\n"
          "varying vec3 v_alb;\n");
    if (k.shadow) {
        t.add("uniform sampler2D u_shadow;\n");
        for (int c = 0; c < k.cascades; c++) { t.add("varying vec3 v_sh"); t.num(c); t.add(";\n"); }
    }
    if (k.ao) {
        t.add("uniform sampler2D u_ao;\n"
              "uniform vec4 u_aos;\n"            // 1 / AO size, AO size
              "uniform vec4 u_aof;\n");          // camera forward, far (m)
        // The AO lookup needs the target size; the post chain declares u_res
        // only when it is not the draft one (set_post sets it either way).
        if (k.post.draft) t.add("uniform vec4 u_res;\n");
    }
    if (k.gi) {
        char gbuf[2048];
        emit_gi_lookup(gbuf, sizeof gbuf);
        t.add(gbuf);
    }
    if (local) {
        t.add("uniform sampler2D u_atlas;\n");
    }
    {
        PostKey pk = k.post;
        pk.hdr_codec = k.hdr;
        add_post(t, pk);
    }
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
        // The cascade is picked by where the pixel lands: the first (finest) whose volume contains it
        // (|ndc xy| < 0.97, ndc z < 0.97), the last one otherwise. Its tile, texel scale and bias come from the frame data.
        t.add("float shadow_tap(vec2 uv, float dref) {\n"
              "    return step(dref, unpack24(texture(u_shadow, uv).rgb));\n"
              "}\n"
              "float shadow_factor(vec3 N, vec3 sdir) {\n"
              "    vec4 q = vec4(v_sh0, 0.0);\n");           // xyz: ndc in the chosen cascade, w: its index
        if (k.cascades > 1) {
            t.add("    {\n"
                  "        float rem = 1.0 - step(abs(q.x), 0.97) * step(abs(q.y), 0.97) * step(q.z, 0.97);\n");
            for (int c = 1; c < k.cascades; c++) {
                t.add("        {\n            vec3 sc = v_sh"); t.num(c); t.add(";\n");
                if (c < k.cascades - 1) {
                    t.add("            float fit = step(abs(sc.x), 0.97) * step(abs(sc.y), 0.97) * step(sc.z, 0.97);\n"
                          "            q = mix(q, vec4(sc, ");
                    t.flt((float)c);
                    t.add("), rem * fit);\n"
                          "            rem = rem * (1.0 - fit);\n");
                } else {
                    t.add("            q = mix(q, vec4(sc, ");
                    t.flt((float)c);
                    t.add("), rem);\n");
                }
                t.add("        }\n");
            }
            t.add("    }\n");
        }
        t.add("    vec4 pa = u_frame[");
        t.num(kCascadeBase + 4); t.add(".0 + q.w * "); t.flt((float)kCascadeStride);
        t.add("];\n"
              "    vec4 pb = u_frame["); t.num(kCascadeBase + 5); t.add(".0 + q.w * "); t.flt((float)kCascadeStride);
        t.add("];\n"
              "    vec2 g = vec2(dot(N, u_frame[7].xyz), dot(N, u_frame[8].xyz)) * (pb.y / max(dot(N, sdir), 0.2));\n"
              // q = (uv in the tile, biased reference depth, inside)
              "    q = vec4(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5, q.z * 0.5 + 0.5 - pb.z, 0.0);\n"
              "    q.w = step(0.0, q.x) * step(q.x, 1.0) * step(0.0, q.y) * step(q.y, 1.0) * step(q.z, 1.0);\n"
              "    q.xy = q.xy * pa.z + pa.xy;\n");
        if (k.shadow >= 2) {
            t.add("    vec2 cs = vec2(0.0);\n"
                  "    {\n"
                  "        float ang = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
                  "        cs = vec2(cos(ang), sin(ang));\n"
                  "    }\n"
                  "    float sum = 0.0;\n");
            for (int i = 0; i < kShadowTaps; i++) {
                // Vogel disk: radius sqrt((i + 0.5) / n), golden angle steps.
                float r = fsqrt(((float)i + 0.5f) / (float)kShadowTaps);
                float an = (float)i * 2.39996323f;
                float ox = fcos(an) * r, oy = fsin(an) * r;
                t.add("    {\n        vec2 o = vec2(cs.x * ");
                t.flt(ox); t.add(" - cs.y * "); t.flt(oy); t.add(", cs.y * "); t.flt(ox); t.add(" + cs.x * "); t.flt(oy);
                t.add(") * pb.x;\n"
                      "        sum += shadow_tap(q.xy + o * pa.z, q.z + g.x * o.x - g.y * o.y);\n    }\n");
            }
            t.add("    float lit = sum * ");
            t.flt(1.0f / (float)kShadowTaps);
            t.add(";\n");
        } else {
            t.add("    float lit = shadow_tap(q.xy, q.z);\n");
        }
        t.add("    return mix(1.0, lit, q.w);\n"
              "}\n");
    }
    t.add("void main() {\n"
          "    vec3 col = vec3(0.0);\n"
          "    {\n"                                  // the shading in a block: its registers are free for post()
          "    vec3 N = normalize(v_nrm);\n");
    // the probe GI too, in a block (the lookup's registers are free again afterwards)
    if (k.gi) t.add("    vec3 giA = vec3(0.0);\n    {\n        giA = gi_amb(N, v_pos);\n    }\n");
    // the shadow first: it needs many registers, main has few live values yet
    if (k.shadow && k.nd > 0) {
        // In a block: an inlined function's temporaries live until the block ends.
        t.add("    float SH = 1.0;\n    {\n        SH = shadow_factor(N, -light1(");
        t.num(k.shadow_light);
        t.add(").xyz);\n    }\n");
    }
    // point / spot shadows too, all four in one register
    if (local) {
        t.add("    vec4 LS = vec4(1.0);\n");
        for (int j = 0; j < k.nps; j++) {
            int i = k.nd + j;
            t.add("    {\n"
                  "        vec3 d = v_pos - light0("); t.num(i); t.add(").xyz;\n"
                  "        d = d + N * (local_shadow("); t.num(j); t.add(").w * length(d));\n"
                  "        vec3 a = abs(d);\n"
                  "        vec3 m = vec3(step(a.y, a.x) * step(a.z, a.x), 0.0, 0.0);\n"
                  "        m.y = (1.0 - m.x) * step(a.z, a.y);\n"
                  "        m.z = 1.0 - m.x - m.y;\n"
                  "        a = step(d, vec3(0.0));\n"            // 1 where d points the negative way
                  "        vec3 f = m * (1.0 - 2.0 * a);\n"
                  "        vec4 p = vec4(local_shadow("); t.num(j); t.add(").x + dot(m, a + vec3(0.0, 2.0, 4.0)), local_shadow("); t.num(j);
            t.add(").y, local_shadow("); t.num(j); t.add(").z, 1.0 / light0("); t.num(i); t.add(").w);\n"
                  "        m = cross(f, vec3(0.0, 1.0 - m.y, m.y));\n"   // right
                  "        a = vec3(dot(d, m), dot(d, cross(m, f)), dot(d, f));\n");
            emit_atlas_lookup(t, kComp[j], k.local_soft);
            t.add("    }\n");
        }
        for (int j = 0; j < k.nss; j++) {
            int i = k.nd + k.np + j, sj = k.nps + j;
            // the spot's camera from its direction, as atlas_pass builds it
            t.add("    {\n"
                  "        vec3 d = v_pos - light0("); t.num(i); t.add(").xyz;\n"
                  "        d = d + N * (local_shadow("); t.num(sj); t.add(").w * length(d));\n"
                  "        vec3 f = light1("); t.num(i); t.add(").xyz;\n"
                  "        vec3 m = normalize(cross(f, vec3(0.0, 1.0 - step(0.99, abs(f.y)), step(0.99, abs(f.y)))));\n"
                  "        vec3 a = vec3(dot(d, m), dot(d, cross(m, f)), dot(d, f));\n"
                  "        vec4 p = vec4(local_shadow("); t.num(sj); t.add(").xyz, 1.0 / light0("); t.num(i); t.add(").w);\n");
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
              "    vec3 L = -light1(i).xyz;\n"
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
        t.add("    att = att * smoothstep(light1(i).w, light2(i).w, dot(-L, light1(i).xyz));\n"
              "    float S = LS."); t.add(kComp[k.nps + j]); t.add(";\n");
        t.add(tail);
    }
    if (k.ns > k.nss) {
        t.add("    for (int i = "); t.num(k.nd + k.np + k.nss); t.add("; i < "); t.num(total); t.add("; i++) {\n");
        t.add(kPointHead);
        t.add("    att = att * smoothstep(light1(i).w, light2(i).w, dot(-L, light1(i).xyz));\n"
              "    float S = 1.0;\n");
        t.add(tail);
    }
    if (k.ao) {
        // AO is half resolution, its texels carry their depth (g, b: 16 bits of depth / far). The four around
        // the pixel are weighted by distance and by how close their depth is to the pixel's own, so an edge
        // pixel takes the AO of its own surface, not of what lies behind it (a bilinear lookup gave objects a
        // light outline: the background's unoccluded AO bled onto their edges).
        t.add("    float ao = 1.0;\n"
              "    {\n"
              "        vec2 apos = gl_FragCoord.xy / u_res.xy * u_aos.zw - 0.5;\n"
              "        vec2 abase = floor(apos);\n"
              "        vec2 afr = apos - abase;\n"
              "        float vd = dot(v_pos - u_cam.xyz, u_aof.xyz);\n"
              "        float atol = 0.05 + 0.02 * vd;\n"
              "        float asum = 0.0;\n"
              "        float awsum = 0.0;\n");
        static const char* const taps[4][3] = {
            { "0.0, 0.0", "(1.0 - afr.x) * (1.0 - afr.y)", "" }, { "1.0, 0.0", "afr.x * (1.0 - afr.y)", "" },
            { "0.0, 1.0", "(1.0 - afr.x) * afr.y", "" }, { "1.0, 1.0", "afr.x * afr.y", "" },
        };
        for (const auto& tp : taps) {
            t.add("        {\n            vec3 ac = texture(u_ao, (abase + vec2(");
            t.add(tp[0]);
            t.add(") + 0.5) * u_aos.xy).rgb;\n"
                  "            float add = ((ac.g * 65280.0 + ac.b * 255.0) * 1.5259022e-5 * u_aof.w - vd) / atol;\n"
                  "            float aw = (");
            t.add(tp[1]);
            t.add(" + 0.001) / (0.0001 + add * add);\n"
                  "            asum += ac.r * aw;\n"
                  "            awsum += aw;\n"
                  "        }\n");
        }
        t.add("        ao = asum / awsum;\n"
              "    }\n"
              "    float aod = 1.0 - (1.0 - ao) * 0.4;\n");
    } else {
        t.add("    float ao = 1.0;\n    float aod = 1.0;\n");
    }
    if (k.pbr) {
        t.add(k.gi ? "    vec3 hemiN = giA;\n" : "    vec3 hemiN = mix(u_gnd.rgb, u_sky.rgb, N.y * 0.5 + 0.5);\n");
        t.add("    vec3 R = reflect(-V, N);\n"
              "    vec3 hemiR = mix(u_gnd.rgb, u_sky.rgb, R.y * 0.5 + 0.5);\n"
              "    vec3 env = mix(hemiR, (u_gnd.rgb + u_sky.rgb) * 0.5, rough * rough);\n"
              "    vec4 rr = rough * vec4(-1.0, -0.0275, -0.572, 0.022) + vec4(1.0, 0.0425, 1.04, -0.04);\n"
              "    float a004 = min(rr.x * rr.x, exp2(-9.28 * M4.z)) * rr.x + rr.y;\n"
              "    vec2 AB = vec2(-1.04, 1.04) * a004 + rr.zw;\n"
              "    vec3 ks = F0 * AB.x + AB.y;\n"
              "    vec3 ambient = dalb * hemiN * (1.0 - ks) + env * ks;\n"
              "    col = ambient * ao + (dalb * diff + spec) * aod;\n");
    } else {
        t.add(k.gi ? "    vec3 amb = giA;\n" : "    vec3 amb = mix(u_gnd.rgb, u_sky.rgb, N.y * 0.5 + 0.5);\n");
        t.add("    col = v_alb * (amb * ao + diff * aod) + spec * aod;\n");
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

void hq_unlit_fragment(char* out, size_t cap, bool fog, bool hdr, const PostKey& post) {
    Text t(out, cap);
    t.add("uniform vec4 u_cam;\n"
          "uniform vec4 u_fog;\n"
          "varying vec3 v_pos;\n"
          "varying vec3 v_alb;\n");
    {
        PostKey pk = post;
        pk.hdr_codec = hdr;
        add_post(t, pk);
    }
    t.add("void main() {\n"
          "    vec3 col = v_alb;\n");
    if (fog) t.add("    col = mix(u_fog.rgb, col, exp(-u_fog.w * length(u_cam.xyz - v_pos)));\n");
    t.add(hdr ? "    gl_FragColor = vec4(hdr_enc(col), 1.0);\n}\n" : "    gl_FragColor = vec4(post(col), 1.0);\n}\n");
}

void hq_sky_fragment(char* out, size_t cap, bool hdr, const PostKey& post, CloudKey ck) {
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
    {
        PostKey pk = post;
        pk.hdr_codec = hdr;
        add_post(t, pk);
    }
    if (ck.quality) {
        char* cs = (char*)malloc(kSourceBytes);
        if (cs) { clouds_shader(cs, kSourceBytes, ck, 0); t.add(cs); free(cs); }
    }
    t.add("void main() {\n"
          "    vec3 d = normalize(u_fwd.xyz + u_rx.xyz * v_ndc.x + u_ry.xyz * v_ndc.y);\n"
          "    float up = clamp(d.y, 0.0, 1.0);\n"
          "    vec3 above = mix(u_hor.rgb, u_zen.rgb, sqrt(up));\n"
          "    vec3 col = mix(above, u_grd.rgb, smoothstep(0.0, 0.12, -d.y));\n"
          "    col = mix(col, u_zen.rgb, u_zen.w);\n"
          "    {\n"
          "    float c = dot(d, u_sun.xyz);\n"
          "    float core = u_sun.w;\n"
          "    float rim = 1.0 - (1.0 - core) * 1.6;\n"
          "    float disc = smoothstep(rim, core, c) * step(core, 1.0);\n"
          "    float halo = pow(max(c, 0.0), 256.0) * 0.15 * step(core, 1.0);\n"
          "    col += u_suncol.rgb * (disc + halo) * (1.0 - u_zen.w);\n"
          "    }\n");
    if (ck.quality) {
        char* cs = (char*)malloc(kSourceBytes);
        if (cs) { clouds_shader(cs, kSourceBytes, ck, 1); t.add(cs); free(cs); }
    }
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

void pp_white_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("void main() {\n"
          "    gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);\n"
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

// ---- GTAO (Jimenez et al., "Practical real-time strategies for accurate indirect occlusion"): the horizon
// of each of 2 slice directions is searched with 3 steps on both sides (the 12 depth taps keep the shader
// under the 16 samples the host JIT accepts), and the visibility of the cosine weighted hemisphere is
// integrated analytically from the two horizon angles and the normal projected into the slice plane.
// Same inputs and output as pp_ssao_fragment (u_ao.xy radius, strength; u_gt.x slice rotation per frame).
const char* const kAcos =
    "float acos_(float x) {\n"
    "    float a = abs(x);\n"
    "    float r = sqrt(max(1.0 - a, 0.0)) * (1.5707288 + a * (-0.2121144 + a * (0.074261 - a * 0.0187293)));\n"
    "    return mix(r, 3.14159265 - r, 1.0 - step(0.0, x));\n"
    "}\n";

void pp_gtao_fragment(char* out, size_t cap) {
    Text t(out, cap);
    // Registers: every variable takes one of the 32 vec4 temporaries, so scalars live in vec4s (q, sc) and each
    // tap is its own block (an inlined function's temporaries stay allocated until the block ends).
    t.add("uniform sampler2D u_depth;\n"
          "uniform sampler2D u_normal;\n"
          "uniform vec4 u_p;\n"
          "uniform vec4 u_ao;\n"                  // radius m, strength, ortho flag, far m
          "uniform vec4 u_proj;\n"                // tan(fov/2) x, y
          "uniform vec4 u_gt;\n");                // x: rotation offset of this frame (0..1)
    t.add(kPack);
    t.add(kAcos);
    t.add("float depth_at(vec2 uv) {\n"
          "    return unpack24(texture(u_depth, uv).rgb) * u_ao.w;\n"
          "}\n"
          "vec3 vpos(vec2 uv, float d) {\n"
          "    float s = mix(d, 1.0, u_ao.z);\n"
          "    return vec3((uv.x * 2.0 - 1.0) * u_proj.x * s, (1.0 - uv.y * 2.0) * u_proj.y * s, d);\n"
          "}\n"
          "void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
          "    vec3 P = vec3(0.0);\n"
          "    vec4 q = vec4(0.0);\n"               // xy: uv radius of the search, z: noise, w: step jitter
          "    {\n"
          "        float d0 = depth_at(uv);\n"
          "        P = vpos(uv, d0);\n"
          "        q.xy = max(min(u_ao.x / (2.0 * u_proj.xy * mix(d0, 1.0, u_ao.z)), vec2(0.15, 0.15)), u_p.xy * 3.0);\n"
          "        q.z = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
          "        q.w = fract(q.z * 7.0 + u_gt.x * 3.0);\n"
          "    }\n"
          "    vec3 N = texture(u_normal, uv).rgb * 2.0 - 1.0;\n"
          "    N = vec3(N.x, N.y, -N.z);\n"
          "    vec3 V = normalize(-P);\n"
          "    float vis = 0.0;\n");
    for (int sl = 0; sl < 2; sl++) {
        t.add("    {\n"
              "        float phi = (fract(q.z + u_gt.x) + ");
        t.flt(sl * 0.5f);
        t.add(") * 3.14159265;\n"
              "        vec3 dir3 = vec3(cos(phi), -sin(phi), 0.0);\n"
              "        vec3 pn = N - cross(dir3, V) * dot(N, cross(dir3, V));\n"
              "        vec4 sc = vec4(max(length(pn), 0.0001), 0.0, 0.0, 0.0);\n"        // x: |pn|, y: cos n, z: n
              "        sc.y = clamp(dot(pn, V) / sc.x, -1.0, 1.0);\n"
              "        sc.z = acos_(sc.y) * (step(0.0, dot(dir3 - dot(dir3, V) * V, pn)) * 2.0 - 1.0);\n"
              "        vec2 duv = vec2(dir3.x, -dir3.y) * q.xy;\n"
              "        vec2 h = vec2(-1.0, -1.0);\n");
        for (int k = 0; k < 3; k++) {
            for (int side = 0; side < 2; side++) {
                const char* sg = side == 0 ? "+" : "-";
                const char* hc = side == 0 ? "x" : "y";
                t.add("        {\n            vec2 u1 = uv "); t.add(sg); t.add(" duv * ((");
                t.flt((float)k + 0.5f);
                t.add(" + q.w - 0.5) / 3.0);\n"
                      "            vec3 d1 = vpos(u1, depth_at(u1)) - P;\n"
                      "            float l1 = max(length(d1), 0.0001);\n"
                      "            h."); t.add(hc); t.add(" = max(h."); t.add(hc);
                t.add(", mix(-1.0, dot(d1, V) / l1, clamp(2.0 - l1 / u_ao.x, 0.0, 1.0)));\n"
                      "        }\n");
            }
        }
        t.add("        vec2 hh = vec2(-acos_(clamp(h.y, -1.0, 1.0)), acos_(clamp(h.x, -1.0, 1.0)));\n"
              "        // h.x is the horizon on the +direction side (the positive angle), h.y on the other side\n"
              "        hh = sc.z + vec2(max(hh.x - sc.z, -1.5707963), min(hh.y - sc.z, 1.5707963));\n"
              "        float sn = sin(sc.z);\n"
              "        vis += sc.x * 0.25 * ((-cos(2.0 * hh.x - sc.z) + sc.y + 2.0 * hh.x * sn) + (-cos(2.0 * hh.y - sc.z) + sc.y + 2.0 * hh.y * sn));\n"
              "    }\n");
    }
    t.add("    float ao = pow(clamp(vis * 0.5, 0.0, 1.0), u_ao.y);\n"
          "    float v16 = floor(clamp(P.z / u_ao.w, 0.0, 1.0) * 65535.0 + 0.5);\n"
          "    float hi = floor(v16 / 256.0);\n"
          "    gl_FragColor = vec4(ao, hi * 0.003921569, (v16 - hi * 256.0) * 0.003921569, 1.0);\n"
          "}\n");
}

// Temporal accumulation of the AO (r) while nothing moves: out = mix(history, current, u_q.x); depth (g, b) from the
// current frame. u_q.x = 1 takes the current frame as it is (the camera or the scene moved).
void pp_ao_accum_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform sampler2D u_cur;\n"
          "uniform sampler2D u_hist;\n"
          "uniform vec4 u_p;\n"
          "uniform vec4 u_q;\n"
          "void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
          "    vec3 c = texture(u_cur, uv).rgb;\n"
          "    float h = texture(u_hist, uv).r;\n"
          "    gl_FragColor = vec4(mix(h, c.r, u_q.x), c.g, c.b, 1.0);\n"
          "}\n");
}

// A plain copy (nearest) of u_src: history updates.
void pp_copy_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform sampler2D u_src;\n"
          "uniform vec4 u_p;\n"
          "void main() {\n"
          "    gl_FragColor = vec4(texture(u_src, gl_FragCoord.xy * u_p.xy).rgb, 1.0);\n"
          "}\n");
}

// ---- material G-buffer for SSR: r = roughness, g = metallic (u_mm per item)
void hq_material_vertex(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform mat4 fgl_ModelViewProjection;\n"
          "attribute vec3 position;\n"
          "void main() {\n"
          "    gl_Position = fgl_ModelViewProjection * vec4(position, 1.0);\n"
          "}\n");
}

void hq_material_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform vec4 u_mm;\n"
          "void main() {\n"
          "    gl_FragColor = vec4(u_mm.x, u_mm.y, 0.0, 1.0);\n"
          "}\n");
}

// ---- SSR. The mask pass discards the pixels that get reflections (geometry smoother than the cutoff, u_q.x),
// so the pixels it keeps (sky, rough surfaces) write a depth and the SSR pass skips them.
void pp_ssrmask_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform sampler2D u_depth;\n"
          "uniform sampler2D u_mat;\n"
          "uniform vec4 u_p;\n"
          "uniform vec4 u_q;\n"
          "void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
          "    vec3 c = texture(u_depth, uv).rgb;\n"
          "    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
          "    if (c.r + c.g < 1.99 && texture(u_mat, uv).r < u_q.x) discard;\n"
          "}\n");
}

// Marches the reflection ray in view space (11 steps, spaced quadratically, jittered) against the depth buffer
// and adds the scene colour found there, weighted by Fresnel and smoothness. A hit must be a few texels away
// and on a surface facing the ray (no self-hits where the ray grazes its own object), and reflections fade
// out at silhouettes, where the normal is nearly side-on (11 depth taps + the hit's normal keep the shader
// within the CPU JIT's 16 samples); u_ao.zw: ortho flag, far m;
// u_q: x max distance m, y thickness m, z roughness cutoff. Output: the HDR encoded contribution.
void pp_ssr_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform sampler2D u_depth;\n"
          "uniform sampler2D u_normal;\n"
          "uniform sampler2D u_mat;\n"
          "uniform sampler2D u_scene;\n"
          "uniform vec4 u_p;\n"
          "uniform vec4 u_ao;\n"                  // x: ortho flag, y: far m, z: frame jitter
          "uniform vec4 u_proj;\n"
          "uniform vec4 u_q;\n");
    t.add(kPack);
    t.add("float depth_at(vec2 uv) {\n"
          "    return unpack24(texture(u_depth, uv).rgb) * u_ao.y;\n"
          "}\n"
          "vec3 vpos(vec2 uv, float d) {\n"
          "    float s = mix(d, 1.0, u_ao.x);\n"
          "    return vec3((uv.x * 2.0 - 1.0) * u_proj.x * s, (1.0 - uv.y * 2.0) * u_proj.y * s, d);\n"
          "}\n"
          "void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
          "    vec3 P = vpos(uv, depth_at(uv));\n"
          "    vec3 N = texture(u_normal, uv).rgb * 2.0 - 1.0;\n"
          "    N = vec3(N.x, N.y, -N.z);\n"
          "    vec3 V = normalize(P);\n"
          "    vec3 Rr = reflect(V, N);\n"
          "    vec2 mat = texture(u_mat, uv).rg;\n"
          "    vec4 st = vec4(uv, 0.0, 0.0);\n"        // xy: the hit uv, z: hit, w: jitter
          "    st.w = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + u_ao.z);\n");
    const int steps = 11;
    for (int i = 0; i < steps; i++) {
        t.add("    {\n        vec3 S = P + Rr * (((");
        t.flt((float)i + 0.5f);
        t.add(" + st.w - 0.5) / 11.0) * ((");
        t.flt((float)i + 0.5f);
        t.add(" + st.w - 0.5) / 11.0) * u_q.x + 0.05);\n"
              "        vec2 suv = mix(vec2(S.x / max(S.z, 0.05) / u_proj.x * 0.5 + 0.5, 0.5 - S.y / max(S.z, 0.05) / u_proj.y * 0.5),\n"
              "                       vec2(S.x / u_proj.x * 0.5 + 0.5, 0.5 - S.y / u_proj.y * 0.5), u_ao.x);\n"
              "        float diff = S.z - depth_at(suv);\n"
              "        vec2 away = (suv - uv) / u_p.xy;\n"     // in texels: a hit next to the start is the surface itself
              "        float ok = step(0.0, diff) * step(diff, u_q.y + 0.05 * S.z) * step(st.z, 0.5) * step(0.0, suv.x) * step(suv.x, 1.0) * step(0.0, suv.y) * step(suv.y, 1.0)\n"
              "                 * step(9.0, dot(away, away));\n"
              "        st.xy = mix(st.xy, suv, ok);\n"
              "        st.z = max(st.z, ok);\n"
              "    }\n");
    }
    t.add("    float k = 0.0;\n"
          "    {\n"
          "        vec3 Nh = texture(u_normal, st.xy).rgb * 2.0 - 1.0;\n"   // the surface hit must face the ray
          "        Nh = vec3(Nh.x, Nh.y, -Nh.z);\n"
          "        float facing = clamp(-dot(Nh, Rr) * 4.0, 0.0, 1.0);\n"
          "        vec2 e = abs(st.xy - 0.5) * 2.0;\n"
          "        float edge = clamp((1.0 - max(e.x, e.y)) * 8.0, 0.0, 1.0);\n"
          "        float nv = clamp(dot(N, -V), 0.0, 1.0);\n"
          "        float silhouette = clamp((nv - 0.08) * 6.0, 0.0, 1.0);\n"   // side-on pixels: unreliable rays
          "        float fr = pow(1.0 - nv, 5.0);\n"
          "        float f0 = mix(0.04, 0.8, mat.g);\n"
          "        float sm = clamp(1.0 - mat.r / u_q.z, 0.0, 1.0);\n"
          "        k = (f0 + (1.0 - f0) * fr) * sm * sm * st.z * edge * facing * silhouette * clamp(Rr.z * 4.0 + 0.2, 0.0, 1.0);\n"
          "    }\n"
          "    vec3 c = texture(u_scene, st.xy).rgb;\n"
          "    c = c * c * 8.0 * mix(1.0, 0.8, mat.g);\n"
          "    gl_FragColor = vec4(sqrt(clamp(c * k, 0.0, 8.0) * 0.125), 1.0);\n"
          "}\n");
}

void pp_ssrblur_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform sampler2D u_src;\n"
          "uniform sampler2D u_depth;\n"
          "uniform vec4 u_p;\n"
          "uniform vec4 u_dir;\n"                 // xy: uv step of one tap
          "uniform vec4 u_q;\n");                 // x: far (m)
    t.add(kPack);
    t.add("void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
          "    float d0 = unpack24(texture(u_depth, uv).rgb) * u_q.x;\n"
          "    float tol = 0.05 + 0.03 * d0;\n"
          "    vec3 sum = texture(u_src, uv).rgb * 0.3;\n"
          "    float wsum = 0.3;\n");
    static const float off[4] = { -2.0f, -1.0f, 1.0f, 2.0f };
    static const float wt[4] = { 0.15f, 0.2f, 0.2f, 0.15f };
    for (int i = 0; i < 4; i++) {
        t.add("    {\n        vec2 o = uv + u_dir.xy * ");
        t.flt(off[i]);
        t.add(";\n        float w = clamp(1.0 - abs(unpack24(texture(u_depth, o).rgb) * u_q.x - d0) / tol, 0.0, 1.0) * ");
        t.flt(wt[i]);
        t.add(";\n        sum += texture(u_src, o).rgb * w;\n        wsum += w;\n    }\n");
    }
    t.add("    gl_FragColor = vec4(sum / wsum, 1.0);\n"
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

// The bright pass: 4 bilinear taps = a 4x4 box of the scene, each tap thresholded with a soft knee.
// The taps are weighted by 1 / (1 + luma) (Karis average), so a few very bright pixels do not make
// the glow flicker.
void pp_bright_fragment(char* out, size_t cap, bool karis) {
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
          "    vec3 a = bright(texture(u_src, uv + vec2(-o.x, -o.y)).rgb);\n"
          "    vec3 b = bright(texture(u_src, uv + vec2(o.x, -o.y)).rgb);\n"
          "    vec3 c = bright(texture(u_src, uv + vec2(-o.x, o.y)).rgb);\n"
          "    vec3 d = bright(texture(u_src, uv + vec2(o.x, o.y)).rgb);\n");
    if (karis)
        t.add("    vec4 w = 1.0 / (1.0 + vec4(dot(a, vec3(0.2126, 0.7152, 0.0722)), dot(b, vec3(0.2126, 0.7152, 0.0722)),\n"
              "                               dot(c, vec3(0.2126, 0.7152, 0.0722)), dot(d, vec3(0.2126, 0.7152, 0.0722))));\n"
              "    vec3 s = (a * w.x + b * w.y + c * w.z + d * w.w) / (w.x + w.y + w.z + w.w);\n");
    else
        t.add("    vec3 s = (a + b + c + d) * 0.25;\n");
    t.add(          "    gl_FragColor = vec4(sqrt(clamp(s, 0.0, 8.0) * 0.125), 1.0);\n"
          "}\n");
}

// The downsample of the chain: 13 bilinear taps over a 6x6 source area (the filter of the call of duty
// "Next generation post processing" talk): 4 inner boxes, 4 outer edges, 4 corners, the centre.
void pp_down_fragment(char* out, size_t cap, bool wide) {
    Text t(out, cap);
    if (!wide) {
        // the simple chain: 4 bilinear taps, a 4x4 box
        t.add("uniform sampler2D u_src;\n"
              "uniform vec4 u_p;\n"
              "void main() {\n"
              "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
              "    vec2 o = u_p.zw;\n"
              "    vec3 c = texture(u_src, uv + vec2(-o.x, -o.y)).rgb + texture(u_src, uv + vec2(o.x, -o.y)).rgb\n"
              "           + texture(u_src, uv + vec2(-o.x, o.y)).rgb + texture(u_src, uv + vec2(o.x, o.y)).rgb;\n"
              "    gl_FragColor = vec4(c * 0.25, 1.0);\n"
              "}\n");
        return;
    }
    t.add("uniform sampler2D u_src;\n"
          "uniform vec4 u_p;\n"
          "vec3 tap(vec2 uv, float x, float y) {\n"
          "    return texture(u_src, uv + vec2(x, y) * u_p.zw).rgb;\n"
          "}\n"
          "void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
          "    vec3 c = tap(uv, 0.0, 0.0) * 0.125;\n"
          "    c += (tap(uv, -1.0, -1.0) + tap(uv, 1.0, -1.0) + tap(uv, -1.0, 1.0) + tap(uv, 1.0, 1.0)) * 0.125;\n"
          "    c += (tap(uv, 0.0, -2.0) + tap(uv, -2.0, 0.0) + tap(uv, 2.0, 0.0) + tap(uv, 0.0, 2.0)) * 0.0625;\n"
          "    c += (tap(uv, -2.0, -2.0) + tap(uv, 2.0, -2.0) + tap(uv, -2.0, 2.0) + tap(uv, 2.0, 2.0)) * 0.03125;\n"
          "    gl_FragColor = vec4(c, 1.0);\n"
          "}\n");
}

// out = fine + k * tent(coarse); u_p.zw is the coarse texel size, u_q.x the weight k of the wider levels
// (the glow's radius: 0 keeps only the finest level, 1 doubles the wide ones).
void pp_up_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform sampler2D u_fine;\n"
          "uniform sampler2D u_coarse;\n"
          "uniform vec4 u_p;\n"
          "uniform vec4 u_q;\n"
          "vec3 hdr_dec(vec3 e) { return e * e * 8.0; }\n"
          "void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
          "    vec2 o = u_p.zw * 0.5;\n"
          "    vec3 c = texture(u_coarse, uv + vec2(-o.x, -o.y)).rgb + texture(u_coarse, uv + vec2(o.x, -o.y)).rgb\n"
          "           + texture(u_coarse, uv + vec2(-o.x, o.y)).rgb + texture(u_coarse, uv + vec2(o.x, o.y)).rgb;\n"
          "    vec3 s = hdr_dec(texture(u_fine, uv).rgb) + hdr_dec(c * 0.25) * u_q.x;\n"
          "    gl_FragColor = vec4(sqrt(clamp(s, 0.0, 8.0) * 0.125), 1.0);\n"
          "}\n");
}

void pp_composite_fragment(char* out, size_t cap, bool bloom, const PostKey& post, bool chromatic, bool ssr) {
    Text t(out, cap);
    t.add("uniform sampler2D u_scene;\n");
    if (ssr) t.add("uniform sampler2D u_ssr;\n");
    if (bloom) t.add("uniform sampler2D u_bloom;\nuniform vec4 u_q;\n");
    t.add("uniform vec4 u_p;\n");
    if (chromatic) t.add("uniform vec4 u_ca;\n");
    {
        PostKey pk = post;
        pk.hdr_codec = true;
        add_post(t, pk);
    }
    t.add("void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n");
    if (chromatic)
        // colour fringes: red and blue are sampled a little further out and in along the line from the centre
        t.add("    vec2 cd = (uv - 0.5) * u_ca.x;\n"
              "    vec3 c = vec3(hdr_dec(texture(u_scene, uv + cd).rgb).r, hdr_dec(texture(u_scene, uv).rgb).g,\n"
              "                  hdr_dec(texture(u_scene, uv - cd).rgb).b);\n");
    else
        t.add("    vec3 c = hdr_dec(texture(u_scene, uv).rgb);\n");
    if (bloom) t.add("    c += hdr_dec(texture(u_bloom, uv).rgb) * u_q.x;\n");
    if (ssr) t.add("    c += hdr_dec(texture(u_ssr, uv).rgb);\n");
    t.add("    gl_FragColor = vec4(tone(c), 1.0);\n"
          "}\n");
}

// ---- SMAA-style morphological AA (see post_stack.h smaa_area for the model and its limits)
// Edge detection on luma (threshold 0.1, local contrast adaptation): r = an edge on the left side of the pixel,
// g = an edge on its top side. Pixels without an edge are discarded (they stay black).
void pp_smaa_edge_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform sampler2D u_src;\n"
          "uniform vec4 u_p;\n"
          "float luma(vec2 uv) {\n"
          "    return dot(texture(u_src, uv).rgb, vec3(0.299, 0.587, 0.114));\n"
          "}\n"
          "void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
          "    float L = luma(uv);\n"
          "    vec2 lt = vec2(luma(uv - vec2(u_p.x, 0.0)), luma(uv - vec2(0.0, u_p.y)));\n"
          "    vec2 delta = abs(L - lt);\n"
          "    vec2 maxd = max(delta, abs(L - vec2(luma(uv + vec2(u_p.x, 0.0)), luma(uv + vec2(0.0, u_p.y)))));\n"
          "    maxd = max(maxd, abs(lt - vec2(luma(uv - vec2(2.0 * u_p.x, 0.0)), luma(uv - vec2(0.0, 2.0 * u_p.y)))));\n"
          "    vec2 e = step(0.1, delta) * step(maxd * 0.5, delta);\n"
          "    gl_FragColor = vec4(e, 0.0, 1.0);\n"
          "    if (e.x + e.y < 0.5) discard;\n"
          "}\n");
}

// Marks the pixels without an edge (they get a depth, so the next passes skip them).
void pp_smaa_mask_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform sampler2D u_edges;\n"
          "uniform vec4 u_p;\n"
          "void main() {\n"
          "    vec2 e = texture(u_edges, gl_FragCoord.xy * u_p.xy).rg;\n"
          "    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
          "    if (e.x + e.y > 0.5) discard;\n"
          "}\n");
}

// Blend weights of the edges that run along x (vertical = false: the edge on the top of the pixel, searched to the
// left and right) or along y (vertical = true: the edge on its left, searched up and down), by the area model:
// the length of the run in each direction (up to 8) and the perpendicular edges that end it pick a texel of the area
// texture. Output: the texel (the two shares * 2).
void pp_smaa_weights_fragment(char* out, size_t cap, bool vertical) {
    Text t(out, cap);
    const char* ch = vertical ? "r" : "g";           // the edge channel along the run
    const char* cc = vertical ? "g" : "r";           // the channel of the edges that cross it
    t.add("uniform sampler2D u_edges;\n"
          "uniform sampler2D u_area;\n"
          "uniform vec4 u_p;\n"
          "void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n");
    t.add(vertical ? "    vec2 a = vec2(0.0, u_p.y);\n    vec2 n = vec2(u_p.x, 0.0);\n"
                   : "    vec2 a = vec2(u_p.x, 0.0);\n    vec2 n = vec2(0.0, u_p.y);\n");
    t.add("    vec4 res = vec4(0.0);\n"
          "    if (texture(u_edges, uv).");
    t.add(ch);
    t.add(" > 0.5) {\n"
          "        vec2 d = vec2(0.0);\n"            // run lengths before / after the pixel
          "        vec2 alive = vec2(1.0);\n");
    for (int k = 1; k <= 7; k++) {
        t.add("        {\n            alive.x = alive.x * texture(u_edges, uv - a * ");
        t.flt((float)k);
        t.add(").");
        t.add(ch);
        t.add(";\n            alive.y = alive.y * texture(u_edges, uv + a * ");
        t.flt((float)k);
        t.add(").");
        t.add(ch);
        t.add(";\n            d = d + alive;\n        }\n");
    }
    t.add("        vec2 cr = vec2(0.0);\n"            // crossing flags: x before, y after (bit 0 the -n side, bit 1 the +n side)
          "        {\n"
          "            vec2 pb = uv - a * d.x;\n"
          "            vec2 pa = uv + a * (d.y + 1.0);\n"
          "            cr.x = texture(u_edges, pb - n).");
    t.add(cc);
    t.add(" + 2.0 * texture(u_edges, pb).");
    t.add(cc);
    t.add(";\n"
          "            cr.y = texture(u_edges, pa - n).");
    t.add(cc);
    t.add(" + 2.0 * texture(u_edges, pa).");
    t.add(cc);
    t.add(";\n"
          "        }\n"
          "        res = vec4(texture(u_area, vec2(cr.x * 8.0 + d.x + 0.5, cr.y * 8.0 + d.y + 0.5) * 0.03125).rg, 0.0, 1.0);\n"
          "    }\n"
          "    gl_FragColor = res;\n"
          "}\n");
}

// Neighbourhood blending: the pixel takes a share of the neighbour across the contour, from its own weights and
// those its neighbours stored for it (the bottom share is in the weights of the pixel below, the right share in
// the pixel to the right). u_wh, u_wv: the weight targets of the two axes.
void pp_smaa_blend_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform sampler2D u_src;\n"
          "uniform sampler2D u_wh;\n"
          "uniform sampler2D u_wv;\n"
          "uniform vec4 u_p;\n"
          "void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
          "    vec4 w = vec4(texture(u_wh, uv).r, texture(u_wh, uv + vec2(0.0, u_p.y)).g,\n"
          "                  texture(u_wv, uv).r, texture(u_wv, uv + vec2(u_p.x, 0.0)).g) * 0.5;\n"
          "    float h = max(w.x, w.y);\n"
          "    float v = max(w.z, w.w);\n"
          "    float isH = step(v, h);\n"
          "    vec2 off = mix(vec2(mix(u_p.x, -u_p.x, step(w.w, w.z)), 0.0), vec2(0.0, mix(u_p.y, -u_p.y, step(w.y, w.x))), isH);\n"
          "    vec3 c = texture(u_src, uv).rgb;\n"
          "    gl_FragColor = vec4(mix(c, texture(u_src, uv + off).rgb, mix(v, h, isH)), 1.0);\n"
          "}\n");
}

// ---- TAA resolve. Renders at the output size: the jittered current frame (internal size, HDR encoded) is sampled
// bilinearly where the unjittered pixel landed (u_j.xy), the history is fetched where the pixel was last frame
// (the depth gives the position, u_r0..3 = previous view-projection * inverse(current), no motion vectors:
// moving objects are left to the clamp), clamped to the 3x3 neighbourhood of the current frame, and blended.
// u_p: xy 1 / output size, zw 1 / internal size; u_j: xy jitter in uv, z far (m), w base blend;
// u_zp: x, y: ndc z = x + y / z (perspective), z: ortho flag, w: history valid; u_zq: m10, m14 (ortho).
void pp_taa_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform sampler2D u_curl;\n"
          "uniform sampler2D u_curn;\n"
          "uniform sampler2D u_depth;\n"
          "uniform sampler2D u_hist;\n"
          "uniform vec4 u_p;\n"
          "uniform vec4 u_j;\n"
          "uniform vec4 u_zp;\n"
          "uniform vec4 u_zq;\n"
          "uniform vec4 u_r0;\n"
          "uniform vec4 u_r1;\n"
          "uniform vec4 u_r2;\n"
          "uniform vec4 u_r3;\n");
    t.add(kPack);
    t.add("void main() {\n"
          "    vec2 uv = gl_FragCoord.xy * u_p.xy;\n"
          "    vec2 cuv = uv + u_j.xy;\n"
          "    vec2 t0 = (floor(cuv / u_p.zw) + 0.5) * u_p.zw;\n"
          "    vec3 c = texture(u_curl, cuv).rgb;\n"
          "    vec3 mn = c;\n"
          "    vec3 mx = c;\n");
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            t.add("    {\n        vec3 n = texture(u_curn, t0 + vec2(");
            t.flt((float)dx); t.add(", "); t.flt((float)dy);
            t.add(") * u_p.zw).rgb;\n        mn = min(mn, n);\n        mx = max(mx, n);\n    }\n");
        }
    t.add("    vec3 h = c;\n"
          "    float alpha = 1.0;\n"
          "    {\n"
          "        float zv = unpack24(texture(u_depth, t0).rgb) * u_j.z;\n"
          "        vec4 nd = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, mix(u_zp.x + u_zp.y / max(zv, 0.0001), u_zq.x * -zv + u_zq.y, u_zp.z), 1.0);\n"
          "        vec4 pc = u_r0 * nd.x + u_r1 * nd.y + u_r2 * nd.z + u_r3;\n"
          "        vec2 puv = vec2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);\n"
          "        float inside = step(0.0, puv.x) * step(puv.x, 1.0) * step(0.0, puv.y) * step(puv.y, 1.0) * u_zp.w;\n"
          "        vec3 hp = texture(u_hist, puv).rgb;\n"
          "        vec3 hc = clamp(hp, mn, mx);\n"
          "        alpha = mix(1.0, mix(u_j.w, 0.6, clamp(length(hp - hc) * 6.0, 0.0, 1.0)), inside);\n"
          "        h = hc;\n"
          "    }\n"
          "    gl_FragColor = vec4(mix(h, c, alpha), 1.0);\n"
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
