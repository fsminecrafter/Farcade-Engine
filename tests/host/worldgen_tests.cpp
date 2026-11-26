// Tests of fe-worldgen (tools/worldgen, docs/example-world.md) without the network: a hill made up as
// Terrarium elevation tiles, a layer table, then fe-worldgen writes a project. The test checks the project,
// the scene (it loads, nothing skipped), the terrain (heights from the tiles, foliage painted, the lake) and
// that the headless player runs it.
//
// Run from the repository root (make test does): it starts build/host/bin/fe-worldgen and feplayer.
#include "platform/platform.h"
#include "scene.h"
#include "scene_io.h"
#include "scene/terrain.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace fe;

static int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                                                         \
    do {                                                                                                    \
        g_checks++;                                                                                         \
        if (!(cond)) { g_failures++; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static const double kLat = 63.405, kLon = 13.080;      // the example world's centre
static const int kZoom = 10;

static void to_pixel(double lat, double lon, int z, double* x, double* y) {
    double n = 256.0 * (double)(1 << z);
    *x = (lon + 180.0) / 360.0 * n;
    double r = lat * M_PI / 180.0;
    *y = (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * n;
}

// The made-up land, in meters, at global pixel (gx, gy): a valley floor at 380 m, a 500 m hill 1 km
// north-east of the centre and a hollow south-west of it (the lake).
static double land(double gx, double gy, double cx, double cy, double mpp) {
    double dx = (gx - cx) * mpp, dy = (gy - cy) * mpp;
    double hill = 500.0 * exp(-((dx - 700) * (dx - 700) + (dy + 700) * (dy + 700)) / (2 * 600.0 * 600.0));
    double hollow = -25.0 * exp(-((dx + 500) * (dx + 500) + (dy - 500) * (dy - 500)) / (2 * 250.0 * 250.0));
    return 380.0 + hill + hollow;
}

static char* read_text(const char* path, size_t* n) {
    void* p = platform::read_file(path, n);
    if (!p) return nullptr;
    char* t = (char*)realloc(p, *n + 1);
    t[*n] = 0;
    return t;
}

static char* capture(const char* cmd) {
    FILE* f = popen(cmd, "r");
    if (!f) return nullptr;
    size_t cap = 1 << 16, n = 0, r;
    char* out = (char*)malloc(cap);
    while ((r = fread(out + n, 1, cap - n - 1, f)) > 0) {
        n += r;
        if (n + 1 >= cap) { cap *= 2; out = (char*)realloc(out, cap); }
    }
    out[n] = 0;
    pclose(f);
    return out;
}

int main() {
    char tmp[] = "/tmp/fe_worldgen_XXXXXX";
    if (!mkdtemp(tmp)) { fprintf(stderr, "no temp dir\n"); return 1; }
    char tiles[300], out[300], path[600];
    snprintf(tiles, sizeof tiles, "%s/tiles", tmp);
    snprintf(out, sizeof out, "%s/world", tmp);
    mkdir(tiles, 0755);

    // ---- the tiles around the centre (a 1 km map needs one or two; write 3 x 3)
    double cx, cy;
    to_pixel(kLat, kLon, kZoom, &cx, &cy);
    double mpp = 40075016.686 * cos(kLat * M_PI / 180.0) / (256.0 * (double)(1 << kZoom));
    int tx0 = (int)(cx / 256) - 1, ty0 = (int)(cy / 256) - 1;
    static uint8_t px[256 * 256 * 3];
    for (int ty = ty0; ty < ty0 + 3; ty++)
        for (int tx = tx0; tx < tx0 + 3; tx++) {
            for (int y = 0; y < 256; y++)
                for (int x = 0; x < 256; x++) {
                    double h = land(tx * 256.0 + x + 0.5, ty * 256.0 + y + 0.5, cx, cy, mpp) + 32768.0;
                    uint8_t* p = &px[(y * 256 + x) * 3];
                    p[0] = (uint8_t)(h / 256.0);
                    p[1] = (uint8_t)fmod(h, 256.0);
                    p[2] = (uint8_t)((h - floor(h)) * 256.0);
                }
            snprintf(path, sizeof path, "%s/%d_%d_%d.png", tiles, kZoom, tx, ty);
            CHECK(stbi_write_png(path, 256, 256, 3, px, 256 * 3) != 0);
        }

    // ---- the layer table: grass with a sway shader and spruce (the models need not exist for the scene)
    snprintf(path, sizeof path, "%s/layers.cfg", tmp);
    const char* layers =
        "# test layers\n"
        "layer 1 grass grass 1.2 1.6 2.6 70 0 35 0 1 shaders/sway.fshader models/grass/grass_0.mof models/grass/grass_1.mof\n"
        "layer 4 spruce spruce 0.014 0.8 1.4 600 100 35 1 0 - models/spruce/spruce_0.mof\n"
        "layer 8 rocks rocks 0.02 0.5 2.0 500 60 90 1 1 - models/rock/rock_0.mof\n";
    CHECK(platform::write_file(path, layers, strlen(layers)));

    // ---- fe-worldgen
    char cmd[2048];
    snprintf(cmd, sizeof cmd,
             "build/host/bin/fe-worldgen --tiles %s --out %s --layers %s/layers.cfg --center %.4f %.4f --size 2048 --cell 8 --zoom %d "
             "--name 'Test Valley' 2>&1",
             tiles, out, tmp, kLat, kLon, kZoom);
    char* log = capture(cmd);
    CHECK(log != nullptr);
    if (log) {
        printf("%s", log);
        CHECK(strstr(log, "fe-worldgen") != nullptr);
        CHECK(strstr(log, "bad layer") == nullptr);
        CHECK(strstr(log, "missing") == nullptr);
    }
    free(log);

    // ---- the project files
    const char* files[] = { "project.fe", "graphics.cfg", "scenes/main.fscene", "terrain/are.fterrain", "textures/colormap.png",
                            "scripts/flythrough.lua" };
    for (const char* f : files) {
        snprintf(path, sizeof path, "%s/%s", out, f);
        bool ok = platform::file_exists(path);
        if (!ok) fprintf(stderr, "missing %s\n", path);
        CHECK(ok);
    }
    size_t n = 0;
    snprintf(path, sizeof path, "%s/project.fe", out);
    char* proj = read_text(path, &n);
    CHECK(proj && strstr(proj, "Test Valley") && strstr(proj, "scenes/main.fscene"));
    free(proj);

    // ---- the scene: loads, nothing skipped; the terrain's layers, the sky, the clouds, the camera
    snprintf(path, sizeof path, "%s/scenes/main.fscene", out);
    char* text = read_text(path, &n);
    CHECK(text != nullptr);
    static Scene scene;
    SceneLoadReport rep;
    CHECK(text && load_scene_text(scene, text, n, &rep));
    CHECK(rep.skipped == 0);
    if (rep.skipped) fprintf(stderr, "skipped lines, the first at %d\n", rep.first_bad_line);
    free(text);
    Object* terrain = nullptr;
    int clouds = 0, cameras = 0, skies = 0;
    for (int i = 0; i < scene.count(); i++) {
        Object& o = scene.at(i);
        if (o.kind == ObjectKind::Terrain) terrain = &o;
        clouds += o.kind == ObjectKind::Clouds;
        skies += o.kind == ObjectKind::Sky;
        cameras += o.modules.has(ModuleKind::Camera);
    }
    CHECK(terrain != nullptr);
    CHECK(clouds == 1 && skies == 1 && cameras == 1);
    if (terrain) {
        const Terrain& t = terrain->modules.terrain;
        CHECK(!strcmp(t.data, "terrain/are.fterrain"));
        CHECK(!strcmp(t.foliage[0].name, "grass"));
        CHECK(strstr(t.foliage[0].model, "grass_0.mof;") != nullptr && strstr(t.foliage[0].model, "grass_1.mof") != nullptr);
        CHECK(!strcmp(t.foliage[0].shader, "shaders/sway.fshader"));
        CHECK(fabsf(t.foliage[0].scale_max - 2.6f) < 1e-4f);
        CHECK(!strcmp(t.foliage[3].name, "spruce") && t.foliage[3].cast_shadows && fabsf(t.foliage[3].lod_distance - 100) < 1e-3f);
        CHECK(!strcmp(t.foliage[7].name, "rocks"));
        CHECK(t.foliage[1].model[0] == 0);              // layers not in the table stay empty
        CHECK(t.detail_texture[0] == 0 || !strcmp(t.detail_texture, "textures/detail.png"));
        CHECK(fabsf(terrain->position.x + 1024.0f) < 1.0f && fabsf(terrain->position.z + 1024.0f) < 1.0f);
    }

    // ---- the terrain: 2 km of 8 m cells (chunks of 32 cells), heights from the tiles, foliage, the lake
    snprintf(path, sizeof path, "%s/terrain/are.fterrain", out);
    void* data = platform::read_file(path, &n);
    CHECK(data != nullptr);
    static TerrainData td;
    CHECK(data && td.load(data, n));
    free(data);
    CHECK(td.side_x() == 256 && td.cells == 32 && fabsf(td.cell() - 8.0f) < 1e-3f);
    float lo = 1e9f, hi = -1e9f;
    for (int z = 0; z <= td.side_x(); z += 4)
        for (int x = 0; x <= td.side_x(); x += 4) {
            float h = td.height_at((float)x * td.cell(), (float)z * td.cell());
            lo = fminf(lo, h);
            hi = fmaxf(hi, h);
        }
    printf("terrain heights %.1f .. %.1f\n", lo, hi);
    CHECK(lo > 330.0f && lo < 385.0f);                  // the valley floor (and the lake bed under it)
    CHECK(hi > 700.0f && hi < 900.0f);                  // the hill (880 m at most)
    // The hill is north-east of the centre: +x, -z in the terrain (north is -z).
    float h_ne = td.height_at(1024.0f + 700.0f, 1024.0f - 700.0f), h_sw = td.height_at(1024.0f - 700.0f, 1024.0f + 700.0f);
    CHECK(h_ne > h_sw + 300.0f);
    int painted[kFoliageLayers] = {};
    for (int c = 0; c < td.chunk_count(); c++)
        for (int l = 0; l < kFoliageLayers; l++) painted[l] += td.has_foliage(c % td.chunks_x, c / td.chunks_x, l);
    CHECK(painted[0] > 0 && painted[3] > 0 && painted[7] > 0);
    CHECK(painted[1] == 0 && painted[2] == 0);

    // ---- the headless player runs it (the models are missing: their layers draw nothing)
    snprintf(cmd, sizeof cmd, "build/host/bin/feplayer %s --frames 2 2>&1", out);
    char* run = capture(cmd);
    CHECK(run && strstr(run, "FE PLAYER DONE") != nullptr);
    if (run && !strstr(run, "FE PLAYER DONE")) printf("%s", run);
    free(run);

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp);
    if (system(cmd) != 0) {}
    printf("worldgen_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
