#include "gfx/shaders.h"
#include "gfx/shaders_post.h"

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

// The post chain for this frame's choices (shaders_post.h), appended to `t`.
void add_post(Text& t, const PostKey& pk) {
    char buf[4096];
    emit_post_chain(buf, sizeof buf, pk);
    t.add(buf);
}

const char* const kSrgb2Lin =
    "vec3 srgb2lin(vec3 c) {\n"
    "    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));\n"
    "}\n";

// The end of one light's contribution: L (to the light) and att are known.
const char* const kLightTailDraft =
    "    float ndl = max(dot(N, L), 0.0);\n"
    "    diff += u_l2[i].rgb * (ndl * att);\n"
    "}\n";

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

size_t emit_gi_lookup(char* out, size_t cap) {
    Text t(out, cap);
    // Few registers (the GPU tier's lit shader is close to the limit): b = (x, y0, row of y0, fy), r, s, inv.
    t.add("uniform sampler2D u_gi;\n"
          "uniform vec4 u_gi0;\n"
          "uniform vec4 u_gi1;\n"
          "vec3 gi_amb(vec3 N, vec3 P) {\n"
          "    vec4 b = vec4(clamp((P - u_gi0.xyz) * u_gi0.w, vec3(0.5), u_gi1.xyz - 0.5), 0.0);\n"
          "    b.w = b.y - 0.5;\n"
          "    b.y = min(floor(b.w), u_gi1.y - 2.0);\n"
          "    b.w = b.w - b.y;\n"
          "    b.z = b.z + b.y * u_gi1.z;\n"
          "    vec2 inv = vec2(0.25 / u_gi1.x, 1.0 / (u_gi1.y * u_gi1.z));\n"
          "    vec3 s = mix(texture(u_gi, vec2(b.x, b.z) * inv).rgb, texture(u_gi, vec2(b.x, b.z + u_gi1.z) * inv).rgb, b.w);\n"
          "    vec3 r = s * s * 4.0;\n");
    static const char* const comp[3] = { "x", "y", "z" };
    for (int c = 1; c <= 3; c++) {
        t.add("    s = mix(texture(u_gi, vec2(b.x + ");
        t.add(c == 1 ? "u_gi1.x" : (c == 2 ? "2.0 * u_gi1.x" : "3.0 * u_gi1.x"));
        t.add(", b.z) * inv).rgb, texture(u_gi, vec2(b.x + ");
        t.add(c == 1 ? "u_gi1.x" : (c == 2 ? "2.0 * u_gi1.x" : "3.0 * u_gi1.x"));
        t.add(", b.z + u_gi1.z) * inv).rgb, b.w) * 2.0 - 1.0;\n"
              "    r += s * abs(s) * (2.0 * N.");
        t.add(comp[c - 1]);
        t.add(");\n");
    }
    t.add("    return max(r, vec3(0.0)) * u_gi1.w;\n"
          "}\n");
    return t.n;
}

void lit_vertex(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform mat4 fgl_ModelViewProjection;\n"
          "uniform mat4 fgl_Model;\n"
          "uniform mat4 fgl_NormalMatrix;\n"
          "uniform vec4 u_mat;\n"
          "attribute vec3 position;\n"
          "attribute vec3 normal;\n"
          "attribute vec2 texcoord;\n"
          "varying vec3 v_pos;\n"
          "varying vec3 v_nrm;\n"
          "varying vec3 v_alb;\n"
          "varying vec2 v_uv;\n");
    t.add(kSrgb2Lin);
    t.add("void main() {\n"
          "    v_pos = (fgl_Model * vec4(position, 1.0)).xyz;\n"
          "    v_nrm = (fgl_NormalMatrix * vec4(normal, 0.0)).xyz;\n"
          "    v_alb = srgb2lin(u_mat.rgb);\n"
          "    v_uv = texcoord;\n"
          "    gl_Position = fgl_ModelViewProjection * vec4(position, 1.0);\n"
          "}\n");
}

void lit_fragment(char* out, size_t cap, int nd, int np, int ns, const PostKey& pk, bool gi_on) {
    Text t(out, cap);
    const bool draft = pk.draft;
    const bool gi = gi_on && !draft;       // the draft shaders keep the hemisphere
    int total = nd + np + ns;
    t.add("uniform vec4 u_mat;\n"
          "uniform vec4 u_cam;\n"
          "uniform vec4 u_sky;\n"
          "uniform vec4 u_gnd;\n"
          "uniform sampler2D u_texture;\n");
    if (total > 0) {
        const char* names[4] = { "u_l0", "u_l1", "u_l2", "u_l3" };
        for (int k = 0; k < 4; k++) {
            t.add("uniform vec4 "); t.add(names[k]); t.add("["); t.num(total); t.add("];\n");
        }
    }
    t.add("varying vec3 v_pos;\n"
          "varying vec3 v_nrm;\n"
          "varying vec3 v_alb;\n"
          "varying vec2 v_uv;\n");
    t.add(kSrgb2Lin);
    if (gi) {
        char buf[2048];
        emit_gi_lookup(buf, sizeof buf);
        t.add(buf);
    }
    add_post(t, pk);
    const char* tail = draft ? kLightTailDraft : kLightTail;
    t.add("void main() {\n"
          "    vec3 N = normalize(v_nrm);\n");
    if (!draft)
        t.add("    vec3 V = normalize(u_cam.xyz - v_pos);\n"
              "    float rough = clamp(u_mat.w, 0.04, 1.0);\n"
              "    float a2 = rough * rough * rough * rough;\n"
              "    float shin = clamp(2.0 / a2 - 2.0, 2.0, 512.0);\n"
              "    float specn = (shin + 8.0) * 0.03978874 * (0.04 + 0.3 * (1.0 - rough) * (1.0 - rough));\n"
              "    vec3 spec = vec3(0.0);\n");
    t.add("    vec3 diff = vec3(0.0);\n");
    if (nd > 0) {
        t.add("    for (int i = 0; i < "); t.num(nd); t.add("; i++) {\n"
              "    vec3 L = -u_l1[i].xyz;\n"
              "    float att = 1.0;\n");
        t.add(tail);
    }
    if (np > 0) {
        t.add("    for (int i = "); t.num(nd); t.add("; i < "); t.num(nd + np); t.add("; i++) {\n");
        t.add(kPointHead);
        t.add(tail);
    }
    if (ns > 0) {
        t.add("    for (int i = "); t.num(nd + np); t.add("; i < "); t.num(total); t.add("; i++) {\n");
        t.add(kPointHead);
        t.add("    att = att * smoothstep(u_l3[i].x, u_l3[i].y, dot(-L, u_l1[i].xyz));\n");
        t.add(tail);
    }
    t.add(gi ? "    vec3 amb = gi_amb(N, v_pos);\n" : "    vec3 amb = mix(u_gnd.rgb, u_sky.rgb, N.y * 0.5 + 0.5);\n");
    t.add("    vec3 albedo = v_alb * srgb2lin(texture2D(u_texture, v_uv).rgb);\n"
          "    vec3 lit = albedo * (amb + diff);\n");
    t.add(draft ? "    gl_FragColor = vec4(post(lit), 1.0);\n}\n" : "    gl_FragColor = vec4(post(lit + spec), 1.0);\n}\n");
}

void unlit_vertex(char* out, size_t cap) {
    Text t(out, cap);
    t.add("uniform mat4 fgl_ModelViewProjection;\n"
          "uniform vec4 u_mat;\n"
          "attribute vec3 position;\n"
          "attribute vec2 texcoord;\n"
          "varying vec3 v_alb;\n"
          "varying vec2 v_uv;\n");
    t.add(kSrgb2Lin);
    t.add("void main() {\n"
          "    v_alb = srgb2lin(u_mat.rgb);\n"
          "    v_uv = texcoord;\n"
          "    gl_Position = fgl_ModelViewProjection * vec4(position, 1.0);\n"
          "}\n");
}

void unlit_fragment(char* out, size_t cap, const PostKey& pk) {
    Text t(out, cap);
    t.add("uniform sampler2D u_texture;\n"
          "varying vec3 v_alb;\n"
          "varying vec2 v_uv;\n");
    t.add(kSrgb2Lin);
    add_post(t, pk);
    t.add("void main() {\n"
          "    vec3 albedo = v_alb * srgb2lin(texture2D(u_texture, v_uv).rgb);\n"
          "    gl_FragColor = vec4(post(albedo), 1.0);\n"
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

void sky_fragment(char* out, size_t cap, const PostKey& pk) {
    Text t(out, cap);
    const bool draft = pk.draft;
    t.add("uniform vec4 u_fwd;\n"
          "uniform vec4 u_rx;\n"
          "uniform vec4 u_ry;\n"
          "uniform vec4 u_zen;\n"
          "uniform vec4 u_hor;\n"
          "uniform vec4 u_grd;\n"
          "uniform vec4 u_sun;\n"
          "uniform vec4 u_suncol;\n"
          "varying vec2 v_ndc;\n");
    add_post(t, pk);
    t.add("void main() {\n"
          "    vec3 d = normalize(u_fwd.xyz + u_rx.xyz * v_ndc.x + u_ry.xyz * v_ndc.y);\n"
          "    float up = clamp(d.y, 0.0, 1.0);\n"
          "    vec3 above = mix(u_hor.rgb, u_zen.rgb, sqrt(up));\n"
          "    vec3 col = mix(above, u_grd.rgb, smoothstep(0.0, 0.12, -d.y));\n"
          "    col = mix(col, u_zen.rgb, u_zen.w);\n");
    if (!draft)
        t.add("    float c = dot(d, u_sun.xyz);\n"
              "    float core = u_sun.w;\n"
              "    float rim = 1.0 - (1.0 - core) * 1.6;\n"
              "    float disc = smoothstep(rim, core, c) * step(core, 1.0);\n"
              "    float halo = pow(max(c, 0.0), 256.0) * 0.15 * step(core, 1.0);\n"
              "    col += u_suncol.rgb * (disc + halo) * (1.0 - u_zen.w);\n");
    t.add("    gl_FragColor = vec4(post(col), 1.0);\n"
          "}\n");
}

}  // namespace fe::gfx::shaders
