#include "view3d.h"

#include <stdlib.h>
#include <string.h>

namespace fe {

namespace {

constexpr float kNearW = 0.05f;     // clip triangles and lines at w = kNearW

// Lighting, in linear color.
const Vec3 kSky(0.25f, 0.28f, 0.35f);       // ambient from above
const Vec3 kGround(0.09f, 0.085f, 0.08f);   // ambient from below
const Vec3 kSunColor(1.20f, 1.14f, 1.05f);
const float kExposure = 0.78f;

inline Vec4 lerp4(const Vec4& a, const Vec4& b, float t) {
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
}
inline Vec3 lerp3(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline float ffloor(float x) { float t = (float)(int)x; return t > x ? t - 1.0f : t; }

// Linear light to 8-bit display values: a filmic curve (Narkowicz's ACES
// fit) and gamma 2 (a square root), as one table over [0, 4).
constexpr int kEncodeSize = 4096;
uint8_t g_encode[kEncodeSize];
bool g_encode_ready = false;
void init_encode() {
    if (g_encode_ready) return;
    for (int i = 0; i < kEncodeSize; i++) {
        float v = (i + 0.5f) / 1024.0f * kExposure;
        float t = (v * (2.51f * v + 0.03f)) / (v * (2.43f * v + 0.59f) + 0.14f);
        g_encode[i] = (uint8_t)(fsqrt(fclamp(t, 0.0f, 1.0f)) * 255.0f + 0.5f);
    }
    g_encode_ready = true;
}
inline uint32_t encode(float v) {
    int i = (int)(v * 1024.0f);
    return g_encode[i < 0 ? 0 : (i >= kEncodeSize ? kEncodeSize - 1 : i)];
}

// 1/sqrt(x) for lighting: the SSE estimate and one Newton step.
inline float frsqrt(float x) {
#if defined(__SSE__)
    typedef float v4sf __attribute__((vector_size(16)));
    v4sf r = __builtin_ia32_rsqrtss((v4sf){ x, 0, 0, 0 });
    float y = r[0];
    return y * (1.5f - 0.5f * x * y * y);
#else
    return 1.0f / fsqrt(x);
#endif
}

inline uint32_t pack(uint32_t r, uint32_t g, uint32_t b) { return r | g << 8 | b << 16 | 0xFF000000u; }

// sRGB 0..1 to linear (gamma 2, matching the encode table).
inline Vec3 to_linear(const Vec3& c) { return mul(c, c); }

uint8_t g_selmark[65536];

}  // namespace

View3D::~View3D() {
    free(color_); free(depth_); free(ids_); free(shadow_);
    free(out_); free(mask_); free(tmp_);
}

void View3D::ensure(int iw, int ih, int ow, int oh) {
    if (iw * ih > icap_) {
        free(color_); free(depth_); free(ids_); free(shadow_);
        icap_ = iw * ih;
        color_ = (uint32_t*)malloc((size_t)icap_ * 4);
        depth_ = (float*)malloc((size_t)icap_ * sizeof(float));
        ids_ = (uint16_t*)malloc((size_t)icap_ * sizeof(uint16_t));
        shadow_ = (uint8_t*)malloc((size_t)icap_ * 2);      // the mask and a blur row buffer
    }
    if (ow * oh > ocap_) {
        free(out_); free(mask_); free(tmp_);
        ocap_ = ow * oh;
        out_ = (uint32_t*)malloc((size_t)ocap_ * 4);
        mask_ = (uint8_t*)malloc((size_t)ocap_);
        tmp_ = (uint8_t*)malloc((size_t)ocap_ * 2);
    }
}

void View3D::begin(int w, int h, const Mat4& view, const Mat4& proj, uint32_t top, uint32_t bottom, int supersample) {
    init_encode();
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    ss_ = supersample >= 2 ? 2 : 1;
    w_ = w; h_ = h;
    iw_ = w * ss_; ih_ = h * ss_;
    ensure(iw_, ih_, w_, h_);
    view_ = view;
    vp_ = proj * view;
    Mat4 inv;
    invert(view, &inv);
    eye_ = transform_point(inv, Vec3(0, 0, 0));
    sun_n_ = normalize(sun);
    triangles_ = 0;
    any_shadow_ = false;
    shx0_ = sly0_ = slx0_ = shy0_ = 1 << 30;
    shx1_ = shy1_ = slx1_ = sly1_ = -1;
    sel_count_ = 0;
    sel_active_ = 0;
    int tr = (top >> 16) & 0xFF, tg = (top >> 8) & 0xFF, tb = top & 0xFF;
    int br = (bottom >> 16) & 0xFF, bg = (bottom >> 8) & 0xFF, bb = bottom & 0xFF;
    for (int y = 0; y < ih_; y++) {
        int k = ih_ > 1 ? y * 256 / (ih_ - 1) : 0;
        uint32_t px = pack((uint32_t)(tr + ((br - tr) * k >> 8)), (uint32_t)(tg + ((bg - tg) * k >> 8)), (uint32_t)(tb + ((bb - tb) * k >> 8)));
        uint32_t* row = color_ + (size_t)y * iw_;
        float* drow = depth_ + (size_t)y * iw_;
        for (int x = 0; x < iw_; x++) { row[x] = px; drow[x] = 1.0f; }
    }
    memset(ids_, 0, (size_t)iw_ * ih_ * sizeof(uint16_t));
    memset(shadow_, 0, (size_t)iw_ * ih_);
    tex_.format = SwTexture::RGBA8;
    tex_.width = w_;
    tex_.height = h_;
    tex_.pitch = w_;
    tex_.pixels = (uint8_t*)out_;
    tex_.linear = false;
}

// ---------------------------------------------------------------- meshes

void View3D::select(const uint16_t* ids, int count, uint16_t active) {
    sel_count_ = 0;
    for (int i = 0; i < count && sel_count_ < 256; i++)
        if (ids[i]) { sel_[sel_count_++] = ids[i]; g_selmark[ids[i]] = 1; }
    sel_active_ = active;
    if (active) g_selmark[active] = 2;
}

uint32_t View3D::shade(const Vec3& albedo, const Vec3& nn, const Vec3& p) const {
    float nl = dot(nn, nn);
    if (nl <= 0) return pack(0, 0, 0);
    Vec3 n = nn * frsqrt(nl);
    Vec3 v = eye_ - p;
    float vl = dot(v, v);
    v = vl > 0 ? v * frsqrt(vl) : Vec3(0, 1, 0);
    // Sky above, ground below, and a sun with a soft terminator.
    Vec3 amb = lerp3(kGround, kSky, n.y * 0.5f + 0.5f);
    float ndl = (dot(n, sun_n_) + 0.08f) * (1.0f / 1.08f);
    if (ndl < 0) ndl = 0;
    Vec3 c = mul(albedo, amb + kSunColor * ndl);
    // Highlight (Blinn-Phong, exponent 64 by squaring) and a rim from the sky.
    Vec3 hv = sun_n_ + v;
    float hl = dot(hv, hv);
    if (hl > 0 && ndl > 0) {
        float s = dot(n, hv) * frsqrt(hl);
        if (s > 0) {
            s *= s; s *= s; s *= s; s *= s; s *= s; s *= s;
            c += kSunColor * (0.22f * s);
        }
    }
    float rim = 1.0f - fclamp(dot(n, v), 0.0f, 1.0f);
    rim *= rim; rim *= rim;
    c += kSky * (0.35f * rim);
    return pack(encode(c.x), encode(c.y), encode(c.z));
}

void View3D::mesh(const Vec3* pos, const Vec3* nrm, uint32_t count, const Mat4& model, const Vec3& color, uint16_t id, bool unlit) {
    // Normals go through the cofactor of the model's 3x3 part (the inverse
    // transpose up to scale), flipped for mirroring transforms.
    Vec3 a0(model.m[0], model.m[1], model.m[2]), a1(model.m[4], model.m[5], model.m[6]), a2(model.m[8], model.m[9], model.m[10]);
    Vec3 c0 = cross(a1, a2), c1 = cross(a2, a0), c2 = cross(a0, a1);
    float det = dot(a0, c0);
    if (det < 0) { c0 = c0 * -1.0f; c1 = c1 * -1.0f; c2 = c2 * -1.0f; }
    Vec3 albedo = to_linear(Vec3(fclamp(color.x, 0, 1), fclamp(color.y, 0, 1), fclamp(color.z, 0, 1)));
    auto ch = [](float v) { return (uint32_t)(fclamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    uint32_t unlit_px = pack(ch(color.x), ch(color.y), ch(color.z));
    for (uint32_t t = 0; t + 2 < count; t += 3) {
        CV v[3];
        for (int i = 0; i < 3; i++) {
            Vec3 p = transform_point(model, pos[t + i]);
            const Vec3& n = nrm[t + i];
            v[i].p = p;
            v[i].n = c0 * n.x + c1 * n.y + c2 * n.z;
            v[i].c = vp_ * Vec4{ p.x, p.y, p.z, 1 };
        }
        triangles_++;
        clip_and_raster(v, albedo, unlit_px, unlit, id);
    }
}

void View3D::clip_and_raster(const CV* in, const Vec3& albedo, uint32_t unlit_px, bool unlit, uint16_t id) {
    // Clip against the near plane (w > kNearW): up to 4 vertices, drawn as a fan.
    CV out[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const CV& p = in[i];
        const CV& q = in[(i + 1) % 3];
        bool pin = p.c.w > kNearW, qin = q.c.w > kNearW;
        if (pin) out[n++] = p;
        if (pin != qin) {
            float t = (kNearW - p.c.w) / (q.c.w - p.c.w);
            out[n++] = { lerp4(p.c, q.c, t), lerp3(p.n, q.n, t), lerp3(p.p, q.p, t) };
        }
    }
    if (n < 3) return;
    raster(out, albedo, unlit_px, unlit, id);
    if (n == 4) {
        CV tri2[3] = { out[0], out[2], out[3] };
        raster(tri2, albedo, unlit_px, unlit, id);
    }
}

void View3D::raster(const CV* v, const Vec3& albedo, uint32_t unlit_px, bool unlit, uint16_t id) {
    float sx[3], sy[3], sz[3], iw[3];
    for (int i = 0; i < 3; i++) {
        iw[i] = 1.0f / v[i].c.w;
        sx[i] = (v[i].c.x * iw[i] * 0.5f + 0.5f) * iw_;
        sy[i] = (0.5f - v[i].c.y * iw[i] * 0.5f) * ih_;
        sz[i] = v[i].c.z * iw[i] * 0.5f + 0.5f;
    }
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
    if (area >= 0) return;      // back face (counter-clockwise in y-up becomes clockwise here)
    int minx = imax(0, (int)fminf_(sx[0], fminf_(sx[1], sx[2])));
    int maxx = imin(iw_ - 1, (int)fmaxf_(sx[0], fmaxf_(sx[1], sx[2])) + 1);
    int miny = imax(0, (int)fminf_(sy[0], fminf_(sy[1], sy[2])));
    int maxy = imin(ih_ - 1, (int)fmaxf_(sy[0], fmaxf_(sy[1], sy[2])) + 1);
    if (minx > maxx || miny > maxy) return;
    float inv = 1.0f / area;
    if (g_selmark[id]) {
        if (minx < slx0_) slx0_ = minx;
        if (miny < sly0_) sly0_ = miny;
        if (maxx > slx1_) slx1_ = maxx;
        if (maxy > sly1_) sly1_ = maxy;
    }
    // Attributes divided by w, for perspective-correct interpolation.
    Vec3 nw[3], pw[3];
    for (int i = 0; i < 3; i++) { nw[i] = v[i].n * iw[i]; pw[i] = v[i].p * iw[i]; }
    // Edge i is opposite vertex i; with area < 0 the inside has all <= 0.
    float ex[3] = { -(sy[2] - sy[1]), -(sy[0] - sy[2]), -(sy[1] - sy[0]) };
    for (int y = miny; y <= maxy; y++) {
        float py = y + 0.5f, px0 = minx + 0.5f;
        float w0 = (sx[2] - sx[1]) * (py - sy[1]) - (sy[2] - sy[1]) * (px0 - sx[1]);
        float w1 = (sx[0] - sx[2]) * (py - sy[2]) - (sy[0] - sy[2]) * (px0 - sx[2]);
        float w2 = (sx[1] - sx[0]) * (py - sy[0]) - (sy[1] - sy[0]) * (px0 - sx[0]);
        uint32_t* row = color_ + (size_t)y * iw_;
        float* drow = depth_ + (size_t)y * iw_;
        uint16_t* irow = ids_ + (size_t)y * iw_;
        for (int x = minx; x <= maxx; x++, w0 += ex[0], w1 += ex[1], w2 += ex[2]) {
            if (w0 > 0 || w1 > 0 || w2 > 0) continue;
            float l0 = w0 * inv, l1 = w1 * inv, l2 = w2 * inv;
            float z = l0 * sz[0] + l1 * sz[1] + l2 * sz[2];
            if (z < 0 || z >= drow[x]) continue;
            drow[x] = z;
            irow[x] = id;
            if (unlit) { row[x] = unlit_px; continue; }
            float r = 1.0f / (l0 * iw[0] + l1 * iw[1] + l2 * iw[2]);
            Vec3 n = (nw[0] * l0 + nw[1] * l1 + nw[2] * l2) * r;
            Vec3 p = (pw[0] * l0 + pw[1] * l1 + pw[2] * l2) * r;
            row[x] = shade(albedo, n, p);
        }
    }
}

// ---------------------------------------------------------------- shadows

void View3D::shadow(const Vec3* pos, uint32_t count, const Mat4& model) {
    if (sun_n_.y < 0.05f) return;
    for (uint32_t t = 0; t + 2 < count; t += 3) {
        Vec4 c[3];
        float strength[3];
        for (int i = 0; i < 3; i++) {
            Vec3 p = transform_point(model, pos[t + i]);
            float hgt = p.y > 0 ? p.y : 0;
            // Along the sun down to the ground; higher parts cast lighter shadows.
            Vec3 g = p - sun_n_ * (hgt / sun_n_.y);
            g.y = 0;
            strength[i] = 1.0f - fclamp(hgt * 0.16f, 0.0f, 0.6f);
            c[i] = vp_ * Vec4{ g.x, g.y, g.z, 1 };
        }
        // Clip to the near plane (rare for ground shadows): drop the triangle.
        if (c[0].w <= kNearW || c[1].w <= kNearW || c[2].w <= kNearW) continue;
        raster_shadow(c, strength);
    }
}

void View3D::raster_shadow(const Vec4* v, const float* strength) {
    float sx[3], sy[3], sz[3], st[3];
    for (int i = 0; i < 3; i++) {
        float iw = 1.0f / v[i].w;
        sx[i] = (v[i].x * iw * 0.5f + 0.5f) * iw_;
        sy[i] = (0.5f - v[i].y * iw * 0.5f) * ih_;
        sz[i] = v[i].z * iw * 0.5f + 0.5f;
        st[i] = strength[i];
    }
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
    if (area == 0) return;
    if (area > 0) {
        // Projected shadows come in either winding: make it negative.
        float t;
        t = sx[1]; sx[1] = sx[2]; sx[2] = t;
        t = sy[1]; sy[1] = sy[2]; sy[2] = t;
        t = sz[1]; sz[1] = sz[2]; sz[2] = t;
        t = st[1]; st[1] = st[2]; st[2] = t;
        area = -area;
    }
    int minx = imax(0, (int)fminf_(sx[0], fminf_(sx[1], sx[2])));
    int maxx = imin(iw_ - 1, (int)fmaxf_(sx[0], fmaxf_(sx[1], sx[2])) + 1);
    int miny = imax(0, (int)fminf_(sy[0], fminf_(sy[1], sy[2])));
    int maxy = imin(ih_ - 1, (int)fmaxf_(sy[0], fmaxf_(sy[1], sy[2])) + 1);
    if (minx > maxx || miny > maxy) return;
    float inv = 1.0f / area;
    if (minx < shx0_) shx0_ = minx;
    if (miny < shy0_) shy0_ = miny;
    if (maxx > shx1_) shx1_ = maxx;
    if (maxy > shy1_) shy1_ = maxy;
    float ex[3] = { -(sy[2] - sy[1]), -(sy[0] - sy[2]), -(sy[1] - sy[0]) };
    for (int y = miny; y <= maxy; y++) {
        float py = y + 0.5f, px0 = minx + 0.5f;
        float w0 = (sx[2] - sx[1]) * (py - sy[1]) - (sy[2] - sy[1]) * (px0 - sx[1]);
        float w1 = (sx[0] - sx[2]) * (py - sy[2]) - (sy[0] - sy[2]) * (px0 - sx[2]);
        float w2 = (sx[1] - sx[0]) * (py - sy[0]) - (sy[1] - sy[0]) * (px0 - sx[0]);
        for (int x = minx; x <= maxx; x++, w0 += ex[0], w1 += ex[1], w2 += ex[2]) {
            if (w0 > 0 || w1 > 0 || w2 > 0) continue;
            float l0 = w0 * inv, l1 = w1 * inv, l2 = w2 * inv;
            float z = l0 * sz[0] + l1 * sz[1] + l2 * sz[2];
            size_t k = (size_t)y * iw_ + x;
            float d = depth_[k];
            // Only on what is at ground depth (or the empty background).
            if (z > d + (1.0f - d) * 0.005f + 1e-6f) continue;
            uint8_t s8 = (uint8_t)(fclamp(l0 * st[0] + l1 * st[1] + l2 * st[2], 0.0f, 1.0f) * 255.0f);
            if (s8 > shadow_[k]) shadow_[k] = s8;
            any_shadow_ = true;
        }
    }
}

// ---------------------------------------------------------------- grid

void View3D::grid(float spacing, bool dark) {
    // Evaluated once per output pixel (it is anti-aliased analytically) and
    // applied to each of its samples that the ground point is in front of.
    if (spacing <= 0) return;
    float ey = eye_.y;
    if (ey > -1e-4f && ey < 1e-4f) return;
    Mat4 inv;
    if (!invert(vp_, &inv)) return;
    // Far-plane points through output pixel (x, y), homogeneous, are linear:
    // F = B + x Cx + y Cy. The ground hit is eye + (F/F.w - eye) t with
    // t = -eye.y / (F.y/F.w - eye.y), so hit.xz = eye.xz + (F.xz - eye.xz F.w) / (eye.y F.w - F.y) * eye.y.
    const float* q = inv.m;
    float sx = 2.0f / w_, sy = -2.0f / h_;
    float ox = -1.0f + 0.5f * sx, oy = 1.0f + 0.5f * sy;
    Vec4 Cx{ q[0] * sx, q[1] * sx, q[2] * sx, q[3] * sx };
    Vec4 Cy{ q[4] * sy, q[5] * sy, q[6] * sy, q[7] * sy };
    Vec4 B{ q[12] + q[8] + q[0] * ox + q[4] * oy, q[13] + q[9] + q[1] * ox + q[5] * oy, q[14] + q[10] + q[2] * ox + q[6] * oy,
            q[15] + q[11] + q[3] * ox + q[7] * oy };
    // Numerators and denominator, each linear in (x, y).
    auto lin = [&](float bx, float cx, float cy, float bw, float cxw, float cyw, float e, Vec3* out) {
        *out = Vec3((bx - e * bw), (cx - e * cxw), (cy - e * cyw));
    };
    Vec3 nx3, nz3, d3;
    lin(B.x, Cx.x, Cy.x, B.w, Cx.w, Cy.w, eye_.x, &nx3);
    lin(B.z, Cx.z, Cy.z, B.w, Cx.w, Cy.w, eye_.z, &nz3);
    d3 = Vec3(ey * B.w - B.y, ey * Cx.w - Cx.y, ey * Cy.w - Cy.y);
    const float* m = vp_.m;
    float hgt = ey < 0 ? -ey : ey;
    float fade_end = hgt * 16.0f + spacing * 12.0f;
    float inv_fade2 = 1.0f / (fade_end * fade_end);
    int base = dark ? 255 : 0;
    float a_minor = dark ? 0.075f : 0.09f, a_major = dark ? 0.16f : 0.17f;
    const int axr = 229, axg = 83, axb = 75;        // x axis (z = 0)
    const int azr = 76, azg = 141, azb = 255;       // z axis (x = 0)
    float inv_g = 1.0f / spacing, inv_g10 = inv_g * 0.1f;
    for (int y = 0; y < h_; y++) {
        float fy = (float)y;
        float rnx = nx3.x + nx3.z * fy, rnz = nz3.x + nz3.z * fy, rd = d3.x + d3.z * fy;
        for (int x = 0; x < w_; x++) {
            float fx_ = (float)x;
            float den = rd + d3.y * fx_;
            if (den == 0) continue;
            float id = 1.0f / den;
            float t = ey * id;                  // ray parameter scale; must be positive
            if (t <= 0 && ey > 0) continue;
            if (t >= 0 && ey < 0) continue;
            float numx = rnx + nx3.y * fx_, numz = rnz + nz3.y * fx_;
            float wx = eye_.x + numx * t, wz = eye_.z + numz * t;
            // Fade with distance from the camera.
            float ddx = wx - eye_.x, ddz = wz - eye_.z;
            float fade = 1.0f - (ddx * ddx + ddz * ddz) * inv_fade2;
            if (fade <= 0) continue;
            fade *= fade;
            // Screen-space derivatives of the hit (quotient rule): world units per pixel.
            float ddenx = d3.y, ddeny = d3.z;
            float dwx_dx = (nx3.y - numx * id * ddenx) * t, dwx_dy = (nx3.z - numx * id * ddeny) * t;
            float dwz_dx = (nz3.y - numz * id * ddenx) * t, dwz_dy = (nz3.z - numz * id * ddeny) * t;
            float fx = fabsf_(dwx_dx) + fabsf_(dwx_dy), fz = fabsf_(dwz_dx) + fabsf_(dwz_dy);
            if (fx <= 0 || fz <= 0) continue;
            // Depth of the ground point (clip space; compared without dividing).
            float cz = m[2] * wx + m[10] * wz + m[14], cw = m[3] * wx + m[11] * wz + m[15];
            if (cw <= 0) continue;
            float ifx = 1.0f / fx, ifz = 1.0f / fz;
            float alpha = 0;
            for (int level = 0; level < 2; level++) {
                float ig = level ? inv_g10 : inv_g, gs = level ? spacing * 10.0f : spacing;
                // Pixels per grid cell along x and z; lines fade out below 3 px apart.
                float px = ifx * gs, pz = ifz * gs;
                float pmin = px < pz ? px : pz;
                float lod = (pmin - 3.0f) * 0.2f;
                if (lod <= 0) continue;
                if (lod > 1) lod = 1;
                float u = wx * ig, v = wz * ig;
                float fu = fabsf_(u - ffloor(u + 0.5f)) * px, fv = fabsf_(v - ffloor(v + 0.5f)) * pz;   // pixels from the line
                float cu = 1.0f - (fu < 1.0f ? fu : 1.0f), cv = 1.0f - (fv < 1.0f ? fv : 1.0f);
                float c = (cu > cv ? cu : cv) * lod * (level ? a_major : a_minor);
                if (c > alpha) alpha = c;
            }
            float ax = 1.0f - fclamp(fabsf_(wz) * ifz * 0.8f, 0.0f, 1.0f);
            float az = 1.0f - fclamp(fabsf_(wx) * ifx * 0.8f, 0.0f, 1.0f);
            if (alpha <= 0 && ax <= 0 && az <= 0) continue;
            int ai = (int)(alpha * fade * 256.0f), xi = (int)(ax * 0.85f * fade * 256.0f), zi = (int)(az * 0.85f * fade * 256.0f);
            for (int sy2 = 0; sy2 < ss_; sy2++) {
                uint32_t* crow = color_ + (size_t)(y * ss_ + sy2) * iw_ + x * ss_;
                const float* drow = depth_ + (size_t)(y * ss_ + sy2) * iw_ + x * ss_;
                for (int sx2 = 0; sx2 < ss_; sx2++) {
                    float d = drow[sx2];
                    float thr = d + (1.0f - d) * 0.005f + 1e-6f;      // in [0, 1] depth
                    if (cz > (2.0f * thr - 1.0f) * cw) continue;
                    uint32_t pxl = crow[sx2];
                    int r = pxl & 0xFF, g = (pxl >> 8) & 0xFF, b = (pxl >> 16) & 0xFF;
                    r += (base - r) * ai >> 8; g += (base - g) * ai >> 8; b += (base - b) * ai >> 8;
                    if (xi > 0) { r += (axr - r) * xi >> 8; g += (axg - g) * xi >> 8; b += (axb - b) * xi >> 8; }
                    if (zi > 0) { r += (azr - r) * zi >> 8; g += (azg - g) * zi >> 8; b += (azb - b) * zi >> 8; }
                    crow[sx2] = pack((uint32_t)r, (uint32_t)g, (uint32_t)b);
                }
            }
        }
    }
}

// ---------------------------------------------------------------- lines

void View3D::line(const Vec3& a, const Vec3& b, uint32_t color, int alpha) {
    Vec4 p = vp_ * Vec4{ a.x, a.y, a.z, 1 }, q = vp_ * Vec4{ b.x, b.y, b.z, 1 };
    if (p.w <= kNearW && q.w <= kNearW) return;
    if (p.w <= kNearW) p = lerp4(p, q, (kNearW - p.w) / (q.w - p.w));
    else if (q.w <= kNearW) q = lerp4(q, p, (kNearW - q.w) / (p.w - q.w));
    float x0 = (p.x / p.w * 0.5f + 0.5f) * iw_, y0 = (0.5f - p.y / p.w * 0.5f) * ih_, z0 = p.z / p.w * 0.5f + 0.5f;
    float x1 = (q.x / q.w * 0.5f + 0.5f) * iw_, y1 = (0.5f - q.y / q.w * 0.5f) * ih_, z1 = q.z / q.w * 0.5f + 0.5f;
    // Clip to the image (Liang-Barsky), so a line passing close to the
    // camera does not step across thousands of off-screen pixels.
    float t0 = 0.0f, t1 = 1.0f, dx = x1 - x0, dy = y1 - y0;
    const float pv[4] = { -dx, dx, -dy, dy };
    const float qv[4] = { x0, (float)iw_ - 1.0f - x0, y0, (float)ih_ - 1.0f - y0 };
    for (int i = 0; i < 4; i++) {
        if (pv[i] == 0.0f) { if (qv[i] < 0) return; continue; }
        float r = qv[i] / pv[i];
        if (pv[i] < 0) { if (r > t1) return; if (r > t0) t0 = r; }
        else { if (r < t0) return; if (r < t1) t1 = r; }
    }
    float cx0 = x0 + dx * t0, cy0 = y0 + dy * t0, cz0 = z0 + (z1 - z0) * t0;
    float cx1 = x0 + dx * t1, cy1 = y0 + dy * t1, cz1 = z0 + (z1 - z0) * t1;
    dx = cx1 - cx0;
    dy = cy1 - cy0;
    float steps = fmaxf_(fabsf_(dx), fabsf_(dy));
    if (steps < 1) steps = 1;
    int n = (int)steps;
    float ix = dx / steps, iy = dy / steps, iz = (cz1 - cz0) / steps;
    int cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;
    float x = cx0 + 0.5f, y = cy0 + 0.5f, z = cz0;
    for (int i = 0; i <= n; i++, x += ix, y += iy, z += iz) {
        for (int oy = 0; oy < ss_; oy++)
            for (int ox = 0; ox < ss_; ox++) {
                int xi = (int)x + ox, yi = (int)y + oy;
                if (xi < 0 || yi < 0 || xi >= iw_ || yi >= ih_) continue;
                size_t k = (size_t)yi * iw_ + xi;
                // Behind something; the bias lets lines lying on a surface show.
                if (z > depth_[k] + 2e-4f) continue;
                uint32_t px = color_[k];
                int r = px & 0xFF, g = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF;
                r += (cr - r) * alpha >> 8; g += (cg - g) * alpha >> 8; b += (cb - b) * alpha >> 8;
                color_[k] = pack((uint32_t)r, (uint32_t)g, (uint32_t)b);
            }
    }
}

// ---------------------------------------------------------------- finish

void View3D::finish(uint32_t active_color, uint32_t other_color, float shadow_strength) {
    // Shadows: a 3x3 blur of the mask, then darken (inside their box only).
    if (any_shadow_ && shadow_strength > 0) {
        int x0 = imax(0, shx0_ - 1), x1 = imin(iw_ - 1, shx1_ + 1), y0 = imax(0, shy0_ - 1), y1 = imin(ih_ - 1, shy1_ + 1);
        uint8_t* blur = shadow_ + (size_t)iw_ * ih_;
        for (int y = y0; y <= y1; y++) {
            const uint8_t* s = shadow_ + (size_t)y * iw_;
            uint8_t* d = blur + (size_t)y * iw_;
            for (int x = x0; x <= x1; x++) {
                int a = s[x] * 2 + (x > 0 ? s[x - 1] : s[x]) + (x + 1 < iw_ ? s[x + 1] : s[x]);
                d[x] = (uint8_t)(a >> 2);
            }
        }
        int k = (int)(fclamp(shadow_strength, 0.0f, 1.0f) * 256.0f);
        for (int y = y0; y <= y1; y++) {
            const uint8_t* up = blur + (size_t)(y > y0 ? y - 1 : y) * iw_;
            const uint8_t* mid = blur + (size_t)y * iw_;
            const uint8_t* dn = blur + (size_t)(y < y1 ? y + 1 : y) * iw_;
            uint32_t* row = color_ + (size_t)y * iw_;
            for (int x = x0; x <= x1; x++) {
                int s = (up[x] + mid[x] * 2 + dn[x]) >> 2;
                if (!s) continue;
                int f = 256 - (s * k >> 8);
                uint32_t px = row[x];
                row[x] = pack((uint32_t)(((px & 0xFF) * f) >> 8), (uint32_t)((((px >> 8) & 0xFF) * f) >> 8), (uint32_t)((((px >> 16) & 0xFF) * f) >> 8));
            }
        }
    }

    // Resolve the supersampling.
    if (ss_ == 1) {
        memcpy(out_, color_, (size_t)w_ * h_ * 4);
    } else {
        for (int y = 0; y < h_; y++) {
            const uint32_t* a = color_ + (size_t)(y * 2) * iw_;
            const uint32_t* b = a + iw_;
            uint32_t* o = out_ + (size_t)y * w_;
            for (int x = 0; x < w_; x++) {
                uint32_t p0 = a[2 * x], p1 = a[2 * x + 1], p2 = b[2 * x], p3 = b[2 * x + 1];
                // Average two channels at a time (R and B, then G).
                uint32_t rb = ((p0 & 0xFF00FF) + (p1 & 0xFF00FF) + (p2 & 0xFF00FF) + (p3 & 0xFF00FF) + 0x020002) >> 2;
                uint32_t g = ((p0 & 0xFF00) + (p1 & 0xFF00) + (p2 & 0xFF00) + (p3 & 0xFF00) + 0x200) >> 2;
                o[x] = (rb & 0xFF00FF) | (g & 0xFF00) | 0xFF000000u;
            }
        }
    }

    // Selection outline: mark the selected pixels, grow the marks by two
    // pixels, and color what grew. Only inside the selection's box.
    bool have = sel_count_ > 0 && slx1_ >= 0;
    int bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;
    if (have) {
        bx0 = imax(0, slx0_ / ss_ - 3); by0 = imax(0, sly0_ / ss_ - 3);
        bx1 = imin(w_ - 1, slx1_ / ss_ + 3); by1 = imin(h_ - 1, sly1_ / ss_ + 3);
        for (int y = by0; y <= by1; y++) {
            uint8_t* mrow = mask_ + (size_t)y * w_;
            for (int x = bx0; x <= bx1; x++) {
                // Inside only when every sample is: the outline then covers
                // the smoothed edge pixels instead of leaving a seam.
                uint8_t mv = 3;
                for (int sy = 0; sy < ss_; sy++)
                    for (int sx = 0; sx < ss_; sx++) {
                        uint8_t s = g_selmark[ids_[(size_t)(y * ss_ + sy) * iw_ + x * ss_ + sx]];
                        if (s < mv) mv = s;
                    }
                mrow[x] = mv;
            }
        }
    }
    for (int i = 0; i < sel_count_; i++) g_selmark[sel_[i]] = 0;
    g_selmark[sel_active_] = 0;
    g_selmark[0] = 0;
    if (!have) return;
    uint8_t* h3 = tmp_;
    uint8_t* h5 = tmp_ + (size_t)w_ * h_;
    for (int y = by0; y <= by1; y++) {
        const uint8_t* m = mask_ + (size_t)y * w_;
        uint8_t* a = h3 + (size_t)y * w_;
        uint8_t* b = h5 + (size_t)y * w_;
        for (int x = bx0; x <= bx1; x++) {
            uint8_t v3 = m[x];
            if (x > bx0 && m[x - 1] > v3) v3 = m[x - 1];
            if (x < bx1 && m[x + 1] > v3) v3 = m[x + 1];
            uint8_t v5 = v3;
            if (x > bx0 + 1 && m[x - 2] > v5) v5 = m[x - 2];
            if (x < bx1 - 1 && m[x + 2] > v5) v5 = m[x + 2];
            a[x] = v3;
            b[x] = v5;
        }
    }
    int ar = (active_color >> 16) & 0xFF, ag = (active_color >> 8) & 0xFF, ab = active_color & 0xFF;
    int orr = (other_color >> 16) & 0xFF, og = (other_color >> 8) & 0xFF, ob = other_color & 0xFF;
    for (int y = by0; y <= by1; y++) {
        const uint8_t* m = mask_ + (size_t)y * w_;
        uint32_t* o = out_ + (size_t)y * w_;
        for (int x = bx0; x <= bx1; x++) {
            if (m[x]) continue;
            uint8_t d1 = 0, d2 = 0;
            for (int dy = -2; dy <= 2; dy++) {
                int yy = y + dy;
                if (yy < by0 || yy > by1) continue;
                uint8_t v5 = h5[(size_t)yy * w_ + x];
                if (v5 > d2) d2 = v5;
                if (dy >= -1 && dy <= 1) {
                    uint8_t v3 = h3[(size_t)yy * w_ + x];
                    if (v3 > d1) d1 = v3;
                }
            }
            if (!d2) continue;
            uint8_t which = d1 ? d1 : d2;
            int a = d1 ? 256 : 110;
            int cr = which == 2 ? ar : orr, cg = which == 2 ? ag : og, cb = which == 2 ? ab : ob;
            uint32_t px = o[x];
            int r = px & 0xFF, g = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF;
            r += (cr - r) * a >> 8; g += (cg - g) * a >> 8; b += (cb - b) * a >> 8;
            o[x] = pack((uint32_t)r, (uint32_t)g, (uint32_t)b);
        }
    }
}

}  // namespace fe
