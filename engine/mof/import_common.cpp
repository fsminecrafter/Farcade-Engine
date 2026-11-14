// Shared importer helpers: model memory, files, images, normals/tangents, matrices.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mof/import_internal.h"
#include "stb/stb_image.h"

static void* libc_realloc(void*, void* ptr, size_t, size_t new_size) {
    if (new_size == 0) { free(ptr); return nullptr; }
    return realloc(ptr, new_size);
}
mof_alloc mof_libc_alloc() { return mof_alloc{libc_realloc, nullptr}; }

void mof_model_init(mof_model* m) {
    memset(m, 0, sizeof *m);
    m->desc.alloc = mof_libc_alloc();
}

void mof_model_free(mof_model* m) {
    for (size_t i = 0; i < m->block_count; i++) free(m->blocks[i]);
    free(m->blocks);
    memset(m, 0, sizeof *m);
}

void* mof_model_alloc(mof_model* m, size_t bytes) {
    if (m->block_count == m->block_cap) {
        size_t cap = m->block_cap ? m->block_cap * 2 : 64;
        void** nb = (void**)realloc(m->blocks, cap * sizeof(void*));
        if (!nb) return nullptr;
        m->blocks = nb;
        m->block_cap = cap;
    }
    // 16-byte alignment is not needed by mof_write; calloc's is plenty.
    void* p = calloc(1, bytes ? bytes : 1);
    if (!p) return nullptr;
    m->blocks[m->block_count++] = p;
    return p;
}

char* mof_model_strdup(mof_model* m, const char* s) {
    if (!s) return nullptr;
    size_t n = strlen(s);
    char* p = (char*)mof_model_alloc(m, n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

void mof_model_warn(mof_model* m, const char* fmt, ...) {
    size_t n = strlen(m->warnings);
    if (n + 2 >= sizeof m->warnings) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->warnings + n, sizeof m->warnings - n - 1, fmt, ap);
    va_end(ap);
    n = strlen(m->warnings);
    if (n + 1 < sizeof m->warnings) { m->warnings[n] = '\n'; m->warnings[n + 1] = 0; }
}

bool mof_read_file(mof_model* m, const char* path, uint8_t** data, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    uint8_t* p = (uint8_t*)mof_model_alloc(m, (size_t)n + 1);
    if (!p) { fclose(f); return false; }
    size_t got = fread(p, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) return false;
    *data = p;
    *size = (size_t)n;
    return true;
}

// ---------------------------------------------------------------- images

static bool sniff_png(const uint8_t* b, size_t n, uint32_t* w, uint32_t* h) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (n < 8 || memcmp(b, sig, 8) != 0) return false;
    *w = *h = 0;
    if (n >= 24 && memcmp(b + 12, "IHDR", 4) == 0) {
        *w = (uint32_t)b[16] << 24 | (uint32_t)b[17] << 16 | (uint32_t)b[18] << 8 | b[19];
        *h = (uint32_t)b[20] << 24 | (uint32_t)b[21] << 16 | (uint32_t)b[22] << 8 | b[23];
    }
    return true;
}

static bool sniff_jpeg(const uint8_t* b, size_t n, uint32_t* w, uint32_t* h) {
    if (n < 4 || b[0] != 0xFF || b[1] != 0xD8 || b[2] != 0xFF) return false;
    *w = *h = 0;
    size_t i = 2;
    while (i + 4 <= n) {
        if (b[i] != 0xFF) { i++; continue; }
        uint8_t mk = b[i + 1];
        if (mk == 0xFF) { i++; continue; }
        if (mk == 0xD8 || mk == 0x01 || (mk >= 0xD0 && mk <= 0xD7)) { i += 2; continue; }
        size_t len = (size_t)b[i + 2] << 8 | b[i + 3];
        if (mk >= 0xC0 && mk <= 0xCF && mk != 0xC4 && mk != 0xC8 && mk != 0xCC) {
            if (i + 9 <= n) { *h = (uint32_t)b[i + 5] << 8 | b[i + 6]; *w = (uint32_t)b[i + 7] << 8 | b[i + 8]; }
            break;
        }
        if (len < 2) break;
        i += 2 + len;
    }
    return true;
}

bool mof_make_texture_data(mof_model* m, const uint8_t* bytes, size_t size, mof_wtexture* t, char* err, size_t errlen) {
    uint32_t w = 0, h = 0;
    t->mip_count = 1;
    if (sniff_png(bytes, size, &w, &h)) {
        t->codec = MOF_TEX_PNG;
    } else if (sniff_jpeg(bytes, size, &w, &h)) {
        t->codec = MOF_TEX_JPEG;
    } else {
        int iw, ih, comp;
        if (size > 0x7FFFFFFF) { snprintf(err, errlen, "image too large"); return false; }
        uint8_t* px = stbi_load_from_memory(bytes, (int)size, &iw, &ih, &comp, 4);
        if (!px) { snprintf(err, errlen, "unsupported or corrupt image: %s", stbi_failure_reason()); return false; }
        size_t n = (size_t)iw * ih * 4;
        uint8_t* own = (uint8_t*)mof_model_alloc(m, n);
        if (own) memcpy(own, px, n);
        stbi_image_free(px);
        if (!own) { snprintf(err, errlen, "out of memory"); return false; }
        t->codec = MOF_TEX_RAW_RGBA8;
        t->width = (uint32_t)iw; t->height = (uint32_t)ih;
        t->data = own; t->data_size = n;
        return true;
    }
    if (w > MOF_MAX_TEXTURE_DIM || h > MOF_MAX_TEXTURE_DIM) { snprintf(err, errlen, "image larger than %u pixels", MOF_MAX_TEXTURE_DIM); return false; }
    t->width = w; t->height = h;
    t->data = bytes; t->data_size = size;
    return true;
}

// ------------------------------------------------------- normals / tangents

void mof_generate_normals(const float* P, uint32_t nv, const uint32_t* idx, uint32_t ni, float* N) {
    memset(N, 0, sizeof(float) * 3 * (size_t)nv);
    for (uint32_t i = 0; i + 2 < ni; i += 3) {
        const float *a = P + 3 * (size_t)idx[i], *b = P + 3 * (size_t)idx[i + 1], *c = P + 3 * (size_t)idx[i + 2];
        float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        // cross product length is proportional to the triangle area: area weighting
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        for (int k = 0; k < 3; k++) {
            float* d = N + 3 * (size_t)idx[i + k];
            d[0] += n[0]; d[1] += n[1]; d[2] += n[2];
        }
    }
    for (uint32_t v = 0; v < nv; v++) {
        float* n = N + 3 * (size_t)v;
        float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 1e-20f && isfinite(l)) { n[0] /= l; n[1] /= l; n[2] /= l; }
        else { n[0] = 0; n[1] = 1; n[2] = 0; }
    }
}

void mof_generate_tangents(const float* P, const float* N, const float* UV, uint32_t nv, const uint32_t* idx,
                           uint32_t ni, float* T4) {
    float* acc = (float*)calloc((size_t)nv * 6 + 1, sizeof(float));  // tan1 xyz, tan2 xyz
    if (acc) {
        for (uint32_t i = 0; i + 2 < ni; i += 3) {
            uint32_t i0 = idx[i], i1 = idx[i + 1], i2 = idx[i + 2];
            const float *p0 = P + 3 * (size_t)i0, *p1 = P + 3 * (size_t)i1, *p2 = P + 3 * (size_t)i2;
            const float *t0 = UV + 2 * (size_t)i0, *t1 = UV + 2 * (size_t)i1, *t2 = UV + 2 * (size_t)i2;
            float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
            float du1 = t1[0] - t0[0], du2 = t2[0] - t0[0];
            float dv1 = -(t1[1] - t0[1]), dv2 = -(t2[1] - t0[1]);  // v flipped: image-up
            float den = du1 * dv2 - du2 * dv1;
            if (fabsf(den) < 1e-20f) continue;
            float r = 1.0f / den;
            for (int k = 0; k < 3; k++) {
                float tx = (e1[k] * dv2 - e2[k] * dv1) * r, bx = (e2[k] * du1 - e1[k] * du2) * r;
                uint32_t vs[3] = {i0, i1, i2};
                for (int q = 0; q < 3; q++) { acc[6 * (size_t)vs[q] + k] += tx; acc[6 * (size_t)vs[q] + 3 + k] += bx; }
            }
        }
    }
    for (uint32_t v = 0; v < nv; v++) {
        const float* n = N + 3 * (size_t)v;
        float t[3] = {0, 0, 0}, b[3] = {0, 0, 0};
        if (acc) { for (int k = 0; k < 3; k++) { t[k] = acc[6 * (size_t)v + k]; b[k] = acc[6 * (size_t)v + 3 + k]; } }
        float d = n[0] * t[0] + n[1] * t[1] + n[2] * t[2];
        float o[3] = {t[0] - n[0] * d, t[1] - n[1] * d, t[2] - n[2] * d};
        float l = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
        if (!(l > 1e-12f) || !isfinite(l)) {  // degenerate UVs: any vector perpendicular to n
            float ax[3] = {fabsf(n[0]) < 0.9f ? 1.0f : 0.0f, fabsf(n[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
            float dd = n[0] * ax[0] + n[1] * ax[1] + n[2] * ax[2];
            for (int k = 0; k < 3; k++) o[k] = ax[k] - n[k] * dd;
            l = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
            if (!(l > 0)) { o[0] = 1; o[1] = o[2] = 0; l = 1; }
        }
        for (int k = 0; k < 3; k++) o[k] /= l;
        float cr[3] = {n[1] * o[2] - n[2] * o[1], n[2] * o[0] - n[0] * o[2], n[0] * o[1] - n[1] * o[0]};
        float w = (cr[0] * b[0] + cr[1] * b[1] + cr[2] * b[2]) < 0.0f ? -1.0f : 1.0f;
        float* out = T4 + 4 * (size_t)v;
        out[0] = o[0]; out[1] = o[1]; out[2] = o[2]; out[3] = w;
    }
    free(acc);
}

// ------------------------------------------------------------------ matrices

void mof_mat4_mul(const float a[16], const float b[16], float o[16]) {
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + rr] * b[c * 4 + k];
            r[c * 4 + rr] = s;
        }
    memcpy(o, r, sizeof r);
}

bool mof_mat4_inverse_affine(const float m[16], float o[16]) {
    // upper 3x3 (column-major: a[row][col] = m[col*4+row])
    float a00 = m[0], a10 = m[1], a20 = m[2], a01 = m[4], a11 = m[5], a21 = m[6], a02 = m[8], a12 = m[9], a22 = m[10];
    float c00 = a11 * a22 - a12 * a21, c01 = a12 * a20 - a10 * a22, c02 = a10 * a21 - a11 * a20;
    float det = a00 * c00 + a01 * c01 + a02 * c02;
    if (fabsf(det) < 1e-20f) return false;
    float id = 1.0f / det;
    float i[3][3] = {{c00 * id, (a02 * a21 - a01 * a22) * id, (a01 * a12 - a02 * a11) * id},
                     {c01 * id, (a00 * a22 - a02 * a20) * id, (a02 * a10 - a00 * a12) * id},
                     {c02 * id, (a01 * a20 - a00 * a21) * id, (a00 * a11 - a01 * a10) * id}};
    float t[3] = {m[12], m[13], m[14]};
    memset(o, 0, sizeof(float) * 16);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) o[c * 4 + r] = i[r][c];
        o[12 + r] = -(i[r][0] * t[0] + i[r][1] * t[1] + i[r][2] * t[2]);
    }
    o[15] = 1;
    return true;
}

void mof_mat4_decompose(const float m[16], float t[3], float r[4], float s[3]) {
    t[0] = m[12]; t[1] = m[13]; t[2] = m[14];
    float sx = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    float sy = sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    float sz = sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[4] * (m[1] * m[10] - m[2] * m[9]) + m[8] * (m[1] * m[6] - m[2] * m[5]);
    if (det < 0) sx = -sx;
    s[0] = sx; s[1] = sy; s[2] = sz;
    if (sx == 0 || sy == 0 || sz == 0) { r[0] = r[1] = r[2] = 0; r[3] = 1; return; }
    float R[3][3];  // R[row][col]
    for (int row = 0; row < 3; row++) {
        R[row][0] = m[row] / sx; R[row][1] = m[4 + row] / sy; R[row][2] = m[8 + row] / sz;
    }
    float tr = R[0][0] + R[1][1] + R[2][2];
    if (tr > 0) {
        float S = sqrtf(tr + 1.0f) * 2;
        r[3] = 0.25f * S; r[0] = (R[2][1] - R[1][2]) / S; r[1] = (R[0][2] - R[2][0]) / S; r[2] = (R[1][0] - R[0][1]) / S;
    } else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) {
        float S = sqrtf(1.0f + R[0][0] - R[1][1] - R[2][2]) * 2;
        r[3] = (R[2][1] - R[1][2]) / S; r[0] = 0.25f * S; r[1] = (R[0][1] + R[1][0]) / S; r[2] = (R[0][2] + R[2][0]) / S;
    } else if (R[1][1] > R[2][2]) {
        float S = sqrtf(1.0f + R[1][1] - R[0][0] - R[2][2]) * 2;
        r[3] = (R[0][2] - R[2][0]) / S; r[0] = (R[0][1] + R[1][0]) / S; r[1] = 0.25f * S; r[2] = (R[1][2] + R[2][1]) / S;
    } else {
        float S = sqrtf(1.0f + R[2][2] - R[0][0] - R[1][1]) * 2;
        r[3] = (R[1][0] - R[0][1]) / S; r[0] = (R[0][2] + R[2][0]) / S; r[1] = (R[1][2] + R[2][1]) / S; r[2] = 0.25f * S;
    }
}

bool mof_import_file(const char* path, mof_model* out, char* err, size_t errlen) {
    const char* dot = strrchr(path, '.');
    char ext[8] = {0};
    if (dot) for (size_t i = 0; i + 1 < sizeof ext && dot[1 + i]; i++) ext[i] = (char)((dot[1 + i] >= 'A' && dot[1 + i] <= 'Z') ? dot[1 + i] + 32 : dot[1 + i]);
    if (!strcmp(ext, "gltf") || !strcmp(ext, "glb")) return mof_import_gltf(path, out, err, errlen);
    if (!strcmp(ext, "obj")) return mof_import_obj(path, out, err, errlen);
    snprintf(err, errlen, "unsupported input extension (expected .gltf .glb .obj): %s", path);
    return false;
}
