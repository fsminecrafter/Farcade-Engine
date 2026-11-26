// mofconv: convert glTF / OBJ models to .mof and inspect .mof files.
//
//   mofconv input.(gltf|glb|obj) output.mof [--info] [--max-triangles N] [--lods N1,N2,...]
//   mofconv --info file.mof
//
// --max-triangles N  reduces the model to about N triangles (engine/mof/simplify.h)
// --lods N1,N2       also writes output_lod1.mof, output_lod2.mof ... with about N1, N2 ... triangles
//                    (terrain foliage switches to them with distance, docs/terrain.md)
// --alpha NAME=FILE  materials whose name contains NAME get FILE (a grey image) as the alpha of their base
//                    color texture and an alpha mask (cutoff 0.5): leaf cards whose alpha comes as a separate
//                    map (Poly Haven's <part>_alpha). Repeatable.
// --max-texture N    textures larger than N pixels a side are halved until they fit (box filter); opaque
//                    ones are stored as JPEG, ones with alpha as PNG
// --split            one file per root node with a mesh (variants laid out side by side in one asset), each
//                    moved to the origin and holding only its own materials and textures: output_0.mof,
//                    output_1.mof ... (with their LODs output_0_lod1.mof ...)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mof/mof.h"
#include "mof/mof_import.h"
#include "mof/simplify.h"
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#include "mof_info.h"

static bool ends_with(const char* s, const char* suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    if (a < b) return false;
    for (size_t i = 0; i < b; i++) {
        char c = s[a - b + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != suffix[i]) return false;
    }
    return true;
}

static int show_info_file(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "mofconv: cannot open %s\n", path); return 1; }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 0) { fclose(fp); return 1; }
    size_t cap = ((size_t)n + 16) & ~(size_t)15;
    void* buf = aligned_alloc(16, cap ? cap : 16);  // reader needs a 16-byte aligned buffer
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) { fprintf(stderr, "mofconv: read error\n"); fclose(fp); free(buf); return 1; }
    fclose(fp);
    mof_file f;
    char err[256];
    if (!mof_open(buf, (size_t)n, &f, err, sizeof err)) {
        fprintf(stderr, "mofconv: %s: invalid .mof: %s\n", path, err);
        free(buf);
        return 1;
    }
    printf("%s\n", path);
    mof_print_info(&f, stdout);
    free(buf);
    return 0;
}

static void* read_all(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* b = sz > 0 ? malloc((size_t)sz) : nullptr;
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = nullptr; }
    fclose(f);
    *n = b ? (size_t)sz : 0;
    return b;
}

static void png_out(void* ctx, void* data, int n) {
    uint8_t** pp = (uint8_t**)ctx;
    size_t have = pp[1] ? *(size_t*)pp[1] : 0;
    uint8_t* nb = (uint8_t*)realloc(pp[0], have + (size_t)n);
    if (!nb) return;
    memcpy(nb + have, data, (size_t)n);
    pp[0] = nb;
    *(size_t*)pp[1] = have + (size_t)n;
}

// Puts `alpha_path` into the base color texture of every material whose name contains `name` (as a PNG) and
// makes the material an alpha mask. The new texture data is kept in `keep` until the model is written.
static int apply_alpha(mof_wdesc* d, const char* name, const char* alpha_path, void** keep, int* nkeep) {
    size_t an = 0;
    void* ab = read_all(alpha_path, &an);
    int aw = 0, ah = 0, ac = 0;
    uint8_t* alpha = ab ? stbi_load_from_memory((const uint8_t*)ab, (int)an, &aw, &ah, &ac, 1) : nullptr;
    free(ab);
    if (!alpha) { fprintf(stderr, "mofconv: cannot read the alpha map %s\n", alpha_path); return -1; }
    int done = 0;
    mof_wmaterial* mats = (mof_wmaterial*)d->materials;
    mof_wtexture* texs = (mof_wtexture*)d->textures;
    for (uint32_t m = 0; m < d->material_count; m++) {
        if (!mats[m].name || !strstr(mats[m].name, name)) continue;
        int t = mats[m].tex[MOF_SLOT_BASE_COLOR].texture;
        if (t < 0 || (uint32_t)t >= d->texture_count) continue;
        mof_wtexture& tx = texs[t];
        int w = 0, h = 0, c = 0;
        uint8_t* rgba = nullptr;
        if (tx.codec == MOF_TEX_RAW_RGBA8) {
            w = (int)tx.width; h = (int)tx.height;
            rgba = (uint8_t*)malloc((size_t)w * h * 4);
            if (rgba) memcpy(rgba, tx.data, (size_t)w * h * 4);
        } else {
            rgba = stbi_load_from_memory(tx.data, (int)tx.data_size, &w, &h, &c, 4);
        }
        if (!rgba) continue;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int sx = x * aw / w, sy = y * ah / h;
                rgba[((size_t)y * w + x) * 4 + 3] = alpha[(size_t)sy * aw + sx];
            }
        uint8_t* png = nullptr;
        size_t png_n = 0;
        uint8_t* ctx[2] = { nullptr, (uint8_t*)&png_n };
        stbi_write_png_to_func(png_out, ctx, w, h, 4, rgba, w * 4);
        png = ctx[0];
        if (tx.codec == MOF_TEX_RAW_RGBA8) free(rgba); else stbi_image_free(rgba);
        if (!png) continue;
        keep[(*nkeep)++] = png;
        tx.codec = MOF_TEX_PNG;
        tx.width = (uint32_t)w; tx.height = (uint32_t)h; tx.mip_count = 1;
        tx.data = png; tx.data_size = png_n;
        mats[m].alpha_mode = MOF_ALPHA_MASK;
        mats[m].alpha_cutoff = 0.5f;
        mats[m].double_sided = 1;
        done++;
        printf("alpha: %s <- %s\n", mats[m].name, alpha_path);
    }
    stbi_image_free(alpha);
    return done;
}

// Halves every texture larger than `max_side` until it fits. New data is kept in `keep`.
static void shrink_textures(mof_wdesc* d, int max_side, void** keep, int* nkeep, int cap) {
    mof_wtexture* texs = (mof_wtexture*)d->textures;
    for (uint32_t t = 0; t < d->texture_count && *nkeep < cap; t++) {
        mof_wtexture& tx = texs[t];
        if ((int)tx.width <= max_side && (int)tx.height <= max_side && tx.width && tx.height) continue;
        int w = 0, h = 0, c = 0;
        uint8_t* px;
        if (tx.codec == MOF_TEX_RAW_RGBA8) {
            w = (int)tx.width; h = (int)tx.height;
            px = (uint8_t*)malloc((size_t)w * h * 4);
            if (px) memcpy(px, tx.data, (size_t)w * h * 4);
        } else {
            px = stbi_load_from_memory(tx.data, (int)tx.data_size, &w, &h, &c, 4);
        }
        if (!px) continue;
        while ((w > max_side || h > max_side) && w > 1 && h > 1) {
            int nw = w / 2, nh = h / 2;
            for (int y = 0; y < nh; y++)
                for (int x = 0; x < nw; x++)
                    for (int k = 0; k < 4; k++) {
                        int sum = px[((size_t)(2 * y) * w + 2 * x) * 4 + k] + px[((size_t)(2 * y) * w + 2 * x + 1) * 4 + k] +
                                  px[((size_t)(2 * y + 1) * w + 2 * x) * 4 + k] + px[((size_t)(2 * y + 1) * w + 2 * x + 1) * 4 + k];
                        px[((size_t)y * nw + x) * 4 + k] = (uint8_t)((sum + 2) / 4);
                    }
            w = nw;
            h = nh;
        }
        bool alpha = false;
        for (size_t i = 0; i < (size_t)w * h && !alpha; i++) alpha = px[i * 4 + 3] < 250;
        size_t out_n = 0;
        uint8_t* ctx[2] = { nullptr, (uint8_t*)&out_n };
        if (alpha) stbi_write_png_to_func(png_out, ctx, w, h, 4, px, w * 4);
        else stbi_write_jpg_to_func(png_out, ctx, w, h, 4, px, 90);
        free(px);
        if (!ctx[0]) continue;
        keep[(*nkeep)++] = ctx[0];
        tx.codec = alpha ? MOF_TEX_PNG : MOF_TEX_JPEG;
        tx.width = (uint32_t)w; tx.height = (uint32_t)h; tx.mip_count = 1;
        tx.data = ctx[0];
        tx.data_size = out_n;
    }
}

// One root node's mesh as a model of its own at the origin, with only the materials and textures it uses.
// The arrays live in `parts` (freed by the caller).
struct SplitDesc {
    mof_wdesc d;
    mof_wmesh mesh;
    mof_wnode node;
    mof_wsubmesh* subs = nullptr;
    mof_wmaterial* mats = nullptr;
    mof_wtexture* texs = nullptr;
    ~SplitDesc() { free(subs); free(mats); free(texs); }
};

static bool split_node(const mof_wdesc* in, uint32_t node, SplitDesc* out) {
    const mof_wnode& n = in->nodes[node];
    if (n.mesh < 0 || (uint32_t)n.mesh >= in->mesh_count) return false;
    out->d = *in;
    out->mesh = in->meshes[n.mesh];
    out->node = n;
    out->node.parent = -1;
    out->node.mesh = 0;
    out->node.translation[0] = out->node.translation[1] = out->node.translation[2] = 0.0f;
    int* mat_map = (int*)malloc(sizeof(int) * (in->material_count + 1));
    int* tex_map = (int*)malloc(sizeof(int) * (in->texture_count + 1));
    for (uint32_t i = 0; i < in->material_count; i++) mat_map[i] = -1;
    for (uint32_t i = 0; i < in->texture_count; i++) tex_map[i] = -1;
    uint32_t nsub = out->mesh.submesh_count ? out->mesh.submesh_count : 0;
    out->subs = (mof_wsubmesh*)malloc(sizeof(mof_wsubmesh) * (nsub ? nsub : 1));
    out->mats = (mof_wmaterial*)malloc(sizeof(mof_wmaterial) * (in->material_count ? in->material_count : 1));
    out->texs = (mof_wtexture*)malloc(sizeof(mof_wtexture) * (in->texture_count ? in->texture_count : 1));
    uint32_t nm = 0, nt = 0;
    for (uint32_t k = 0; k < nsub; k++) {
        out->subs[k] = out->mesh.submeshes[k];
        int m = out->subs[k].material;
        if (m < 0 || (uint32_t)m >= in->material_count) continue;
        if (mat_map[m] < 0) {
            mat_map[m] = (int)nm;
            mof_wmaterial mat = in->materials[m];
            for (int sl = 0; sl < (int)MOF_SLOT_COUNT; sl++) {
                int t = mat.tex[sl].texture;
                if (t < 0 || (uint32_t)t >= in->texture_count) continue;
                if (tex_map[t] < 0) { tex_map[t] = (int)nt; out->texs[nt++] = in->textures[t]; }
                mat.tex[sl].texture = tex_map[t];
            }
            out->mats[nm++] = mat;
        }
        out->subs[k].material = mat_map[m];
    }
    free(mat_map);
    free(tex_map);
    out->mesh.submeshes = nsub ? out->subs : nullptr;
    out->d.mesh_count = 1; out->d.meshes = &out->mesh;
    out->d.material_count = nm; out->d.materials = out->mats;
    out->d.texture_count = nt; out->d.textures = out->texs;
    out->d.node_count = 1; out->d.nodes = &out->node;
    out->d.bone_count = 0; out->d.bones = nullptr;
    return true;
}

static void usage() {
    fprintf(stderr,
            "usage: mofconv input.(gltf|glb|obj) output.mof [--info] [--max-triangles N] [--lods N1,N2,...]\n"
            "       mofconv --info file.mof\n");
}

// Writes desc reduced to about `triangles` (0 = as it is) to path.
static bool write_reduced(const mof_wdesc* desc, uint64_t triangles, const char* path) {
    mof_simplified red;
    memset(&red, 0, sizeof red);
    const mof_wdesc* use = desc;
    char err[512];
    uint64_t have = mof_desc_triangles(desc);
    if (triangles && have > triangles) {
        if (!mof_simplify(desc, (float)((double)triangles / (double)have), &red, err, sizeof err)) {
            fprintf(stderr, "mofconv: simplify: %s\n", err);
            return false;
        }
        use = &red.desc;
        printf("%s: %llu -> %llu triangles\n", path, (unsigned long long)red.triangles_before, (unsigned long long)red.triangles_after);
    }
    mof_buffer buf;
    bool ok = mof_write(use, &buf, err, sizeof err);
    mof_simplified_free(&red);
    if (!ok) { fprintf(stderr, "mofconv: write failed: %s\n", err); return false; }
    FILE* fp = fopen(path, "wb");
    if (!fp || fwrite(buf.data, 1, buf.size, fp) != buf.size) {
        fprintf(stderr, "mofconv: cannot write %s\n", path);
        if (fp) fclose(fp);
        mof_buffer_free(&buf);
        return false;
    }
    fclose(fp);
    printf("wrote %s (%zu bytes)\n", path, buf.size);
    mof_buffer_free(&buf);
    return true;
}

int main(int argc, char** argv) {
    const char* pos[2] = {nullptr, nullptr};
    int npos = 0;
    bool info = false;
    uint64_t max_tris = 0;
    uint64_t lods[8];
    int nlods = 0;
    const char* alphas[16];
    int nalpha = 0;
    bool split = false;
    int max_texture = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--info")) info = true;
        else if (!strcmp(argv[i], "--max-triangles") && i + 1 < argc) max_tris = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--split")) split = true;
        else if (!strcmp(argv[i], "--max-texture") && i + 1 < argc) max_texture = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--alpha") && i + 1 < argc) { if (nalpha < 16) alphas[nalpha++] = argv[++i]; else ++i; }
        else if (!strcmp(argv[i], "--lods") && i + 1 < argc) {
            for (char* p = argv[++i]; *p && nlods < 8;) {
                lods[nlods++] = strtoull(p, &p, 10);
                while (*p == ',' || *p == ' ') p++;
            }
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (npos < 2) pos[npos++] = argv[i];
        else { usage(); return 2; }
    }
    if (npos == 1 && info && ends_with(pos[0], ".mof")) return show_info_file(pos[0]);
    if (npos != 2) { usage(); return 2; }

    mof_model model;
    char err[512];
    if (!mof_import_file(pos[0], &model, err, sizeof err)) {
        fprintf(stderr, "mofconv: %s\n", err);
        return 1;
    }
    if (model.warnings[0]) fprintf(stderr, "mofconv: warnings:\n%s", model.warnings);
    void* keep[256];
    int nkeep = 0;
    for (int a = 0; a < nalpha; a++) {
        char name[128];
        const char* eq = strchr(alphas[a], '=');
        if (!eq || eq == alphas[a] || (size_t)(eq - alphas[a]) >= sizeof name) { fprintf(stderr, "mofconv: --alpha wants NAME=FILE\n"); return 2; }
        snprintf(name, sizeof name, "%.*s", (int)(eq - alphas[a]), alphas[a]);
        int n = apply_alpha(&model.desc, name, eq + 1, keep, &nkeep);
        if (n < 0) return 1;
        if (n == 0) fprintf(stderr, "mofconv: warning: no material with a base color texture matches '%s'\n", name);
        if (nkeep > 240) break;
    }
    if (max_texture > 0) shrink_textures(&model.desc, max_texture, keep, &nkeep, 250);
    // The files to write: the model, or each root node's mesh (--split).
    size_t n = strlen(pos[1]);
    if (n > 4 && ends_with(pos[1], ".mof")) n -= 4;
    bool ok = true;
    auto write_set = [&](const mof_wdesc* d, const char* base, size_t blen) {
        char path[1024];
        snprintf(path, sizeof path, "%.*s.mof", (int)blen, base);
        ok = ok && write_reduced(d, max_tris, path);
        for (int l = 0; ok && l < nlods; l++) {
            snprintf(path, sizeof path, "%.*s_lod%d.mof", (int)blen, base, l + 1);
            ok = write_reduced(d, lods[l], path);
        }
    };
    bool any_root_mesh = false;
    if (split)
        for (uint32_t i = 0; i < model.desc.node_count; i++) any_root_mesh |= model.desc.nodes[i].parent < 0 && model.desc.nodes[i].mesh >= 0;
    if (!split || model.desc.node_count == 0 || !any_root_mesh) {
        if (split) fprintf(stderr, "mofconv: --split: no root nodes with meshes, writing one file\n");
        write_set(&model.desc, pos[1], n);
    } else {
        int k = 0;
        for (uint32_t i = 0; i < model.desc.node_count && ok; i++) {
            const mof_wnode& nd = model.desc.nodes[i];
            if (nd.parent >= 0 || nd.mesh < 0) continue;
            SplitDesc sd;
            if (!split_node(&model.desc, i, &sd)) continue;
            char base[1024];
            snprintf(base, sizeof base, "%.*s_%d", (int)n, pos[1], k++);
            write_set(&sd.d, base, strlen(base));
        }
    }
    mof_model_free(&model);
    for (int k = 0; k < nkeep; k++) free(keep[k]);
    if (!ok) return 1;
    return info ? show_info_file(pos[1]) : 0;
}
