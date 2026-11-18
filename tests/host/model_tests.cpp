// Models in scenes: .mof files loaded through the VFS and the model cache,
// bounds and picking on the real mesh, drawn by FarcadeGL, the importer.
//
// Run from the repository root (make test does).
#include "core/pak.h"
#include "core/vfs.h"
#include "gfx/renderer.h"
#include "model_cache.h"
#include "model_import.h"
#include "platform/platform.h"
#include "render_bridge.h"
#include "scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

using namespace fe;

static int g_checks = 0, g_failures = 0;
#define CHECK(cond, ...) \
    do { g_checks++; if (!(cond)) { g_failures++; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static uint32_t checksum(const uint32_t* px, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ px[i]) * 16777619u;
    return h;
}

static gfx::Renderer g_fgl;

// Renders `scene` from a fixed camera and returns the picture's checksum (0 on failure).
static uint32_t render(Scene& scene) {
    const int W = 96, H = 72;
    if (!g_fgl.ready() && !g_fgl.init(W, H)) return 0;
    Vec3 eye(1.6f, 1.4f, 3.2f);
    Mat4 view = Mat4::look_at(eye, Vec3(0, 0.8f, 0), Vec3(0, 1, 0));
    Mat4 proj = Mat4::perspective(radians(50.0f), (float)W / H, 0.1f, 100.0f);
    RenderBridge bridge;
    bridge.build(scene, view, proj, eye, 0.1f, 100.0f);
    static ModelAssetReader assets{ vfs::read_cb, nullptr };
    bridge.scene.read_asset = model_asset_reader;
    bridge.scene.asset_user = &assets;
    if (!g_fgl.render(bridge.scene)) { fprintf(stderr, "render: %s\n", g_fgl.error()); return 0; }
    return checksum(g_fgl.pixels(), W * H);
}

static ObjectId add_model(Scene& s, const char* path, Vec3 at) {
    ObjectId id = s.create("M", MeshKind::Model);
    snprintf(s.get(id)->model_path, 256, "%s", path);
    s.get(id)->position = at;
    return id;
}

static void test_load_and_pick() {
    vfs::Disk d;
    d.read_file = platform::read_file;
    d.file_exists = platform::file_exists;
    vfs::set_disk(d);
    vfs::set_root(".");
    model_cache_clear();
    Scene s;
    ObjectId bar = add_model(s, "assets/samples/skinned_bar.mof", Vec3(0, 0, 0));
    const Model* m = Scene::model_for(*s.get(bar));
    CHECK(m != nullptr);
    if (m) {
        CHECK(m->part_count == 1 && m->mesh.vertex_count == 108);
        CHECK(m->mof.bone_count == 2 && m->mof.node_count == 4);            // the skeleton is reachable for an Animator
        CHECK(mof_mesh_joints(&m->mof, &m->mof.meshes[m->parts[0].mesh_index]) != nullptr);
        const MeshData& md = Scene::mesh_for(*s.get(bar));
        CHECK(md.vertex_count == 108 && md.bounds_min.y > -0.01f && md.bounds_max.y > 1.99f && md.bounds_max.y < 2.01f);   // the bar, not the unit cube
    }
    // Picking uses the real mesh: the bar is 2 high, so a ray at y = 1.8 hits, one at y = 2.5 does not.
    Ray hit{ Vec3(0, 1.8f, 5), Vec3(0, 0, -1) }, miss{ Vec3(0, 2.5f, 5), Vec3(0, 0, -1) };
    CHECK(s.pick(hit) == bar && s.pick(miss) == kNoObject);
    // The export demo's textured model.
    ObjectId cube = add_model(s, "tests/data/export_demo/cube.mof", Vec3(3, 0, 0));
    const Model* cm = Scene::model_for(*s.get(cube));
    CHECK(cm && cm->part_count >= 1 && cm->parts[0].uvs != nullptr);
    if (cm) {
        bool textured = false;
        for (int i = 0; i < cm->part_count; i++) {
            if (!cm->parts[i].texture[0]) continue;
            textured = true;
            // The "mof:" path gives the renderer decodable bytes.
            size_t n = 0;
            ModelAssetReader r{ vfs::read_cb, nullptr };
            void* bytes = model_asset_reader(cm->parts[i].texture, &n, &r);
            CHECK(bytes && n > 8);
            free(bytes);
        }
        CHECK(textured);
    }
    CHECK(model_load("assets/samples/cube_textured.mof") != nullptr && model_load("assets/samples/skinned_bar.mof") != nullptr);
    // A missing or broken file falls back to the cube and is not an error.
    ObjectId gone = add_model(s, "nope/missing.mof", Vec3(0, 0, 0));
    CHECK(Scene::model_for(*s.get(gone)) == nullptr && Scene::mesh_for(*s.get(gone)).vertex_count == 36);
    platform::write_file("build/host/broken.mof", "MOF\0garbage", 11);
    ObjectId bad = add_model(s, "build/host/broken.mof", Vec3(0, 0, 0));
    CHECK(Scene::model_for(*s.get(bad)) == nullptr);
}

static void test_render() {
    Scene empty;
    uint32_t c0 = render(empty);
    CHECK(c0 != 0);
    Scene s;
    add_model(s, "assets/samples/skinned_bar.mof", Vec3(0, 0, 0));
    uint32_t c1 = render(s), c2 = render(s);
    CHECK(c1 != 0 && c1 == c2 && c1 != c0);                 // drawn, and repeatable
    Scene t;
    add_model(t, "tests/data/export_demo/cube.mof", Vec3(0, 0.5f, 0));
    uint32_t c3 = render(t);
    Scene u;
    add_model(u, "nope.mof", Vec3(0, 0.5f, 0));             // the plain cube instead
    uint32_t c4 = render(u);
    CHECK(c3 != 0 && c4 != 0 && c3 != c4);                  // the model's own material and texture show
}

static void test_from_pak() {
    // The model inside a package, with no disk fallback: what an exported game sees.
    size_t n = 0;
    void* bytes = platform::read_file("assets/samples/skinned_bar.mof", &n);
    PakWriter w;
    w.add("models/bar.mof", bytes, n);
    free(bytes);
    size_t size = 0;
    uint8_t* image = w.build(&size);
    vfs::unmount_all();
    CHECK(vfs::mount_pak_memory(image, size));
    vfs::set_disk_fallback(false);
    model_cache_clear();
    Scene s;
    ObjectId id = add_model(s, "models/bar.mof", Vec3(0, 0, 0));
    CHECK(Scene::model_for(*s.get(id)) != nullptr);
    uint32_t c = render(s);
    vfs::set_disk_fallback(true);
    Scene d;
    add_model(d, "assets/samples/skinned_bar.mof", Vec3(0, 0, 0));
    CHECK(c != 0 && c == render(d));                        // the same picture from the package and from disk
    vfs::unmount_all();
    free(image);
    model_cache_clear();
}

static void test_import() {
    const char* obj = "o quad\nv 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nvn 0 0 1\nf 1//1 2//1 3//1\nf 2//1 4//1 3//1\n";
    mkdir("build/host/import-test", 0755);
    platform::write_file("build/host/import-test/quad.obj", obj, strlen(obj));
    char out[300], msg[1024];
    CHECK(path_is_importable_model("a/B.GLB") && path_is_importable_model("x.obj") && !path_is_importable_model("x.mof"));
    CHECK(model_convert_file("build/host/import-test/quad.obj", out, sizeof out, msg, sizeof msg));
    CHECK(strcmp(out, "build/host/import-test/quad.mof") == 0);
    model_cache_clear();
    const Model* m = model_load(out);
    CHECK(m && m->mesh.vertex_count == 6);
    CHECK(!model_convert_file("build/host/import-test/missing.obj", out, sizeof out, msg, sizeof msg) && msg[0]);
}

int main() {
    test_load_and_pick();
    test_render();
    test_from_pak();
    test_import();
    printf("model_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
