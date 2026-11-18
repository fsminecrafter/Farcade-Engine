#include "gfx/particles.h"
#include "stb/stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe::gfx {

namespace {

constexpr int kDepthDiv = 4;                    // the depth map is this much smaller than the image
constexpr uint32_t kMaxOccluderTris = 120000;   // triangles rasterized into the depth map per frame

void shell_sort(int* idx, const float* key, int n) {     // far (large key) first
    for (int gap = n / 2; gap > 0; gap /= 2)
        for (int i = gap; i < n; i++) {
            int t = idx[i];
            float k = key[t];
            int j = i;
            while (j >= gap && key[idx[j - gap]] < k) { idx[j] = idx[j - gap]; j -= gap; }
            idx[j] = t;
        }
}

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The classic 5x7 font, ASCII 32..126: 5 columns per glyph, bit 0 the top row (bit 7 a descender).
const uint8_t kFont[95][5] = {
{0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},
{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x56,0x20,0x50},{0x00,0x08,0x07,0x03,0x00},{0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},
{0x2A,0x1C,0x7F,0x1C,0x2A},{0x08,0x08,0x3E,0x08,0x08},{0x00,0x80,0x70,0x30,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x00,0x60,0x60,0x00},
{0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x72,0x49,0x49,0x49,0x46},{0x21,0x41,0x49,0x4D,0x33},
{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x31},{0x41,0x21,0x11,0x09,0x07},{0x36,0x49,0x49,0x49,0x36},
{0x46,0x49,0x49,0x29,0x1E},{0x00,0x00,0x14,0x00,0x00},{0x00,0x40,0x34,0x00,0x00},{0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},
{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x59,0x09,0x06},{0x3E,0x41,0x5D,0x59,0x4E},{0x7C,0x12,0x11,0x12,0x7C},{0x7F,0x49,0x49,0x49,0x36},
{0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x41,0x3E},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x41,0x51,0x73},
{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
{0x7F,0x02,0x1C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},
{0x7F,0x09,0x19,0x29,0x46},{0x26,0x49,0x49,0x49,0x32},{0x03,0x01,0x7F,0x01,0x03},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},
{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},{0x61,0x59,0x49,0x4D,0x43},{0x00,0x7F,0x41,0x41,0x41},
{0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x41,0x7F},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},{0x00,0x03,0x07,0x08,0x00},
{0x20,0x54,0x54,0x78,0x40},{0x7F,0x28,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x28},{0x38,0x44,0x44,0x28,0x7F},{0x38,0x54,0x54,0x54,0x18},
{0x00,0x08,0x7E,0x09,0x02},{0x18,0xA4,0xA4,0x9C,0x78},{0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x40,0x3D,0x00},
{0x7F,0x10,0x28,0x44,0x00},{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x78,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
{0xFC,0x18,0x24,0x24,0x18},{0x18,0x24,0x24,0x18,0xFC},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x24},{0x04,0x04,0x3F,0x44,0x24},
{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x4C,0x90,0x90,0x90,0x7C},
{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x77,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x02,0x01,0x02,0x04,0x02},
};

}  // namespace

void ParticleRaster::draw_label(const TextLabel& t, const Mat4& view, const Mat4& proj, bool have_depth, uint32_t* dst, int width, int height,
                                int y0, int rows, bool rgba, long* pixels) {
    Vec3 vq = transform_point(view, t.pos);
    float depth = -vq.z;
    if (depth <= 0.05f || t.a <= 0.003f) return;
    Vec4 c = proj * Vec4{ vq.x, vq.y, vq.z, 1.0f };
    if (c.w <= 1e-5f) return;
    const bool ortho = proj.m[15] == 1.0f && proj.m[11] == 0.0f;
    float inv = 1.0f / c.w;
    float cx = (c.x * inv * 0.5f + 0.5f) * width;
    float cy = (0.5f - c.y * inv * 0.5f) * height;
    float ppm = proj.m[5] * 0.5f * height * (ortho ? 1.0f : inv);
    float cell = t.height * ppm / 8.0f;           // pixels per font dot (a line is 8 dots tall)
    if (cell < 0.45f || cell > 200.0f) return;
    int n = 0;
    while (t.text[n] && n < 95) n++;
    if (n == 0) return;
    float advance = 6.0f * cell;
    float total = (float)n * advance - cell;
    float x0 = t.align == 0 ? cx : (t.align == 2 ? cx - total : cx - total * 0.5f);
    float top = cy - 4.0f * cell;
    const int cr = (int)(fclamp(t.r, 0, 1) * 255.0f + 0.5f), cg = (int)(fclamp(t.g, 0, 1) * 255.0f + 0.5f), cb = (int)(fclamp(t.b, 0, 1) * 255.0f + 0.5f);
    const int ai = (int)(fclamp(t.a, 0, 1) * 256.0f);
    for (int pass = t.outline ? 0 : 1; pass < 2; pass++) {
        // pass 0: the outline (the glyphs a dot to each side, dark), pass 1: the text.
        for (int ch = 0; ch < n; ch++) {
            int g = (unsigned char)t.text[ch];
            if (g < 32 || g > 126) g = '?';
            const uint8_t* col = kFont[g - 32];
            for (int cxl = 0; cxl < 5; cxl++)
                for (int row = 0; row < 8; row++) {
                    if (!((col[cxl] >> row) & 1)) continue;
                    float fx = x0 + (float)ch * advance + (float)cxl * cell, fy = top + (float)row * cell;
                    int offs[5][2] = { { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
                    for (int o = pass == 0 ? 1 : 0; o < (pass == 0 ? 5 : 1); o++) {
                        float ox = pass == 0 ? offs[o][0] * (cell < 2.0f ? 1.0f : cell * 0.5f) : 0, oy = pass == 0 ? offs[o][1] * (cell < 2.0f ? 1.0f : cell * 0.5f) : 0;
                        int xa = (int)(fx + ox), xb = (int)(fx + ox + cell - 0.001f), ya = (int)(fy + oy), yb = (int)(fy + oy + cell - 0.001f);
                        if (xb < xa) xb = xa;
                        if (yb < ya) yb = ya;
                        for (int y = ya; y <= yb; y++) {
                            if (y < y0 || y >= y0 + rows || y < 0 || y >= height) continue;
                            uint32_t* r = dst + (size_t)(y - y0) * width;
                            for (int x = xa; x <= xb; x++) {
                                if (x < 0 || x >= width) continue;
                                if (have_depth && depth_[clampi(y / kDepthDiv, 0, dh_ - 1) * dw_ + clampi(x / kDepthDiv, 0, dw_ - 1)] < depth - 0.02f) continue;
                                (*pixels)++;
                                uint32_t d = r[x];
                                int dr, dg, db;
                                if (rgba) { dr = d & 0xFF; dg = (d >> 8) & 0xFF; db = (d >> 16) & 0xFF; }
                                else { dr = (d >> 16) & 0xFF; dg = (d >> 8) & 0xFF; db = d & 0xFF; }
                                int tr = pass == 0 ? 0 : cr, tg = pass == 0 ? 0 : cg, tb = pass == 0 ? 0 : cb;
                                int a = pass == 0 ? ai * 3 / 4 : ai;
                                dr += ((tr - dr) * a) >> 8; dg += ((tg - dg) * a) >> 8; db += ((tb - db) * a) >> 8;
                                r[x] = rgba ? (0xFF000000u | ((uint32_t)db << 16) | ((uint32_t)dg << 8) | (uint32_t)dr)
                                            : (((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db);
                            }
                        }
                    }
                }
        }
    }
}

ParticleRaster::ParticleRaster() { memset(tex_, 0, sizeof tex_); }
ParticleRaster::~ParticleRaster() {
    free(depth_);
    free(order_);
    free(keys_);
    forget_textures();
}

void ParticleRaster::forget_textures() {
    for (Texture& t : tex_) { free(t.px); t = Texture(); }
}

const ParticleRaster::Texture* ParticleRaster::texture(const char* path, void* (*read_asset)(const char*, size_t*, void*), void* user) {
    if (!path || !path[0] || !read_asset) return nullptr;
    for (Texture& t : tex_)
        if (t.px && strcmp(t.path, path) == 0) { t.last_use = frame_; return &t; }
    size_t size = 0;
    uint8_t* bytes = (uint8_t*)read_asset(path, &size, user);
    if (!bytes) return nullptr;
    int w = 0, h = 0, comp = 0;
    stbi_uc* rgba = nullptr;
    if (size > 0 && size < 0x7FFFFFFFu && stbi_info_from_memory(bytes, (int)size, &w, &h, &comp) && w > 0 && h > 0 && w <= 2048 && h <= 2048)
        rgba = stbi_load_from_memory(bytes, (int)size, &w, &h, &comp, 4);
    free(bytes);
    if (!rgba) return nullptr;
    Texture* slot = &tex_[0];
    for (Texture& t : tex_) {
        if (!t.px) { slot = &t; break; }
        if (t.last_use < slot->last_use) slot = &t;
    }
    free(slot->px);
    slot->px = (uint32_t*)malloc((size_t)w * h * 4);
    if (!slot->px) { stbi_image_free(rgba); *slot = Texture(); return nullptr; }
    memcpy(slot->px, rgba, (size_t)w * h * 4);
    stbi_image_free(rgba);
    snprintf(slot->path, sizeof slot->path, "%s", path);
    slot->w = w;
    slot->h = h;
    slot->last_use = frame_;
    return slot;
}

// A quarter size z-buffer of view depth (meters along the view direction)
// from the occluders' triangles. Triangles reaching behind the near plane are
// skipped.
void ParticleRaster::build_depth(const Occluder* occ, int n, const Mat4& view, const Mat4& proj, int width, int height) {
    dw_ = (width + kDepthDiv - 1) / kDepthDiv;
    dh_ = (height + kDepthDiv - 1) / kDepthDiv;
    if (dw_ * dh_ > dcap_) {
        dcap_ = dw_ * dh_;
        free(depth_);
        depth_ = (float*)malloc(sizeof(float) * dcap_);
        if (!depth_) { dcap_ = 0; dw_ = dh_ = 0; return; }
    }
    for (int i = 0; i < dw_ * dh_; i++) depth_[i] = 1e30f;
    Mat4 vp = proj * view;
    uint32_t tris = 0;
    for (int o = 0; o < n && tris < kMaxOccluderTris; o++) {
        const Occluder& oc = occ[o];
        if (!oc.positions || oc.vertex_count < 3) continue;
        Mat4 mvp = vp * oc.model;
        Mat4 mv = view * oc.model;
        for (uint32_t v = 0; v + 2 < oc.vertex_count && tris < kMaxOccluderTris; v += 3, tris++) {
            float sx[3], sy[3], iz[3];
            bool ok = true;
            for (int k = 0; k < 3; k++) {
                const Vec3& p = oc.positions[v + k];
                Vec4 c = mvp * Vec4{ p.x, p.y, p.z, 1.0f };
                if (c.w <= 1e-4f) { ok = false; break; }
                float inv = 1.0f / c.w;
                sx[k] = (c.x * inv * 0.5f + 0.5f) * dw_;
                sy[k] = (0.5f - c.y * inv * 0.5f) * dh_;
                Vec3 q = transform_point(mv, p);
                float depth = -q.z;
                iz[k] = depth > 1e-4f ? 1.0f / depth : 1e4f;
            }
            if (!ok) continue;
            float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
            if (area > -1e-6f && area < 1e-6f) continue;
            int x0 = clampi((int)(fminf_(sx[0], fminf_(sx[1], sx[2]))), 0, dw_ - 1);
            int x1 = clampi((int)(fmaxf_(sx[0], fmaxf_(sx[1], sx[2]))), 0, dw_ - 1);
            int y0 = clampi((int)(fminf_(sy[0], fminf_(sy[1], sy[2]))), 0, dh_ - 1);
            int y1 = clampi((int)(fmaxf_(sy[0], fmaxf_(sy[1], sy[2]))), 0, dh_ - 1);
            float ia = 1.0f / area;
            for (int y = y0; y <= y1; y++) {
                float py = y + 0.5f;
                for (int x = x0; x <= x1; x++) {
                    float px = x + 0.5f;
                    float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) * ia;
                    float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) * ia;
                    float w2 = 1.0f - w0 - w1;
                    if (w0 < -0.02f || w1 < -0.02f || w2 < -0.02f) continue;
                    float i1 = w0 * iz[0] + w1 * iz[1] + w2 * iz[2];
                    if (i1 <= 0) continue;
                    float d = 1.0f / i1;
                    float* dst = &depth_[y * dw_ + x];
                    if (d < *dst) *dst = d;
                }
            }
        }
    }
}

int ParticleRaster::draw(const ParticleBatch* batches, int batch_count, const TextLabel* labels, int label_count,
                         const Mat4& view, const Mat4& proj,
                         const Occluder* occluders, int occluder_count,
                         uint32_t* dst, int width, int height, int y0, int rows, bool rgba,
                         void* (*read_asset)(const char*, size_t*, void*), void* asset_user) {
    last_quads_ = 0;
    last_pixels_ = 0;
    frame_++;
    int total = 0;
    for (int b = 0; b < batch_count; b++) total += batches[b].count;
    if (!labels) label_count = 0;
    if ((total <= 0 && label_count <= 0) || !dst || width <= 0 || height <= 0 || rows <= 0) return 0;
    const bool ortho = proj.m[15] == 1.0f && proj.m[11] == 0.0f;
    bool have_depth = false;
    if (occluders && occluder_count > 0) {
        build_depth(occluders, occluder_count, view, proj, width, height);
        have_depth = depth_ != nullptr;
    }
    if (total > 0 && ocap_ < total) {
        ocap_ = total < 256 ? 256 : total * 2;
        free(order_);
        free(keys_);
        order_ = (int*)malloc(sizeof(int) * ocap_);
        keys_ = (float*)malloc(sizeof(float) * ocap_);
        if (!order_ || !keys_) { free(order_); free(keys_); order_ = nullptr; keys_ = nullptr; ocap_ = 0; return 0; }
    }
    const float half_w = width * 0.5f, half_h = height * 0.5f;
    int drawn = 0;
    long pixels = 0;
    for (int b = 0; b < batch_count; b++) {
        const ParticleBatch& pb = batches[b];
        if (pb.count <= 0 || !pb.items) continue;
        const Texture* tex = texture(pb.texture, read_asset, asset_user);
        int n = pb.count;
        // View depth of each quad; far ones first.
        for (int i = 0; i < n; i++) {
            Vec3 q = transform_point(view, pb.items[i].pos);
            keys_[i] = -q.z;
            order_[i] = i;
        }
        shell_sort(order_, keys_, n);
        for (int oi = 0; oi < n; oi++) {
            if (drawn >= kMaxQuads || pixels >= kMaxPixels) break;
            int i = order_[oi];
            const Particle& p = pb.items[i];
            float depth = keys_[i];
            if (depth <= 0.02f || p.a <= 0.003f) continue;
            Vec3 vq = transform_point(view, p.pos);
            Vec4 c = proj * Vec4{ vq.x, vq.y, vq.z, 1.0f };
            if (c.w <= 1e-5f) continue;
            float inv = 1.0f / c.w;
            float cx = (c.x * inv * 0.5f + 0.5f) * width;
            float cy = (0.5f - c.y * inv * 0.5f) * height;
            float hx = p.size * 0.5f * proj.m[0] * (ortho ? 1.0f : inv) * half_w;
            float hy = p.size * 0.5f * p.aspect * proj.m[5] * (ortho ? 1.0f : inv) * half_h;
            if (hx < 0.3f && hy < 0.3f) continue;
            if (hx > 4000.0f || hy > 4000.0f) continue;
            int x0 = clampi((int)(cx - hx), 0, width - 1), x1 = clampi((int)(cx + hx), 0, width - 1);
            int ya = clampi((int)(cy - hy), 0, height - 1), yb = clampi((int)(cy + hy), 0, height - 1);
            if (yb < y0 || ya >= y0 + rows) continue;
            if (ya < y0) ya = y0;
            if (yb >= y0 + rows) yb = y0 + rows - 1;
            if (cx + hx < 0 || cx - hx >= width) continue;
            drawn++;
            const int cr = (int)(fclamp(p.r, 0, 4) * 255.0f + 0.5f), cg = (int)(fclamp(p.g, 0, 4) * 255.0f + 0.5f), cb = (int)(fclamp(p.b, 0, 4) * 255.0f + 0.5f);
            const float ihx = 1.0f / hx, ihy = 1.0f / hy;
            for (int y = ya; y <= yb; y++) {
                float v = ((y + 0.5f) - cy) * ihy;          // -1..1
                uint32_t* row = dst + (size_t)(y - y0) * width;
                const float* drow = have_depth ? depth_ + (size_t)clampi(y / kDepthDiv, 0, dh_ - 1) * dw_ : nullptr;
                for (int x = x0; x <= x1; x++) {
                    float u = ((x + 0.5f) - cx) * ihx;
                    float a;
                    int tr = cr, tg = cg, tb = cb;
                    if (tex) {
                        int tx = clampi((int)((u * 0.5f + 0.5f) * tex->w), 0, tex->w - 1);
                        int ty = clampi((int)((v * 0.5f + 0.5f) * tex->h), 0, tex->h - 1);
                        uint32_t t = tex->px[ty * tex->w + tx];
                        a = ((t >> 24) & 0xFF) * (1.0f / 255.0f);
                        tr = (cr * (int)(t & 0xFF)) / 255;
                        tg = (cg * (int)((t >> 8) & 0xFF)) / 255;
                        tb = (cb * (int)((t >> 16) & 0xFF)) / 255;
                    } else {
                        float r2 = u * u + v * v;
                        if (r2 >= 1.0f) continue;
                        float f = 1.0f - r2;
                        a = f * f;
                    }
                    a *= p.a;
                    if (a <= 0.004f) continue;
                    if (have_depth && drow[clampi(x / kDepthDiv, 0, dw_ - 1)] < depth - 0.02f) continue;
                    pixels++;
                    uint32_t d = row[x];
                    int dr, dg, db;
                    if (rgba) { dr = d & 0xFF; dg = (d >> 8) & 0xFF; db = (d >> 16) & 0xFF; }
                    else { dr = (d >> 16) & 0xFF; dg = (d >> 8) & 0xFF; db = d & 0xFF; }
                    int ai = (int)(a * 256.0f);
                    if (pb.additive) {
                        dr += (tr * ai) >> 8; dg += (tg * ai) >> 8; db += (tb * ai) >> 8;
                        if (dr > 255) dr = 255;
                        if (dg > 255) dg = 255;
                        if (db > 255) db = 255;
                    } else {
                        if (tr > 255) tr = 255;
                        if (tg > 255) tg = 255;
                        if (tb > 255) tb = 255;
                        dr += ((tr - dr) * ai) >> 8; dg += ((tg - dg) * ai) >> 8; db += ((tb - db) * ai) >> 8;
                    }
                    row[x] = rgba ? (0xFF000000u | ((uint32_t)db << 16) | ((uint32_t)dg << 8) | (uint32_t)dr)
                                  : (((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db);
                }
            }
        }
    }
    for (int i = 0; i < label_count; i++) {
        if (pixels >= kMaxPixels) break;
        draw_label(labels[i], view, proj, have_depth, dst, width, height, y0, rows, rgba, &pixels);
        drawn++;
    }
    last_quads_ = drawn;
    last_pixels_ = pixels;
    return drawn;
}

namespace {

void box_fill(uint32_t* px, int w, int h, int pitch, int x0, int y0, int x1, int y1, uint32_t c, int alpha) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (int y = y0; y < y1; y++) {
        uint32_t* r = px + (size_t)y * pitch;
        for (int x = x0; x < x1; x++) {
            uint32_t d = r[x];
            int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
            dr += ((int)((c >> 16) & 255) - dr) * alpha >> 8;
            dg += ((int)((c >> 8) & 255) - dg) * alpha >> 8;
            db += ((int)(c & 255) - db) * alpha >> 8;
            r[x] = ((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db;
        }
    }
}

int box_text_width(const char* s, int cell) {
    int n = 0;
    while (s[n]) n++;
    return n ? n * 6 * cell - cell : 0;
}

void box_text(uint32_t* px, int w, int h, int pitch, int x, int y, const char* s, int cell, uint32_t c) {
    for (int ch = 0; s[ch]; ch++) {
        int g = (unsigned char)s[ch];
        if (g < 32 || g > 126) g = '?';
        const uint8_t* col = kFont[g - 32];
        for (int cx = 0; cx < 5; cx++)
            for (int row = 0; row < 8; row++)
                if ((col[cx] >> row) & 1)
                    box_fill(px, w, h, pitch, x + (ch * 6 + cx) * cell, y + row * cell, x + (ch * 6 + cx + 1) * cell,
                             y + (row + 1) * cell, c, 256);
    }
}

}  // namespace

void draw_status_box(uint32_t* xrgb, int width, int height, int pitch, const char* line, const char* sub, float progress,
                     double t) {
    if (!xrgb || width < 64 || height < 48) return;
    int cell = height < 360 ? 1 : 2 + (height >= 940 ? (height - 540) / 400 : 0);
    int pad = 8 * cell, lh = 8 * cell;
    int tw = box_text_width(line ? line : "", cell);
    if (sub && box_text_width(sub, cell) > tw) tw = box_text_width(sub, cell);
    int bw = tw + 2 * pad;
    if (bw < width / 3) bw = width / 3;
    if (bw > width - 16) bw = width - 16;
    int bar_h = 3 * cell;
    int bh = pad + lh + pad / 2 + bar_h + (sub && *sub ? pad / 2 + lh : 0) + pad;
    int x0 = (width - bw) / 2, y0 = height - bh - height / 12;
    box_fill(xrgb, width, height, pitch, x0, y0, x0 + bw, y0 + bh, 0x14181E, 215);
    box_fill(xrgb, width, height, pitch, x0, y0, x0 + bw, y0 + cell, 0x4FA3E0, 256);
    int y = y0 + pad;
    if (line) box_text(xrgb, width, height, pitch, x0 + pad, y, line, cell, 0xE8EEF3);
    y += lh + pad / 2;
    int tx0 = x0 + pad, tx1 = x0 + bw - pad;
    box_fill(xrgb, width, height, pitch, tx0, y, tx1, y + bar_h, 0x2C3742, 256);
    if (progress >= 0.0f) {
        float p = progress > 1.0f ? 1.0f : progress;
        int fx = tx0 + (int)((float)(tx1 - tx0) * p);
        if (fx < tx0 + bar_h) fx = tx0 + bar_h;
        box_fill(xrgb, width, height, pitch, tx0, y, fx, y + bar_h, 0x4FA3E0, 256);
    } else {
        // No known end: a segment that runs along the track.
        int seg = (tx1 - tx0) / 4;
        double ph = t * 0.8 - (double)(long)(t * 0.8);
        int sx = tx0 + (int)((double)(tx1 - tx0 + seg) * ph) - seg;
        box_fill(xrgb, width, height, pitch, sx < tx0 ? tx0 : sx, y, sx + seg > tx1 ? tx1 : sx + seg, y + bar_h, 0x4FA3E0, 256);
    }
    y += bar_h + pad / 2;
    if (sub && *sub) box_text(xrgb, width, height, pitch, x0 + pad, y, sub, cell, 0x8FA3AF);
}

}  // namespace fe::gfx
