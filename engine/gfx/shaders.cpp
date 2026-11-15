#include "gfx/shaders.h"

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
};

// Declarations and the post processing chain shared by every fragment shader:
// exposure, tint, tonemap, contrast, saturation, vignette, linear to sRGB.
const char* const kPost =
    "uniform vec4 u_post;\n"
    "uniform vec4 u_tint;\n"
    "uniform vec4 u_res;\n"
    "vec3 lin2srgb(vec3 c) {\n"
    "    return mix(c * 12.92, 1.055 * pow(c, vec3(0.4166667)) - 0.055, step(0.0031308, c));\n"
    "}\n"
    "vec3 post(vec3 c) {\n"
    "    c = c * exp2(u_post.x) * u_tint.rgb;\n"
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
    "}\n";

const char* const kSrgb2Lin =
    "vec3 srgb2lin(vec3 c) {\n"
    "    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));\n"
    "}\n";

// The end of one light's contribution: L (to the light) and att are known.
const char* const kLightTail =
    "    float ndl = max(dot(N, L), 0.0);\n"
    "    vec3 H = normalize(L + V);\n"
    "    float sp = pow(max(dot(N, H), 0.0), shin) * ndl * att;\n"
    "    diff += u_l2[i].rgb * (ndl * att);\n"
    "    spec += u_l2[i].rgb * (sp * specn);\n"
    "}\n";

const char* const kPointHead =
    "    vec3 d = u_l0[i].xyz - v_pos;\n"
    "    float dist = length(d);\n"
    "    vec3 L = d / max(dist, 0.0001);\n"
    "    float x = min(dist / max(u_l0[i].w, 0.0001), 1.0);\n"
    "    float att = 1.0 - x * x;\n"
    "    att = att * att;\n";

}  // namespace

void lit_vertex(char* out, size_t cap) {
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
    t.add(kSrgb2Lin);
    t.add("void main() {\n"
          "    v_pos = (fgl_Model * vec4(position, 1.0)).xyz;\n"
          "    v_nrm = (fgl_NormalMatrix * vec4(normal, 0.0)).xyz;\n"
          "    v_alb = srgb2lin(u_mat.rgb);\n"
          "    gl_Position = fgl_ModelViewProjection * vec4(position, 1.0);\n"
          "}\n");
}

void lit_fragment(char* out, size_t cap, int nd, int np, int ns) {
    Text t(out, cap);
    int total = nd + np + ns;
    t.add("uniform vec4 u_mat;\n"
          "uniform vec4 u_cam;\n"
          "uniform vec4 u_sky;\n"
          "uniform vec4 u_gnd;\n");
    if (total > 0) {
        const char* names[4] = { "u_l0", "u_l1", "u_l2", "u_l3" };
        for (int k = 0; k < 4; k++) {
            t.add("uniform vec4 "); t.add(names[k]); t.add("["); t.num(total); t.add("];\n");
        }
    }
    t.add("varying vec3 v_pos;\n"
          "varying vec3 v_nrm;\n"
          "varying vec3 v_alb;\n");
    t.add(kPost);
    t.add("void main() {\n"
          "    vec3 N = normalize(v_nrm);\n"
          "    vec3 V = normalize(u_cam.xyz - v_pos);\n"
          "    float rough = clamp(u_mat.w, 0.04, 1.0);\n"
          "    float a2 = rough * rough * rough * rough;\n"
          "    float shin = clamp(2.0 / a2 - 2.0, 2.0, 512.0);\n"
          "    float specn = (shin + 8.0) * 0.03978874 * (0.04 + 0.3 * (1.0 - rough) * (1.0 - rough));\n"
          "    vec3 diff = vec3(0.0);\n"
          "    vec3 spec = vec3(0.0);\n");
    if (nd > 0) {
        t.add("    for (int i = 0; i < "); t.num(nd); t.add("; i++) {\n"
              "    vec3 L = -u_l1[i].xyz;\n"
              "    float att = 1.0;\n");
        t.add(kLightTail);
    }
    if (np > 0) {
        t.add("    for (int i = "); t.num(nd); t.add("; i < "); t.num(nd + np); t.add("; i++) {\n");
        t.add(kPointHead);
        t.add(kLightTail);
    }
    if (ns > 0) {
        t.add("    for (int i = "); t.num(nd + np); t.add("; i < "); t.num(total); t.add("; i++) {\n");
        t.add(kPointHead);
        t.add("    att = att * smoothstep(u_l3[i].x, u_l3[i].y, dot(-L, u_l1[i].xyz));\n");
        t.add(kLightTail);
    }
    t.add("    vec3 amb = mix(u_gnd.rgb, u_sky.rgb, N.y * 0.5 + 0.5);\n"
          "    gl_FragColor = vec4(post(v_alb * (amb + diff) + spec), 1.0);\n"
          "}\n");
}

void unlit_vertex(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform mat4 fgl_ModelViewProjection;\n"
          "uniform vec4 u_mat;\n"
          "attribute vec3 position;\n"
          "varying vec3 v_alb;\n");
    t.add(kSrgb2Lin);
    t.add("void main() {\n"
          "    v_alb = srgb2lin(u_mat.rgb);\n"
          "    gl_Position = fgl_ModelViewProjection * vec4(position, 1.0);\n"
          "}\n");
}

void unlit_fragment(char* out, size_t cap) {
    Text t(out, cap);
    t.add("varying vec3 v_alb;\n");
    t.add(kPost);
    t.add("void main() {\n"
          "    gl_FragColor = vec4(post(v_alb), 1.0);\n"
          "}\n");
}

void sky_vertex(char* out, size_t cap) {
    Text t(out, cap);
    t.add("attribute vec3 position;\n"
          "varying vec2 v_ndc;\n"
          "void main() {\n"
          "    v_ndc = position.xy;\n"
          "    gl_Position = vec4(position.xy, 0.999999, 1.0);\n"
          "}\n");
}

void sky_fragment(char* out, size_t cap) {
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
    t.add(kPost);
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
          "    col += u_suncol.rgb * (disc + halo) * (1.0 - u_zen.w);\n"
          "    gl_FragColor = vec4(post(col), 1.0);\n"
          "}\n");
}

}  // namespace fe::gfx::shaders
