#include "view3d.h"

#include <stdlib.h>

namespace fe {

namespace {
constexpr float kNearW = 0.05f;     // clip triangles and lines at w = kNearW

inline Vec4 lerp4(const Vec4& a, const Vec4& b, float t) {
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
}
inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
}  // namespace

View3D::~View3D() {
    free(color_);
    free(depth_);
}

void View3D::begin(int w, int h, const Mat4& viewproj, uint32_t top, uint32_t bottom) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w * h > cap_) {
        free(color_);
        free(depth_);
        cap_ = w * h;
        color_ = (uint8_t*)malloc((size_t)cap_ * 4);
        depth_ = (float*)malloc((size_t)cap_ * sizeof(float));
    }
    w_ = w;
    h_ = h;
    vp_ = viewproj;
    int tr = (top >> 16) & 0xFF, tg = (top >> 8) & 0xFF, tb = top & 0xFF;
    int br = (bottom >> 16) & 0xFF, bg = (bottom >> 8) & 0xFF, bb = bottom & 0xFF;
    for (int y = 0; y < h; y++) {
        int k = h > 1 ? y * 256 / (h - 1) : 0;
        uint32_t px = (uint32_t)(tr + ((br - tr) * k >> 8)) | (uint32_t)(tg + ((bg - tg) * k >> 8)) << 8 |
                      (uint32_t)(tb + ((bb - tb) * k >> 8)) << 16 | 0xFF000000u;
        uint32_t* row = (uint32_t*)color_ + (size_t)y * w;
        float* drow = depth_ + (size_t)y * w;
        for (int x = 0; x < w; x++) { row[x] = px; drow[x] = 1.0f; }
    }
    tex_.format = SwTexture::RGBA8;
    tex_.width = w;
    tex_.height = h;
    tex_.pitch = w;
    tex_.pixels = color_;
    tex_.linear = false;
}

void View3D::triangle(const Vec3& a, const Vec3& b, const Vec3& c, uint32_t color) {
    Vec4 in[3] = { vp_ * Vec4{ a.x, a.y, a.z, 1 }, vp_ * Vec4{ b.x, b.y, b.z, 1 }, vp_ * Vec4{ c.x, c.y, c.z, 1 } };
    // Clip against the near plane (w > kNearW): up to 4 vertices, drawn as a fan.
    Vec4 out[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const Vec4& p = in[i];
        const Vec4& q = in[(i + 1) % 3];
        bool pin = p.w > kNearW, qin = q.w > kNearW;
        if (pin) out[n++] = p;
        if (pin != qin) out[n++] = lerp4(p, q, (kNearW - p.w) / (q.w - p.w));
    }
    if (n < 3) return;
    Vec4 tri[3] = { out[0], out[1], out[2] };
    raster(tri, color);
    if (n == 4) {
        Vec4 tri2[3] = { out[0], out[2], out[3] };
        raster(tri2, color);
    }
}

void View3D::raster(const Vec4* v, uint32_t color) {
    // To screen: x, y in pixels, z in [0, 1] (affine in screen space).
    float sx[3], sy[3], sz[3];
    for (int i = 0; i < 3; i++) {
        float iw = 1.0f / v[i].w;
        sx[i] = (v[i].x * iw * 0.5f + 0.5f) * w_;
        sy[i] = (0.5f - v[i].y * iw * 0.5f) * h_;
        sz[i] = v[i].z * iw * 0.5f + 0.5f;
    }
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
    if (area >= 0) return;      // back face (counter-clockwise in y-up becomes clockwise here)
    int minx = imax(0, (int)fminf_(sx[0], fminf_(sx[1], sx[2])));
    int maxx = imin(w_ - 1, (int)fmaxf_(sx[0], fmaxf_(sx[1], sx[2])) + 1);
    int miny = imax(0, (int)fminf_(sy[0], fminf_(sy[1], sy[2])));
    int maxy = imin(h_ - 1, (int)fmaxf_(sy[0], fmaxf_(sy[1], sy[2])) + 1);
    if (minx > maxx || miny > maxy) return;
    float inv = 1.0f / area;
    uint32_t px = ((color >> 16) & 0xFF) | (color & 0xFF00) | ((color & 0xFF) << 16) | 0xFF000000u;
    // Edge functions; with area < 0 the inside has all of them <= 0.
    for (int y = miny; y <= maxy; y++) {
        float py = y + 0.5f;
        uint32_t* row = (uint32_t*)color_ + (size_t)y * w_;
        float* drow = depth_ + (size_t)y * w_;
        for (int x = minx; x <= maxx; x++) {
            float pxf = x + 0.5f;
            float w0 = (sx[2] - sx[1]) * (py - sy[1]) - (sy[2] - sy[1]) * (pxf - sx[1]);
            float w1 = (sx[0] - sx[2]) * (py - sy[2]) - (sy[0] - sy[2]) * (pxf - sx[2]);
            float w2 = (sx[1] - sx[0]) * (py - sy[0]) - (sy[1] - sy[0]) * (pxf - sx[0]);
            if (w0 > 0 || w1 > 0 || w2 > 0) continue;
            float z = (w0 * sz[0] + w1 * sz[1] + w2 * sz[2]) * inv;
            if (z < 0 || z >= drow[x]) continue;
            drow[x] = z;
            row[x] = px;
        }
    }
}

void View3D::line(const Vec3& a, const Vec3& b, uint32_t color, int alpha) {
    Vec4 p = vp_ * Vec4{ a.x, a.y, a.z, 1 }, q = vp_ * Vec4{ b.x, b.y, b.z, 1 };
    if (p.w <= kNearW && q.w <= kNearW) return;
    if (p.w <= kNearW) p = lerp4(p, q, (kNearW - p.w) / (q.w - p.w));
    else if (q.w <= kNearW) q = lerp4(q, p, (kNearW - q.w) / (p.w - q.w));
    float x0 = (p.x / p.w * 0.5f + 0.5f) * w_, y0 = (0.5f - p.y / p.w * 0.5f) * h_, z0 = p.z / p.w * 0.5f + 0.5f;
    float x1 = (q.x / q.w * 0.5f + 0.5f) * w_, y1 = (0.5f - q.y / q.w * 0.5f) * h_, z1 = q.z / q.w * 0.5f + 0.5f;
    // Clip to the image (Liang-Barsky), so a line passing close to the
    // camera does not step across thousands of off-screen pixels.
    float t0 = 0.0f, t1 = 1.0f, dx = x1 - x0, dy = y1 - y0;
    const float pv[4] = { -dx, dx, -dy, dy };
    const float qv[4] = { x0, (float)w_ - 1.0f - x0, y0, (float)h_ - 1.0f - y0 };
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
        int xi = (int)x, yi = (int)y;
        if (xi < 0 || yi < 0 || xi >= w_ || yi >= h_) continue;
        size_t k = (size_t)yi * w_ + xi;
        // Behind something; the bias lets lines lying on a surface (the grid
        // on the ground) show instead of flickering.
        if (z > depth_[k] + 2e-4f) continue;
        uint8_t* d = color_ + k * 4;
        d[0] = (uint8_t)(d[0] + ((cr - d[0]) * alpha >> 8));
        d[1] = (uint8_t)(d[1] + ((cg - d[1]) * alpha >> 8));
        d[2] = (uint8_t)(d[2] + ((cb - d[2]) * alpha >> 8));
    }
}

}  // namespace fe
