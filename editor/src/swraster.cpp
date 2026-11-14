#include "swraster.h"

#include "imgui/imgui.h"

namespace fe {

namespace {

inline int div255(int x) { return (x + 1 + (x >> 8)) >> 8; }     // x / 255 for 0..65025

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline float fmin2(float a, float b) { return a < b ? a : b; }
inline float fmax2(float a, float b) { return a > b ? a : b; }
inline float fmin3(float a, float b, float c) { return fmin2(a, fmin2(b, c)); }
inline float fmax3(float a, float b, float c) { return fmax2(a, fmax2(b, c)); }

// floor/ceil without libm (the values are small: screen coordinates).
inline int ifloor(float v) { int i = (int)v; return (v < (float)i) ? i - 1 : i; }
inline int iceil(float v) { int i = (int)v; return (v > (float)i) ? i + 1 : i; }

// Writes color (r, g, b, a: 0..255) over a framebuffer pixel.
inline void blend(uint32_t* d, int r, int g, int b, int a) {
    if (a <= 0) return;
    if (a >= 255) { *d = (uint32_t)(r << 16 | g << 8 | b); return; }
    uint32_t dst = *d;
    int dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    int ia = 255 - a;
    dr = div255(r * a + dr * ia);
    dg = div255(g * a + dg * ia);
    db = div255(b * a + db * ia);
    *d = (uint32_t)(dr << 16 | dg << 8 | db);
}

struct Rgba { int r, g, b, a; };

inline Rgba unpack(ImU32 c) {
    return { (int)(c >> IM_COL32_R_SHIFT) & 0xFF, (int)(c >> IM_COL32_G_SHIFT) & 0xFF,
             (int)(c >> IM_COL32_B_SHIFT) & 0xFF, (int)(c >> IM_COL32_A_SHIFT) & 0xFF };
}

// Texel at integer coordinates, clamped; returns RGBA 0..255.
inline Rgba texel(const SwTexture* t, int x, int y) {
    x = imax(0, imin(t->width - 1, x));
    y = imax(0, imin(t->height - 1, y));
    if (t->format == SwTexture::ALPHA8) {
        int a = t->pixels[(size_t)y * t->pitch + x];
        return { 255, 255, 255, a };
    }
    const uint8_t* p = t->pixels + ((size_t)y * t->pitch + x) * 4;
    return { p[0], p[1], p[2], p[3] };
}

inline Rgba sample(const SwTexture* t, float u, float v) {
    if (!t || !t->pixels) return { 255, 255, 255, 255 };
    if (!t->linear) return texel(t, ifloor(u * t->width), ifloor(v * t->height));
    float fx = u * t->width - 0.5f, fy = v * t->height - 0.5f;
    int x0 = ifloor(fx), y0 = ifloor(fy);
    int ax = (int)((fx - x0) * 256.0f), ay = (int)((fy - y0) * 256.0f);
    Rgba a = texel(t, x0, y0), b = texel(t, x0 + 1, y0);
    Rgba c = texel(t, x0, y0 + 1), d = texel(t, x0 + 1, y0 + 1);
    auto lerp = [](int p, int q, int w) { return p + (((q - p) * w) >> 8); };
    return { lerp(lerp(a.r, b.r, ax), lerp(c.r, d.r, ax), ay), lerp(lerp(a.g, b.g, ax), lerp(c.g, d.g, ax), ay),
             lerp(lerp(a.b, b.b, ax), lerp(c.b, d.b, ax), ay), lerp(lerp(a.a, b.a, ax), lerp(c.a, d.a, ax), ay) };
}

struct Clip { int x0, y0, x1, y1; };     // pixels [x0, x1) x [y0, y1)

// An axis-aligned rectangle with one color and linearly mapped UVs (what
// ImGui's PrimRect / PrimRectUV emit: solid fills and glyphs).
uint64_t fill_rect(Framebuffer& fb, const Clip& clip, float x0f, float y0f, float x1f, float y1f,
                   float u0, float v0, float u1, float v1, ImU32 col, const SwTexture* tex) {
    if (x1f < x0f) { float t = x0f; x0f = x1f; x1f = t; t = u0; u0 = u1; u1 = t; }
    if (y1f < y0f) { float t = y0f; y0f = y1f; y1f = t; t = v0; v0 = v1; v1 = t; }
    // Pixel centers inside the rectangle.
    int x0 = imax(clip.x0, iceil(x0f - 0.5f)), x1 = imin(clip.x1, iceil(x1f - 0.5f));
    int y0 = imax(clip.y0, iceil(y0f - 0.5f)), y1 = imin(clip.y1, iceil(y1f - 0.5f));
    if (x0 >= x1 || y0 >= y1) return 0;
    Rgba c = unpack(col);
    if (c.a == 0) return 0;
    bool solid = !tex || !tex->pixels || (u0 == u1 && v0 == v1);
    if (solid && tex && tex->pixels) {
        Rgba t = sample(tex, u0, v0);
        c = { div255(c.r * t.r), div255(c.g * t.g), div255(c.b * t.b), div255(c.a * t.a) };
        if (c.a == 0) return 0;
    }
    if (solid) {
        uint32_t packed = (uint32_t)(c.r << 16 | c.g << 8 | c.b);
        for (int y = y0; y < y1; y++) {
            uint32_t* row = fb.pixels + (size_t)y * fb.pitch;
            if (c.a == 255) for (int x = x0; x < x1; x++) row[x] = packed;
            else for (int x = x0; x < x1; x++) blend(&row[x], c.r, c.g, c.b, c.a);
        }
        return (uint64_t)(x1 - x0) * (y1 - y0);
    }
    float du = (x1f > x0f) ? (u1 - u0) / (x1f - x0f) : 0.0f;
    float dv = (y1f > y0f) ? (v1 - v0) / (y1f - y0f) : 0.0f;
    for (int y = y0; y < y1; y++) {
        uint32_t* row = fb.pixels + (size_t)y * fb.pitch;
        float v = v0 + ((float)y + 0.5f - y0f) * dv;
        float u = u0 + ((float)x0 + 0.5f - x0f) * du;
        for (int x = x0; x < x1; x++, u += du) {
            Rgba t = sample(tex, u, v);
            int a = div255(c.a * t.a);
            if (!a) continue;
            blend(&row[x], div255(c.r * t.r), div255(c.g * t.g), div255(c.b * t.b), a);
        }
    }
    return (uint64_t)(x1 - x0) * (y1 - y0);
}

// A general triangle: color and UV interpolated, top-left fill rule.
uint64_t fill_triangle(Framebuffer& fb, const Clip& clip, const ImDrawVert& a, const ImDrawVert& b,
                       const ImDrawVert& c, const SwTexture* tex) {
    float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
    if (area > -1e-6f && area < 1e-6f) return 0;     // degenerate
    const ImDrawVert* v0 = &a;
    const ImDrawVert* v1 = &b;
    const ImDrawVert* v2 = &c;

    int minx = imax(clip.x0, ifloor(fmin3(v0->pos.x, v1->pos.x, v2->pos.x)));
    int maxx = imin(clip.x1 - 1, iceil(fmax3(v0->pos.x, v1->pos.x, v2->pos.x)));
    int miny = imax(clip.y0, ifloor(fmin3(v0->pos.y, v1->pos.y, v2->pos.y)));
    int maxy = imin(clip.y1 - 1, iceil(fmax3(v0->pos.y, v1->pos.y, v2->pos.y)));
    if (minx > maxx || miny > maxy) return 0;

    // Edge i is opposite vertex i: w_i(p) = (Vj - Vk) x (p - Vk) style.
    struct Edge { float a, b, c; bool tl; };
    auto make_edge = [](const ImVec2& p, const ImVec2& q) {
        Edge e;
        e.a = p.y - q.y;
        e.b = q.x - p.x;
        e.c = p.x * q.y - p.y * q.x;
        e.tl = false;
        return e;
    };
    Edge e0 = make_edge(v1->pos, v2->pos);
    Edge e1 = make_edge(v2->pos, v0->pos);
    Edge e2 = make_edge(v0->pos, v1->pos);
    // Orientation check: e0 at v0 must be positive.
    float s = e0.a * v0->pos.x + e0.b * v0->pos.y + e0.c;
    if (s < 0) {
        e0.a = -e0.a; e0.b = -e0.b; e0.c = -e0.c;
        e1.a = -e1.a; e1.b = -e1.b; e1.c = -e1.c;
        e2.a = -e2.a; e2.b = -e2.b; e2.c = -e2.c;
        s = -s;
    }
    // Top-left rule, with w positive inside: a left edge has the inside to
    // its right (w grows with x), a top edge is horizontal with the inside
    // below (w grows with y). Pixels exactly on other edges are left out, so
    // triangles sharing an edge never blend a pixel twice.
    e0.tl = e0.a > 0 || (e0.a == 0 && e0.b > 0);
    e1.tl = e1.a > 0 || (e1.a == 0 && e1.b > 0);
    e2.tl = e2.a > 0 || (e2.a == 0 && e2.b > 0);
    float inv = 1.0f / s;

    Rgba c0 = unpack(v0->col), c1 = unpack(v1->col), c2 = unpack(v2->col);
    bool flat = v0->col == v1->col && v1->col == v2->col;
    bool uvflat = v0->uv.x == v1->uv.x && v1->uv.x == v2->uv.x && v0->uv.y == v1->uv.y && v1->uv.y == v2->uv.y;
    Rgba flat_tex = { 255, 255, 255, 255 };
    bool use_tex = tex && tex->pixels;
    if (use_tex && uvflat) { flat_tex = sample(tex, v0->uv.x, v0->uv.y); use_tex = false; }

    uint64_t drawn = 0;
    for (int y = miny; y <= maxy; y++) {
        float py = (float)y + 0.5f;
        float px = (float)minx + 0.5f;
        float w0 = e0.a * px + e0.b * py + e0.c;
        float w1 = e1.a * px + e1.b * py + e1.c;
        float w2 = e2.a * px + e2.b * py + e2.c;
        uint32_t* row = fb.pixels + (size_t)y * fb.pitch;
        for (int x = minx; x <= maxx; x++, w0 += e0.a, w1 += e1.a, w2 += e2.a) {
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            if ((w0 == 0 && !e0.tl) || (w1 == 0 && !e1.tl) || (w2 == 0 && !e2.tl)) continue;
            float b0 = w0 * inv, b1 = w1 * inv, b2 = w2 * inv;
            int r, g, bb, al;
            if (flat) { r = c0.r; g = c0.g; bb = c0.b; al = c0.a; }
            else {
                r = (int)(c0.r * b0 + c1.r * b1 + c2.r * b2 + 0.5f);
                g = (int)(c0.g * b0 + c1.g * b1 + c2.g * b2 + 0.5f);
                bb = (int)(c0.b * b0 + c1.b * b1 + c2.b * b2 + 0.5f);
                al = (int)(c0.a * b0 + c1.a * b1 + c2.a * b2 + 0.5f);
            }
            Rgba t = flat_tex;
            if (use_tex) {
                float u = v0->uv.x * b0 + v1->uv.x * b1 + v2->uv.x * b2;
                float v = v0->uv.y * b0 + v1->uv.y * b1 + v2->uv.y * b2;
                t = sample(tex, u, v);
            }
            blend(&row[x], div255(r * t.r), div255(g * t.g), div255(bb * t.b), div255(al * t.a));
            drawn++;
        }
    }
    return drawn;
}

}  // namespace

void SwRenderer::clear(Framebuffer& fb, uint32_t color) {
    for (int y = 0; y < fb.height; y++) {
        uint32_t* row = fb.pixels + (size_t)y * fb.pitch;
        for (int x = 0; x < fb.width; x++) row[x] = color;
    }
}

void SwRenderer::update_texture(ImTextureData* tex) {
    SwTexture* t = (SwTexture*)tex->BackendUserData;
    if (tex->Status == ImTextureStatus_WantDestroy) {
        if (t) IM_DELETE(t);
        tex->BackendUserData = nullptr;
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
        return;
    }
    if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
        // The pixels stay in ImGui's buffer (it outlives the texture), so an
        // update only has to refresh the view of it.
        if (!t) {
            t = IM_NEW(SwTexture)();
            tex->BackendUserData = t;
        }
        t->format = tex->Format == ImTextureFormat_Alpha8 ? SwTexture::ALPHA8 : SwTexture::RGBA8;
        t->width = tex->Width;
        t->height = tex->Height;
        t->pitch = tex->Width;
        t->pixels = (const uint8_t*)tex->Pixels;
        t->linear = false;
        tex->SetTexID((ImTextureID)(uintptr_t)t);
        tex->SetStatus(ImTextureStatus_OK);
    }
}

void SwRenderer::render(ImDrawData* dd, Framebuffer& fb) {
    stats_ = SwStats();
    if (dd->Textures)
        for (ImTextureData* tex : *dd->Textures)
            if (tex->Status != ImTextureStatus_OK) update_texture(tex);

    ImVec2 off = dd->DisplayPos;
    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList* list = dd->CmdLists[n];
        const ImDrawVert* vtx = list->VtxBuffer.Data;
        const ImDrawIdx* idx = list->IdxBuffer.Data;
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState) cmd.UserCallback(list, &cmd);
                continue;
            }
            stats_.commands++;
            Clip clip = { imax(0, ifloor(cmd.ClipRect.x - off.x)), imax(0, ifloor(cmd.ClipRect.y - off.y)),
                          imin(fb.width, iceil(cmd.ClipRect.z - off.x)), imin(fb.height, iceil(cmd.ClipRect.w - off.y)) };
            if (clip.x0 >= clip.x1 || clip.y0 >= clip.y1) continue;
            const SwTexture* tex = (const SwTexture*)(uintptr_t)cmd.GetTexID();
            const ImDrawIdx* ip = idx + cmd.IdxOffset;
            const ImDrawVert* vp = vtx + cmd.VtxOffset;
            unsigned count = cmd.ElemCount;
            for (unsigned i = 0; i + 2 < count;) {
                // ImGui's rectangles: indices p, p+1, p+2, p, p+2, p+3 over
                // corners top-left, top-right, bottom-right, bottom-left.
                if (i + 5 < count && ip[i + 3] == ip[i] && ip[i + 4] == ip[i + 2] &&
                    ip[i + 1] == ip[i] + 1 && ip[i + 2] == ip[i] + 2 && ip[i + 5] == ip[i] + 3) {
                    const ImDrawVert& A = vp[ip[i]];
                    const ImDrawVert& B = vp[ip[i] + 1];
                    const ImDrawVert& C = vp[ip[i] + 2];
                    const ImDrawVert& D = vp[ip[i] + 3];
                    if (A.pos.y == B.pos.y && C.pos.y == D.pos.y && A.pos.x == D.pos.x && B.pos.x == C.pos.x &&
                        A.uv.y == B.uv.y && C.uv.y == D.uv.y && A.uv.x == D.uv.x && B.uv.x == C.uv.x &&
                        A.col == B.col && A.col == C.col && A.col == D.col) {
                        stats_.pixels += fill_rect(fb, clip, A.pos.x - off.x, A.pos.y - off.y, C.pos.x - off.x,
                                                   C.pos.y - off.y, A.uv.x, A.uv.y, C.uv.x, C.uv.y, A.col, tex);
                        stats_.fast_rects++;
                        i += 6;
                        continue;
                    }
                }
                ImDrawVert a = vp[ip[i]], b = vp[ip[i + 1]], c = vp[ip[i + 2]];
                a.pos.x -= off.x; a.pos.y -= off.y;
                b.pos.x -= off.x; b.pos.y -= off.y;
                c.pos.x -= off.x; c.pos.y -= off.y;
                stats_.pixels += fill_triangle(fb, clip, a, b, c, tex);
                stats_.triangles++;
                i += 3;
            }
        }
    }
}

}  // namespace fe
