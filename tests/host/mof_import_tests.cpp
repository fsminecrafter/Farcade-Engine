// Importer tests: a generated skinned, textured glTF (.gltf with data URIs and
// an external JPEG, and the same model as .glb with the PNG in a buffer view),
// an OBJ + MTL, error handling, and mutation fuzzing of the importers.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "mof/mof.h"
#include "mof/mof_import.h"
#include "mof_check.h"
#include "mof/simplify.h"
#include "stb/stb_image_write.h"

static const char* kTmp = "build/mof-test-tmp";

// ------------------------------------------------------------------ helpers

struct Bytes { uint8_t* d = nullptr; size_t n = 0, cap = 0; };
static void put(Bytes& b, const void* p, size_t n) {
    if (n == 0) return;
    if (b.n + n > b.cap) { b.cap = (b.n + n) * 2 + 64; b.d = (uint8_t*)realloc(b.d, b.cap); }
    if (n) memcpy(b.d + b.n, p, n);
    b.n += n;
}
static void pad4(Bytes& b) { static const uint8_t z[4] = {0}; put(b, z, (4 - (b.n & 3)) & 3); }
static void write_file(const char* path, const void* d, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fwrite(d, 1, n, f);
    fclose(f);
}
static void write_text(const char* path, const char* s) { write_file(path, s, strlen(s)); }
static void png_cb(void* ctx, void* data, int n) { put(*(Bytes*)ctx, data, (size_t)n); }

static char* base64(const uint8_t* d, size_t n) {
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char* o = (char*)malloc((n + 2) / 3 * 4 + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o[k++] = A[v >> 18 & 63]; o[k++] = A[v >> 12 & 63];
        o[k++] = i + 1 < n ? A[v >> 6 & 63] : '='; o[k++] = i + 2 < n ? A[v & 63] : '=';
    }
    o[k] = 0;
    return o;
}

// Loads a .mof image produced by the importer + writer, in an aligned buffer.
struct Loaded { mof_buffer buf; mof_file f; bool ok = false; mof_model model; char err[512] = {0}; };
static bool import_and_write(const char* path, Loaded* L, bool want_warnings = false) {
    (void)want_warnings;
    if (!mof_import_file(path, &L->model, L->err, sizeof L->err)) return false;
    bool w = mof_write(&L->model.desc, &L->buf, L->err, sizeof L->err);
    if (!w) { mof_model_free(&L->model); return false; }
    L->ok = mof_open(L->buf.data, L->buf.size, &L->f, L->err, sizeof L->err);
    return L->ok;
}
static void release(Loaded* L) { mof_buffer_free(&L->buf); mof_model_free(&L->model); }

// ------------------------------------------------------------------ glTF

static uint8_t g_png[4096]; static size_t g_png_n;
static uint8_t g_jpg[4096]; static size_t g_jpg_n;

static void make_images() {
    uint8_t px[4 * 2 * 4];
    for (int i = 0; i < 8; i++) { px[i * 4] = (uint8_t)(i * 30); px[i * 4 + 1] = 100; px[i * 4 + 2] = (uint8_t)(255 - i * 20); px[i * 4 + 3] = 255; }
    Bytes b;
    stbi_write_png_to_func(png_cb, &b, 4, 2, 4, px, 4 * 4);
    memcpy(g_png, b.d, b.n); g_png_n = b.n; free(b.d);
    uint8_t jp[8 * 8 * 3];
    for (int i = 0; i < 64; i++) { jp[i * 3] = (uint8_t)(i * 4); jp[i * 3 + 1] = 128; jp[i * 3 + 2] = (uint8_t)(255 - i * 4); }
    Bytes j;
    stbi_write_jpg_to_func(png_cb, &j, 8, 8, 3, jp, 90);
    memcpy(g_jpg, j.d, j.n); g_jpg_n = j.n; free(j.d);
}

static const float kP1[6 * 3] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 2, 0, 1, 2, 0};
static const float kUV[6 * 2] = {0, 1, 1, 1, 0, 0.5f, 1, 0.5f, 0, 0, 1, 0};
static const uint8_t kJ[6 * 4] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const float kW[6 * 4] = {1, 0, 0, 0, 1, 0, 0, 0, 0.5f, 0.5f, 0, 0, 0.5f, 0.5f, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
static const uint16_t kI1[12] = {0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4};
static const float kP2[3 * 3] = {0, 0, 5, 1, 0, 5, 0, 1, 5};
static const float kC2[3 * 3] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
static const uint8_t kI2[3] = {0, 1, 2};
static const float kIBM[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1,   // tip (skin joint 0)
                               1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // root (skin joint 1)

static void write_gltf(const char* path, bool glb) {
    Bytes bin;
    size_t off[10], len[10];
    auto add = [&](int i, const void* p, size_t n) { pad4(bin); off[i] = bin.n; len[i] = n; put(bin, p, n); };
    add(0, kP1, sizeof kP1); add(1, kUV, sizeof kUV); add(2, kJ, sizeof kJ); add(3, kW, sizeof kW); add(4, kI1, sizeof kI1);
    add(5, kP2, sizeof kP2); add(6, kC2, sizeof kC2); add(7, kI2, sizeof kI2); add(8, kIBM, sizeof kIBM);
    int nviews = 9;
    if (glb) { add(9, g_png, g_png_n); nviews = 10; }
    pad4(bin);

    Bytes js;
    char* b64png = base64(g_png, g_png_n);
    char* b64bin = base64(bin.d, bin.n);
    char tmp[1024];
    auto S = [&](const char* s) { put(js, s, strlen(s)); };
    S("{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0,2]}],");
    S("\"nodes\":[{\"name\":\"skinned_mesh\",\"mesh\":0,\"skin\":0,\"translation\":[1,2,3]},"
      "{\"name\":\"tip\",\"translation\":[0,1,0]},{\"name\":\"root\",\"children\":[1]}],");
    S("\"skins\":[{\"joints\":[1,2],\"inverseBindMatrices\":8}],");  // child before parent on purpose
    S("\"meshes\":[{\"name\":\"bar\",\"primitives\":["
      "{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1,\"JOINTS_0\":2,\"WEIGHTS_0\":3},\"indices\":4,\"material\":0},"
      "{\"attributes\":{\"POSITION\":5,\"COLOR_0\":6},\"indices\":7,\"material\":1}]}],");
    S("\"materials\":[{\"name\":\"skin_mat\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,0.5,0.25,1],"
      "\"baseColorTexture\":{\"index\":0},\"metallicFactor\":0.25,\"roughnessFactor\":0.75},\"normalTexture\":{\"index\":1,\"scale\":2}},"
      "{\"name\":\"cutout\",\"alphaMode\":\"MASK\",\"alphaCutoff\":0.3,\"doubleSided\":true,\"emissiveFactor\":[0.1,0.2,0.3]}],");
    S("\"textures\":[{\"name\":\"albedo\",\"source\":0,\"sampler\":0},{\"source\":1}],");
    S("\"samplers\":[{\"magFilter\":9728,\"minFilter\":9987,\"wrapS\":33071,\"wrapT\":33648}],");
    if (glb) snprintf(tmp, sizeof tmp, "\"images\":[{\"bufferView\":9,\"mimeType\":\"image/png\"},{\"name\":\"normal_img\",\"uri\":\"my%%20tex.jpg\"}],");
    else snprintf(tmp, sizeof tmp, "\"images\":[{\"uri\":\"data:image/png;base64,");
    S(tmp);
    if (!glb) { S(b64png); S("\"},{\"name\":\"normal_img\",\"uri\":\"my%20tex.jpg\"}],"); }
    S("\"bufferViews\":[");
    for (int i = 0; i < nviews; i++) { snprintf(tmp, sizeof tmp, "%s{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}", i ? "," : "", off[i], len[i]); S(tmp); }
    S("],\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":6,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,2,0]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":6,\"type\":\"VEC2\"},"
      "{\"bufferView\":2,\"componentType\":5121,\"count\":6,\"type\":\"VEC4\"},"
      "{\"bufferView\":3,\"componentType\":5126,\"count\":6,\"type\":\"VEC4\"},"
      "{\"bufferView\":4,\"componentType\":5123,\"count\":12,\"type\":\"SCALAR\"},"
      "{\"bufferView\":5,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,5],\"max\":[1,1,5]},"
      "{\"bufferView\":6,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
      "{\"bufferView\":7,\"componentType\":5121,\"count\":3,\"type\":\"SCALAR\"},"
      "{\"bufferView\":8,\"componentType\":5126,\"count\":2,\"type\":\"MAT4\"}],");
    if (glb) snprintf(tmp, sizeof tmp, "\"buffers\":[{\"byteLength\":%zu}]}", bin.n);
    else snprintf(tmp, sizeof tmp, "\"buffers\":[{\"byteLength\":%zu,\"uri\":\"data:application/octet-stream;base64,", bin.n);
    S(tmp);
    if (!glb) { S(b64bin); S("\"}]}"); }

    if (!glb) {
        write_file(path, js.d, js.n);
    } else {
        while (js.n & 3) put(js, " ", 1);
        Bytes out;
        uint32_t hdr[3] = {0x46546C67u, 2, (uint32_t)(12 + 8 + js.n + 8 + bin.n)};
        put(out, hdr, 12);
        uint32_t ch[2] = {(uint32_t)js.n, 0x4E4F534Au};
        put(out, ch, 8); put(out, js.d, js.n);
        ch[0] = (uint32_t)bin.n; ch[1] = 0x004E4942u;
        put(out, ch, 8); put(out, bin.d, bin.n);
        write_file(path, out.d, out.n);
        free(out.d);
    }
    free(b64png); free(b64bin); free(js.d); free(bin.d);
}

static void check_gltf(const mof_file& f, const char* what) {
    printf("  checking %s\n", what);
    CHECK_EQ(f.mesh_count, 1); CHECK_EQ(f.material_count, 2); CHECK_EQ(f.texture_count, 2);
    CHECK_EQ(f.bone_count, 2); CHECK_EQ(f.node_count, 3); CHECK_EQ(f.submesh_count, 2);
    if (f.mesh_count != 1 || f.bone_count != 2 || f.node_count != 3 || f.texture_count != 2 || f.material_count != 2) return;
    const mof_mesh& m = f.meshes[0];
    CHECK_STR(mof_string(&f, m.name), "bar");
    CHECK_EQ(m.vertex_count, 9); CHECK_EQ(m.index_count, 15); CHECK_EQ(m.submesh_count, 2);
    CHECK_EQ(m.stream_mask, MOF_STREAM_NORMAL | MOF_STREAM_TANGENT | MOF_STREAM_UV0 | MOF_STREAM_COLOR | MOF_STREAM_SKIN);
    const mof_submesh* s = f.submeshes + m.first_submesh;
    CHECK_EQ(s[0].first_index, 0); CHECK_EQ(s[0].index_count, 12); CHECK_EQ(s[0].material, 0);
    CHECK_EQ(s[1].first_index, 12); CHECK_EQ(s[1].index_count, 3); CHECK_EQ(s[1].material, 1);
    // positions + indices (second primitive's indices are offset by 6)
    const float* P = mof_mesh_positions(&f, &m);
    CHECK(memcmp(P, kP1, sizeof kP1) == 0 && memcmp(P + 18, kP2, sizeof kP2) == 0);
    for (int i = 0; i < 12; i++) CHECK_EQ(mof_mesh_index(&f, &m, (uint32_t)i), kI1[i]);
    for (int i = 0; i < 3; i++) CHECK_EQ(mof_mesh_index(&f, &m, 12u + i), 6u + kI2[i]);
    CHECK_NEAR(m.bounds_min[0], 0); CHECK_NEAR(m.bounds_max[1], 2); CHECK_NEAR(m.bounds_max[2], 5);
    // normals were absent: generated, area weighted, +Z for these CCW triangles
    const float* N = mof_mesh_normals(&f, &m);
    for (int v = 0; v < 9; v++) { CHECK_NEAR(N[v * 3], 0); CHECK_NEAR(N[v * 3 + 1], 0); CHECK_NEAR(N[v * 3 + 2], 1); }
    // uv0 copied (prim 2 has none: zeros), v is not flipped for glTF
    const float* UV = mof_mesh_uv0(&f, &m);
    CHECK(memcmp(UV, kUV, sizeof kUV) == 0);
    CHECK_NEAR(UV[12], 0); CHECK_NEAR(UV[17], 0);
    // tangents generated for the normal-mapped primitive: +X, handedness +1
    const float* T = mof_mesh_tangents(&f, &m);
    for (int v = 0; v < 6; v++) { CHECK_NEAR(T[v * 4], 1); CHECK_NEAR(T[v * 4 + 1], 0); CHECK_NEAR(T[v * 4 + 2], 0); CHECK_NEAR(T[v * 4 + 3], 1); }
    for (int v = 6; v < 9; v++) CHECK_NEAR(T[v * 4] * N[v * 3] + T[v * 4 + 1] * N[v * 3 + 1] + T[v * 4 + 2] * N[v * 3 + 2], 0);
    // colors: white where absent, converted floats otherwise
    const uint8_t* C = mof_mesh_colors(&f, &m);
    for (int i = 0; i < 24; i++) CHECK_EQ(C[i], 255);
    CHECK(C[24] == 255 && C[25] == 0 && C[28] == 0 && C[29] == 255 && C[34] == 255 && C[35] == 255);
    // skin: the file lists joints [tip, root]; bones are written parent first [root, tip], so
    // every joint index has to be remapped (skin joint 0 -> bone 1, skin joint 1 -> bone 0)
    const uint16_t* J = mof_mesh_joints(&f, &m);
    const float* W = (const float*)mof_mesh_weights(&f, &m);
    CHECK_EQ(m.weight_format, MOF_WEIGHTS_F32);
    static const uint16_t eJ[9 * 4] = {0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    CHECK(memcmp(J, eJ, sizeof eJ) == 0);
    CHECK(memcmp(W, kW, sizeof kW) == 0);
    CHECK_NEAR(W[24], 1); CHECK_NEAR(W[25], 0);
    // skeleton
    CHECK_STR(mof_string(&f, f.bones[0].name), "root"); CHECK_STR(mof_string(&f, f.bones[1].name), "tip");
    CHECK_EQ(f.bones[0].parent, -1); CHECK_EQ(f.bones[1].parent, 0);
    CHECK_NEAR(f.bones[1].translation[1], 1); CHECK_NEAR(f.bones[0].translation[1], 0); CHECK_NEAR(f.bones[1].rotation[3], 1); CHECK_NEAR(f.bones[1].scale[2], 1);
    CHECK_NEAR(f.bones[1].inverse_bind[13], -1); CHECK_NEAR(f.bones[1].inverse_bind[0], 1);  // reordered with the bones
    CHECK_NEAR(f.bones[0].inverse_bind[13], 0); CHECK_NEAR(f.bones[0].inverse_bind[15], 1);
    // nodes: parents before children (depth order), bones point at their nodes
    CHECK_STR(mof_string(&f, f.nodes[0].name), "skinned_mesh");
    CHECK_STR(mof_string(&f, f.nodes[1].name), "root");
    CHECK_STR(mof_string(&f, f.nodes[2].name), "tip");
    CHECK_EQ(f.nodes[0].parent, -1); CHECK_EQ(f.nodes[1].parent, -1); CHECK_EQ(f.nodes[2].parent, 1);
    CHECK_EQ(f.nodes[0].mesh, 0); CHECK_EQ(f.nodes[1].mesh, -1);
    CHECK(f.nodes[0].flags & MOF_NODE_SKINNED);
    CHECK_NEAR(f.nodes[0].translation[0], 1); CHECK_NEAR(f.nodes[0].translation[1], 2); CHECK_NEAR(f.nodes[0].translation[2], 3);
    CHECK_EQ(f.bones[0].node, 1); CHECK_EQ(f.bones[1].node, 2);
    // materials
    const mof_material& a = f.materials[0];
    CHECK_STR(mof_string(&f, a.name), "skin_mat");
    CHECK_NEAR(a.base_color[1], 0.5f); CHECK_NEAR(a.base_color[2], 0.25f); CHECK_NEAR(a.metallic, 0.25f); CHECK_NEAR(a.roughness, 0.75f);
    CHECK_EQ(a.tex[MOF_SLOT_BASE_COLOR].texture, 0); CHECK_EQ(a.tex[MOF_SLOT_NORMAL].texture, 1);
    CHECK_EQ(a.tex[MOF_SLOT_EMISSIVE].texture, MOF_NONE); CHECK_NEAR(a.normal_scale, 2);
    CHECK_EQ((a.flags & MOF_MATERIAL_ALPHA_MASK) >> MOF_MATERIAL_ALPHA_SHIFT, MOF_ALPHA_OPAQUE);
    const mof_material& c = f.materials[1];
    CHECK_EQ((c.flags & MOF_MATERIAL_ALPHA_MASK) >> MOF_MATERIAL_ALPHA_SHIFT, MOF_ALPHA_MASK);
    CHECK(c.flags & MOF_MATERIAL_DOUBLE_SIDED); CHECK_NEAR(c.alpha_cutoff, 0.3f);
    CHECK_NEAR(c.emissive[0], 0.1f); CHECK_NEAR(c.emissive[2], 0.3f);
    // textures: PNG/JPEG bytes kept as they were
    uint64_t n;
    const mof_texture& t0 = f.textures[0];
    CHECK_STR(mof_string(&f, t0.name), "albedo");
    CHECK_EQ(t0.codec, MOF_TEX_PNG); CHECK_EQ(t0.width, 4); CHECK_EQ(t0.height, 2);
    CHECK_EQ(t0.wrap_s, MOF_WRAP_CLAMP); CHECK_EQ(t0.wrap_t, MOF_WRAP_MIRROR);
    CHECK_EQ(t0.mag_filter, MOF_FILTER_NEAREST); CHECK_EQ(t0.min_filter, MOF_FILTER_LINEAR); CHECK_EQ(t0.mip_filter, MOF_MIP_LINEAR);
    const uint8_t* b = mof_texture_bytes(&f, 0, &n);
    CHECK(n == g_png_n && memcmp(b, g_png, n) == 0);
    const mof_texture& t1 = f.textures[1];
    CHECK_STR(mof_string(&f, t1.name), "normal_img");
    CHECK_EQ(t1.codec, MOF_TEX_JPEG); CHECK_EQ(t1.width, 8); CHECK_EQ(t1.height, 8);
    CHECK_EQ(t1.wrap_s, MOF_WRAP_REPEAT);
    b = mof_texture_bytes(&f, 1, &n);
    CHECK(n == g_jpg_n && memcmp(b, g_jpg, n) == 0);
}

static void test_gltf() {
    char path[256], p2[256];
    snprintf(p2, sizeof p2, "%s/my tex.jpg", kTmp);
    write_file(p2, g_jpg, g_jpg_n);
    snprintf(path, sizeof path, "%s/skinned.gltf", kTmp);
    write_gltf(path, false);
    Loaded L;
    CHECK_MSG(import_and_write(path, &L), L.err);
    if (L.ok) { check_gltf(L.f, ".gltf (data URIs + external jpg)"); release(&L); }

    snprintf(path, sizeof path, "%s/skinned.glb", kTmp);
    write_gltf(path, true);
    Loaded G;
    CHECK_MSG(import_and_write(path, &G), G.err);
    if (G.ok) { check_gltf(G.f, ".glb (png in buffer view)"); release(&G); }

    // a missing external image is a warning, not an error: the texture is dropped
    remove(p2);
    snprintf(path, sizeof path, "%s/skinned.gltf", kTmp);
    Loaded M;
    CHECK_MSG(import_and_write(path, &M), M.err);
    if (M.ok) {
        CHECK(strstr(M.model.warnings, "cannot read"));
        CHECK_EQ(M.f.texture_count, 1);  // the lost texture is removed and the material slot cleared
        CHECK_EQ(M.f.materials[0].tex[MOF_SLOT_NORMAL].texture, MOF_NONE);
        CHECK_EQ(M.f.materials[0].tex[MOF_SLOT_BASE_COLOR].texture, 0);
        release(&M);
    }
    write_file(p2, g_jpg, g_jpg_n);
}

static void test_gltf_errors_and_fuzz() {
    char path[256], err[256];
    mof_model m;
    CHECK(!mof_import_file("build/mof-test-tmp/does_not_exist.gltf", &m, err, sizeof err) && err[0]);
    CHECK(!mof_import_file("build/mof-test-tmp/does_not_exist.obj", &m, err, sizeof err) && err[0]);
    CHECK(!mof_import_file("model.fbx", &m, err, sizeof err) && strstr(err, "unsupported"));
    snprintf(path, sizeof path, "%s/garbage.gltf", kTmp);
    write_text(path, "{ this is not json");
    CHECK(!mof_import_gltf(path, &m, err, sizeof err));
    write_text(path, "{\"asset\":{\"version\":\"2.0\"},\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":3}}]}]}");
    CHECK(!mof_import_gltf(path, &m, err, sizeof err));
    snprintf(path, sizeof path, "%s/empty.obj", kTmp);
    write_text(path, "# nothing\n");
    CHECK(!mof_import_obj(path, &m, err, sizeof err));

    // truncated and mutated .glb files: never crash; whatever imports must also write and re-open
    snprintf(path, sizeof path, "%s/skinned.glb", kTmp);
    FILE* f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    size_t n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* orig = (uint8_t*)malloc(n);
    size_t got = fread(orig, 1, n, f);
    fclose(f);
    CHECK(got == n);
    snprintf(path, sizeof path, "%s/mutated.glb", kTmp);
    int imported = 0, rejected = 0, write_rejected = 0;
    Rng rng(0xC0FFEE);
    const int iters = getenv("MOF_FUZZ_ITERS") ? atoi(getenv("MOF_FUZZ_ITERS")) / 40 : 500;
    for (int it = 0; it < iters; it++) {
        uint8_t* c = (uint8_t*)malloc(n);
        memcpy(c, orig, n);
        size_t len = n;
        if (it % 5 == 0) len = rng.below((uint32_t)n);  // truncation
        else for (uint32_t k = 0, e = 1 + rng.below(4); k < e; k++) {
            size_t pos = rng.below((uint32_t)n);
            if (rng.below(2)) c[pos] ^= (uint8_t)(1u << rng.below(8)); else c[pos] = (uint8_t)rng.next();
        }
        write_file(path, c, len);
        free(c);
        mof_model mm;
        if (mof_import_gltf(path, &mm, err, sizeof err)) {
            mof_buffer buf;
            if (mof_write(&mm.desc, &buf, err, sizeof err)) {
                mof_file mf;
                CHECK(mof_open(buf.data, buf.size, &mf, err, sizeof err));
                imported++;
                mof_buffer_free(&buf);
            } else { write_rejected++; if (getenv("MOF_TEST_VERBOSE")) printf("    writer: %s\n", err); }
            mof_model_free(&mm);
        } else rejected++;
    }
    printf("  glb fuzz: %d imported and re-read, %d rejected by the importer, %d rejected by the writer\n", imported, rejected, write_rejected);
    free(orig);
}

// ------------------------------------------------------------------ OBJ

static void test_obj() {
    char p[256];
    uint8_t px[2 * 2 * 3] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 0};
    Bytes bmp;
    stbi_write_bmp_to_func(png_cb, &bmp, 2, 2, 3, px);
    snprintf(p, sizeof p, "%s/tex.bmp", kTmp);
    write_file(p, bmp.d, bmp.n);
    snprintf(p, sizeof p, "%s/normal.png", kTmp);
    write_file(p, g_png, g_png_n);
    snprintf(p, sizeof p, "%s/t.mtl", kTmp);
    write_text(p,
               "newmtl red\nKd 1 0 0\nNs 100\nd 1\nmap_Kd tex.bmp\nmap_Bump normal.png\n"
               "newmtl blue\nKd 0 0 1\nKe 0.1 0 0\nd 0.5\n");
    snprintf(p, sizeof p, "%s/thing.obj", kTmp);
    write_text(p,
               "mtllib t.mtl\no thing\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\n"
               "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\n"
               "usemtl red\nf 1/1/1 2/2/1 3/3/1 4/4/1\n"
               "usemtl blue\nf 1/1 2/2 5/3\n");
    Loaded L;
    CHECK_MSG(import_and_write(p, &L), L.err);
    if (!L.ok) return;
    const mof_file& f = L.f;
    CHECK_EQ(f.mesh_count, 1); CHECK_EQ(f.material_count, 2); CHECK_EQ(f.texture_count, 2);
    CHECK_EQ(f.node_count, 1); CHECK_EQ(f.bone_count, 0);
    const mof_mesh& m = f.meshes[0];
    CHECK_STR(mof_string(&f, m.name), "thing");
    CHECK_EQ(m.vertex_count, 7); CHECK_EQ(m.index_count, 9); CHECK_EQ(m.submesh_count, 2);
    CHECK_EQ(m.stream_mask, MOF_STREAM_NORMAL | MOF_STREAM_TANGENT | MOF_STREAM_UV0);
    const mof_submesh* s = f.submeshes + m.first_submesh;
    CHECK_EQ(s[0].first_index, 0); CHECK_EQ(s[0].index_count, 6); CHECK_EQ(s[0].material, 0);
    CHECK_EQ(s[1].first_index, 6); CHECK_EQ(s[1].index_count, 3); CHECK_EQ(s[1].material, 1);
    // quad fan-triangulated: (0,1,2) (0,2,3); triangle uses its own three vertices
    static const uint32_t eI[9] = {0, 1, 2, 0, 2, 3, 4, 5, 6};
    for (uint32_t i = 0; i < 9; i++) CHECK_EQ(mof_mesh_index(&f, &m, i), eI[i]);
    const float* P = mof_mesh_positions(&f, &m);
    CHECK_NEAR(P[3 * 6 + 2], 1);  // vertex 6 = position 5 (0,0,1)
    const float* UV = mof_mesh_uv0(&f, &m);
    CHECK_NEAR(UV[2 * 2], 1); CHECK_NEAR(UV[2 * 2 + 1], 0);   // vt (1,1) -> v flipped to 0
    CHECK_NEAR(UV[1 * 2 + 1], 1);                              // vt (1,0) -> v = 1
    const float* N = mof_mesh_normals(&f, &m);
    CHECK_NEAR(N[2], 1);                                       // from the file
    CHECK_NEAR(N[4 * 3 + 1], -1);                              // generated for the face without vn
    const float* T = mof_mesh_tangents(&f, &m);
    CHECK_NEAR(T[0], 1); CHECK_NEAR(T[3], 1);                  // tangent +X, handedness +1
    // materials
    CHECK_STR(mof_string(&f, f.materials[0].name), "red"); CHECK_STR(mof_string(&f, f.materials[1].name), "blue");
    CHECK_NEAR(f.materials[0].base_color[0], 1); CHECK_NEAR(f.materials[0].base_color[3], 1);
    CHECK(fabsf(f.materials[0].roughness - (1.0f - sqrtf(0.1f))) < 1e-4f);
    CHECK_NEAR(f.materials[0].metallic, 0);
    CHECK_EQ(f.materials[0].tex[MOF_SLOT_BASE_COLOR].texture, 0); CHECK_EQ(f.materials[0].tex[MOF_SLOT_NORMAL].texture, 1);
    CHECK_EQ(f.materials[1].tex[MOF_SLOT_BASE_COLOR].texture, MOF_NONE);
    CHECK_NEAR(f.materials[1].base_color[2], 1); CHECK_NEAR(f.materials[1].base_color[3], 0.5f);
    CHECK_NEAR(f.materials[1].emissive[0], 0.1f);
    CHECK_EQ((f.materials[1].flags & MOF_MATERIAL_ALPHA_MASK) >> MOF_MATERIAL_ALPHA_SHIFT, MOF_ALPHA_BLEND);
    // textures: BMP decoded to raw RGBA8, PNG kept as-is
    const mof_texture& t0 = f.textures[0];
    CHECK_EQ(t0.codec, MOF_TEX_RAW_RGBA8); CHECK_EQ(t0.width, 2); CHECK_EQ(t0.height, 2); CHECK_EQ(t0.data_size, 16);
    uint64_t n;
    const uint8_t* b = mof_texture_bytes(&f, 0, &n);
    CHECK(b[0] == 255 && b[1] == 0 && b[2] == 0 && b[3] == 255);  // top-left pixel is red
    CHECK_EQ(f.textures[1].codec, MOF_TEX_PNG); CHECK_EQ(f.textures[1].width, 4);
    b = mof_texture_bytes(&f, 1, &n);
    CHECK(n == g_png_n && memcmp(b, g_png, n) == 0);
    // the single node references the mesh
    CHECK_EQ(f.nodes[0].parent, -1); CHECK_EQ(f.nodes[0].mesh, 0);
    release(&L);
    free(bmp.d);

    // OBJ without normals / uvs / mtl: normals are generated
    snprintf(p, sizeof p, "%s/bare.obj", kTmp);
    write_text(p, "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nf 1 2 3\nf 1 3 4\n");
    Loaded B;
    CHECK_MSG(import_and_write(p, &B), B.err);
    if (B.ok) {
        const mof_mesh& bm = B.f.meshes[0];
        CHECK_EQ(bm.vertex_count, 4); CHECK_EQ(bm.index_count, 6); CHECK_EQ(bm.stream_mask, MOF_STREAM_NORMAL);
        CHECK_EQ(B.f.material_count, 0); CHECK_EQ(B.f.submeshes[bm.first_submesh].material, MOF_NONE);
        const float* BN = mof_mesh_normals(&B.f, &bm);
        // vertex 0 is shared by a +Z and a +X triangle of equal area: normalized sum
        CHECK_NEAR(BN[0], 0.70710678f); CHECK_NEAR(BN[1], 0); CHECK_NEAR(BN[2], 0.70710678f);
        CHECK_NEAR(BN[3 + 2], 1);  // vertex 1 only belongs to the +Z triangle
        release(&B);
    }
    // hostile OBJ: index out of range is an error, not a crash
    snprintf(p, sizeof p, "%s/bad.obj", kTmp);
    write_text(p, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 99\n");
    mof_model mm;
    char err[256];
    mof_import_obj(p, &mm, err, sizeof err);  // either outcome is fine; it must not crash
}

static void test_generators() {
    // area weighted normals: the big triangle dominates the shared vertex
    const float P[5 * 3] = {0, 0, 0, 10, 0, 0, 0, 10, 0, 0, 0, 1, 1, 0, 0};
    const uint32_t I[6] = {0, 1, 2, 0, 3, 4};  // big triangle in XY (+Z, area 50), small one in XZ (+Y, area 0.5)
    float N[5 * 3];
    mof_generate_normals(P, 5, I, 6, N);
    CHECK(N[2] > 0.99f && N[1] > 0 && N[1] < 0.02f);  // vertex 0: mostly +Z, slightly tilted by the small triangle
    CHECK_NEAR(N[3 + 0] * N[3 + 0] + N[3 + 1] * N[3 + 1] + N[3 + 2] * N[3 + 2], 1);
    float zero[3];
    const float PZ[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    const uint32_t IZ[3] = {0, 1, 2};
    float NZ[9];
    mof_generate_normals(PZ, 3, IZ, 3, NZ);
    CHECK_NEAR(NZ[1], 1);  // degenerate triangle: fallback normal
    (void)zero;
}

// Triangle reduction (mof/simplify.h): a 64 x 64 grid bent into a hill, and 300 separate small cards.
static void test_simplify() {
    const int G = 64;
    static float pos[(G + 1) * (G + 1) * 3], uv[(G + 1) * (G + 1) * 2];
    static uint32_t idx[G * G * 6 + 300 * 6];
    for (int z = 0; z <= G; z++)
        for (int x = 0; x <= G; x++) {
            int v = z * (G + 1) + x;
            float fx = (float)x / G - 0.5f, fz = (float)z / G - 0.5f;
            pos[v * 3] = fx; pos[v * 3 + 1] = 0.3f * (1.0f - 4.0f * (fx * fx + fz * fz)); pos[v * 3 + 2] = fz;
            uv[v * 2] = (float)x / G; uv[v * 2 + 1] = (float)z / G;
        }
    uint32_t n = 0;
    for (int z = 0; z < G; z++)
        for (int x = 0; x < G; x++) {
            uint32_t a = z * (G + 1) + x, b = a + 1, c = a + G + 1, d = c + 1;
            idx[n++] = a; idx[n++] = c; idx[n++] = b; idx[n++] = b; idx[n++] = c; idx[n++] = d;
        }
    mof_wmesh m;
    mof_wmesh_default(&m);
    m.vertex_count = (G + 1) * (G + 1);
    m.positions = pos;
    m.uv0 = uv;
    m.indices = idx;
    m.index_count = n;
    mof_wdesc d;
    memset(&d, 0, sizeof d);
    d.alloc = mof_libc_alloc();
    d.mesh_count = 1;
    d.meshes = &m;
    CHECK(mof_desc_triangles(&d) == (uint64_t)G * G * 2);
    mof_simplified s;
    char err[128];
    CHECK(mof_simplify(&d, 0.1f, &s, err, sizeof err));
    uint64_t want = (uint64_t)G * G * 2 / 10;
    CHECK(s.triangles_after <= want + want / 3 && s.triangles_after >= want / 3);
    const mof_wmesh& r = s.desc.meshes[0];
    CHECK(r.vertex_count < m.vertex_count / 3);          // unused vertices dropped
    float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
    bool idx_ok = true;
    for (uint32_t i = 0; i < r.index_count; i++) idx_ok &= r.indices[i] < r.vertex_count;
    for (uint32_t v = 0; v < r.vertex_count; v++)
        for (int k = 0; k < 3; k++) { lo[k] = fminf(lo[k], r.positions[v * 3 + k]); hi[k] = fmaxf(hi[k], r.positions[v * 3 + k]); }
    CHECK(idx_ok && r.uv0 != nullptr);
    CHECK(lo[0] < -0.49f && hi[0] > 0.49f && lo[2] < -0.49f && hi[2] > 0.49f);   // the outline is kept
    mof_buffer buf;
    CHECK(mof_write(&s.desc, &buf, err, sizeof err));           // a valid file
    mof_buffer_free(&buf);
    mof_simplified_free(&s);
    // 300 separate cards (leaves): the sloppy pass merges them to reach the target.
    static float cp[300 * 4 * 3];
    static uint32_t ci[300 * 6];
    for (int c = 0; c < 300; c++) {
        float ox = (float)(c % 20) * 0.1f, oz = (float)(c / 20) * 0.1f;
        const float q[4][3] = { { ox, 0, oz }, { ox + 0.05f, 0, oz }, { ox + 0.05f, 0.05f, oz }, { ox, 0.05f, oz } };
        memcpy(cp + c * 12, q, sizeof q);
        const uint32_t t[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; k++) ci[c * 6 + k] = (uint32_t)(c * 4) + t[k];
    }
    mof_wmesh mc;
    mof_wmesh_default(&mc);
    mc.vertex_count = 1200;
    mc.positions = cp;
    mc.indices = ci;
    mc.index_count = 1800;
    d.meshes = &mc;
    CHECK(mof_simplify(&d, 0.2f, &s, err, sizeof err));
    CHECK(s.triangles_after <= 160 && s.triangles_after > 0);
    mof_simplified_free(&s);
}

int main() {
    printf("mof_import_tests\n");
    test_simplify();
    mkdir("build", 0777);
    mkdir(kTmp, 0777);
    make_images();
    test_generators();
    test_gltf();
    test_obj();
    test_gltf_errors_and_fuzz();
    return finish_tests("mof_import_tests");
}
