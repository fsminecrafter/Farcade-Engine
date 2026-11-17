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

}  // namespace

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

int ParticleRaster::draw(const ParticleBatch* batches, int batch_count, const Mat4& view, const Mat4& proj,
                         const Occluder* occluders, int occluder_count,
                         uint32_t* dst, int width, int height, int y0, int rows, bool rgba,
                         void* (*read_asset)(const char*, size_t*, void*), void* asset_user) {
    last_quads_ = 0;
    last_pixels_ = 0;
    frame_++;
    int total = 0;
    for (int b = 0; b < batch_count; b++) total += batches[b].count;
    if (total <= 0 || !dst || width <= 0 || height <= 0 || rows <= 0) return 0;
    const bool ortho = proj.m[15] == 1.0f && proj.m[11] == 0.0f;
    bool have_depth = false;
    if (occluders && occluder_count > 0) {
        build_depth(occluders, occluder_count, view, proj, width, height);
        have_depth = depth_ != nullptr;
    }
    if (ocap_ < total) {
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
            float hy = p.size * 0.5f * proj.m[5] * (ortho ? 1.0f : inv) * half_h;
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
    last_quads_ = drawn;
    last_pixels_ = pixels;
    return drawn;
}

}  // namespace fe::gfx
