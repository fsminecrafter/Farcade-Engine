// fe-worldgen: builds the example world project from real elevation data (docs/example-world.md).
//
//   fe-worldgen --tiles DIR --out PROJECT --layers LAYERS.cfg [--center LAT LON] [--size M] [--cell M]
//               [--zoom Z] [--ground GROUND.jpg] [--name NAME]
//
// --tiles   Terrarium elevation tiles DIR/<z>_<x>_<y>.png (the AWS terrain tiles, tools/worldgen/fetch.py)
// --center  the map's centre (default 63.405 13.080: Åre, Sweden, lake Åresjön to the top of Åreskutan)
// --size    meters a side (default 8192), --cell meters per grid cell (default 2; the map has at most
//           4096 cells a side)
// --layers  the foliage layers (written by tools/worldgen/build.py), one per line:
//             layer <1..8> <kind> <name> <density> <scale_min> <scale_max> <distance> <lod_distance> <max_slope>
//                   <shadows 0|1> <align 0|1> <shader or -> <model> [model ...]
//           kind picks where the layer grows: grass flowers shrubs spruce pine broadleaf saplings rocks
// --ground  a ground photo (forest floor): made into the terrain's grey detail texture
//
// Writes PROJECT/project.fe, graphics.cfg, scenes/main.fscene, terrain/are.fterrain, textures/colormap.png,
// textures/detail.png, scripts/flythrough.lua. Models and shaders are copied in by build.py.
#include "scene/terrain.h"
#include "scene/modules.h"

#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

using namespace fe;

namespace {

const double kPiD = 3.14159265358979323846;

// ---------------------------------------------------------------- small helpers

void* read_file(const char* path, size_t* n) {
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

bool write_file(const char* path, const void* data, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
}

void make_dirs(const char* base, const char* sub) {
    char p[1024];
    snprintf(p, sizeof p, "%s", base);
    mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/%s", base, sub);
    mkdir(p, 0777);
}

// A growing text buffer.
struct Text {
    char* p = nullptr;
    size_t n = 0, cap = 0;
    void f(const char* fmt, ...) {
        va_list a, b;
        va_start(a, fmt);
        va_copy(b, a);
        int k = vsnprintf(nullptr, 0, fmt, a);
        va_end(a);
        if (k < 0) { va_end(b); return; }
        if (n + (size_t)k + 1 > cap) {
            cap = (n + (size_t)k + 1) * 2;
            p = (char*)realloc(p, cap);
        }
        vsnprintf(p + n, (size_t)k + 1, fmt, b);
        va_end(b);
        n += (size_t)k;
    }
    ~Text() { free(p); }
};

float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
float smooth(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// Value noise: hashed lattice, smooth interpolation, octaves.
uint32_t hash2(int x, int y, uint32_t seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
float vnoise(float x, float y, uint32_t seed) {
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float tx = x - (float)xi, ty = y - (float)yi;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    auto v = [&](int a, int b) { return (float)(hash2(a, b, seed) & 0xFFFF) / 65535.0f; };
    float a = lerpf(v(xi, yi), v(xi + 1, yi), tx), b = lerpf(v(xi, yi + 1), v(xi + 1, yi + 1), tx);
    return lerpf(a, b, ty);
}
float fbm(float x, float y, int oct, uint32_t seed) {
    float s = 0, amp = 0.5f, norm = 0;
    for (int o = 0; o < oct; o++) {
        s += vnoise(x, y, seed + (uint32_t)o * 101u) * amp;
        norm += amp;
        x *= 2.03f;
        y *= 2.03f;
        amp *= 0.5f;
    }
    return s / norm;
}

// ---------------------------------------------------------------- elevation

// The Terrarium tiles of a zoom level, loaded as needed: elevation in meters at global pixel coordinates.
struct Dem {
    const char* dir = nullptr;
    int zoom = 13;
    struct Tile { int x, y; float* h; };
    Tile tiles[64];
    int count = 0;
    int missing = 0;
    const float* tile(int tx, int ty) {
        for (int i = 0; i < count; i++) if (tiles[i].x == tx && tiles[i].y == ty) return tiles[i].h;
        if (count == 64) return nullptr;
        char path[1024];
        snprintf(path, sizeof path, "%s/%d_%d_%d.png", dir, zoom, tx, ty);
        size_t n = 0;
        void* b = read_file(path, &n);
        int w = 0, h = 0, c = 0;
        uint8_t* px = b ? stbi_load_from_memory((const uint8_t*)b, (int)n, &w, &h, &c, 3) : nullptr;
        free(b);
        float* out = nullptr;
        if (px && w == 256 && h == 256) {
            out = (float*)malloc(sizeof(float) * 256 * 256);
            for (int i = 0; i < 256 * 256; i++) out[i] = (float)px[i * 3] * 256.0f + (float)px[i * 3 + 1] + (float)px[i * 3 + 2] / 256.0f - 32768.0f;
        } else {
            missing++;
            fprintf(stderr, "fe-worldgen: missing or bad tile %s\n", path);
        }
        if (px) stbi_image_free(px);
        tiles[count++] = { tx, ty, out };
        return out;
    }
    float at(int gx, int gy) {
        const float* t = tile(gx >> 8, gy >> 8);
        return t ? t[(gy & 255) * 256 + (gx & 255)] : 0.0f;
    }
    // Bicubic (Catmull-Rom) at fractional global pixel coordinates.
    float sample(double gx, double gy) {
        gx -= 0.5;
        gy -= 0.5;
        int x0 = (int)floor(gx), y0 = (int)floor(gy);
        float tx = (float)(gx - x0), ty = (float)(gy - y0);
        auto cr = [](float p0, float p1, float p2, float p3, float t) {
            return p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
        };
        float col[4];
        for (int j = 0; j < 4; j++)
            col[j] = cr(at(x0 - 1, y0 - 1 + j), at(x0, y0 - 1 + j), at(x0 + 1, y0 - 1 + j), at(x0 + 2, y0 - 1 + j), tx);
        return cr(col[0], col[1], col[2], col[3], ty);
    }
};

// Web Mercator: global pixel coordinates of a latitude / longitude at zoom z.
void to_pixel(double lat, double lon, int z, double* px, double* py) {
    double n = 256.0 * (double)(1 << z);
    *px = (lon + 180.0) / 360.0 * n;
    double r = lat * kPiD / 180.0;
    *py = (1.0 - log(tan(r) + 1.0 / cos(r)) / kPiD) / 2.0 * n;
}

// ---------------------------------------------------------------- foliage layers

enum Kind { K_GRASS, K_FLOWERS, K_SHRUBS, K_SPRUCE, K_PINE, K_BROADLEAF, K_SAPLINGS, K_ROCKS, K_COUNT };
const char* const kKinds[K_COUNT] = { "grass", "flowers", "shrubs", "spruce", "pine", "broadleaf", "saplings", "rocks" };

struct Layer {
    bool used = false;
    int kind = 0;
    char name[64] = {};
    float density = 1, smin = 0.8f, smax = 1.2f, distance = 100, lod = 0, slope = 40;
    bool shadows = false, align = false;
    char shader[256] = {};
    char models[kPathListMax] = {};
};

bool read_layers(const char* path, Layer* layers) {
    size_t n = 0;
    char* text = (char*)read_file(path, &n);
    if (!text) return false;
    text = (char*)realloc(text, n + 1);
    text[n] = 0;
    for (char* line = strtok(text, "\n"); line; line = strtok(nullptr, "\n")) {
        if (strncmp(line, "layer ", 6) != 0) continue;
        int idx = 0, sh = 0, al = 0;
        char kind[32], name[64], shader[256];
        int used = 0;
        Layer L;
        if (sscanf(line, "layer %d %31s %63s %f %f %f %f %f %f %d %d %255s %n", &idx, kind, name, &L.density, &L.smin, &L.smax, &L.distance,
                   &L.lod, &L.slope, &sh, &al, shader, &used) < 12 || idx < 1 || idx > kFoliageLayers) {
            fprintf(stderr, "fe-worldgen: bad layer line: %s\n", line);
            continue;
        }
        L.used = true;
        L.kind = -1;
        for (int k = 0; k < K_COUNT; k++) if (!strcmp(kind, kKinds[k])) L.kind = k;
        if (L.kind < 0) { fprintf(stderr, "fe-worldgen: unknown kind %s\n", kind); continue; }
        snprintf(L.name, sizeof L.name, "%s", name);
        L.shadows = sh != 0;
        L.align = al != 0;
        snprintf(L.shader, sizeof L.shader, "%s", strcmp(shader, "-") ? shader : "");
        // the models: the rest of the line, separated by ';' in the scene
        const char* m = line + used;
        size_t at = 0;
        while (*m) {
            while (*m == ' ') m++;
            const char* e = m;
            while (*e && *e != ' ' && *e != '\r') e++;
            if (e > m && at + (size_t)(e - m) + 2 < sizeof L.models) {
                if (at) L.models[at++] = ';';
                memcpy(L.models + at, m, (size_t)(e - m));
                at += (size_t)(e - m);
                L.models[at] = 0;
            }
            m = *e ? e + 1 : e;
            if (*e == '\r') break;
        }
        layers[idx - 1] = L;
    }
    free(text);
    return true;
}

// ---------------------------------------------------------------- the world

struct World {
    int side = 0;           // cells a side
    float cell = 2;
    float size = 0;
    float* h = nullptr;     // (side + 1)^2
    float water = 0;        // the lake's level
    float hmin = 0, hmax = 0;
    float height(int x, int z) const {
        x = x < 0 ? 0 : (x > side ? side : x);
        z = z < 0 ? 0 : (z > side ? side : z);
        return h[(size_t)z * (side + 1) + x];
    }
    // slope in degrees at a cell's centre
    float slope(int x, int z) const {
        float dx = (height(x + 1, z) + height(x + 1, z + 1) - height(x, z) - height(x, z + 1)) * 0.5f / cell;
        float dz = (height(x, z + 1) + height(x + 1, z + 1) - height(x, z) - height(x + 1, z)) * 0.5f / cell;
        return (float)(atan(sqrt(dx * dx + dz * dz)) * 180.0 / kPiD);
    }
    float cell_h(int x, int z) const { return (height(x, z) + height(x + 1, z) + height(x, z + 1) + height(x + 1, z + 1)) * 0.25f; }
};

// The ecology of a cell: the forest cover, the meadows, the tree line.
struct Eco {
    float forest;           // 0..1, where trees grow
    float meadow;           // 0..1, open grass
    float alpine;           // 0..1, above the tree line
    float shore;            // 0..1, next to the lake
    float rock;             // 0..1, bare rock (steep or high)
};

Eco eco_at(const World& w, int x, int z) {
    float e = w.cell_h(x, z), s = w.slope(x, z);
    float wx = (float)x * w.cell, wz = (float)z * w.cell;
    Eco c;
    // the tree line wavers with the terrain and noise (about 760..880 m on Åreskutan)
    float tl = 820.0f + (fbm(wx / 900.0f, wz / 900.0f, 3, 11) - 0.5f) * 140.0f;
    c.alpine = smooth(tl - 40.0f, tl + 60.0f, e);
    c.shore = 1.0f - smooth(w.water + 2.0f, w.water + 12.0f, e);
    float clusters = fbm(wx / 420.0f, wz / 420.0f, 4, 23);          // stands of forest and clearings
    float open_land = smooth(0.58f, 0.70f, clusters);               // meadows / farmland near the valley floor
    float valley = 1.0f - smooth(w.water + 40.0f, w.water + 140.0f, e);
    c.meadow = clampf(open_land * (0.4f + 0.6f * valley) + c.shore * 0.5f, 0.0f, 1.0f) * (1.0f - c.alpine);
    c.rock = clampf(smooth(32.0f, 45.0f, s) + smooth(1150.0f, 1350.0f, e) * 0.7f, 0.0f, 1.0f);
    c.forest = clampf((1.0f - c.meadow) * (1.0f - c.alpine) * (1.0f - smooth(30.0f, 40.0f, s)) * (1.0f - c.shore), 0.0f, 1.0f);
    if (e < w.water + 0.5f) c.forest = c.meadow = 0.0f;              // under water
    return c;
}

uint8_t density_for(int kind, const World& w, int x, int z, const Eco& c) {
    float e = w.cell_h(x, z), s = w.slope(x, z);
    float wx = (float)x * w.cell, wz = (float)z * w.cell;
    if (e < w.water + 0.3f) return 0;
    float v = 0;
    float mix = fbm(wx / 300.0f, wz / 300.0f, 3, 41);                // spruce vs pine stands
    float fine = fbm(wx / 25.0f, wz / 25.0f, 3, 57);                 // patches
    switch (kind) {
        case K_SPRUCE: v = c.forest * smooth(0.25f, 0.6f, 1.0f - mix) * (1.0f - smooth(700.0f, 800.0f, e)); break;
        case K_PINE: v = c.forest * smooth(0.35f, 0.75f, mix) * (0.6f + 0.4f * smooth(8.0f, 25.0f, s)) * (1.0f - smooth(600.0f, 720.0f, e)); break;
        case K_BROADLEAF: {
            // mountain birch below the tree line, and birches along the shore and the meadows' edges
            float belt = smooth(620.0f, 700.0f, e) * (1.0f - c.alpine);
            float edge = c.meadow * (1.0f - c.meadow) * 4.0f;
            v = clampf(belt * (1.0f - c.rock) * 0.9f + edge * 0.5f * (1.0f - c.alpine) + c.shore * 0.3f * (1.0f - c.rock), 0.0f, 1.0f);
            break;
        }
        case K_SAPLINGS: {
            float edge = c.forest * (1.0f - c.forest) * 4.0f;
            v = clampf(edge + c.forest * 0.15f + smooth(0.0f, 0.3f, c.alpine) * (1.0f - smooth(0.3f, 0.8f, c.alpine)) * 0.6f, 0.0f, 1.0f) *
                (1.0f - c.rock);
            break;
        }
        case K_SHRUBS:
            v = clampf(c.forest * 0.55f * smooth(0.35f, 0.7f, fine) + c.alpine * (1.0f - c.rock) * 0.5f * smooth(0.3f, 0.6f, fine) +
                       c.shore * 0.4f, 0.0f, 1.0f);
            break;
        case K_GRASS:
            v = clampf(c.meadow * 0.95f + c.forest * 0.12f * fine + c.alpine * (1.0f - c.rock) * 0.45f, 0.0f, 1.0f) * (1.0f - smooth(30.0f, 38.0f, s));
            break;
        case K_FLOWERS: v = c.meadow * smooth(0.55f, 0.8f, fine); break;
        case K_ROCKS:
            v = clampf(c.rock * 0.8f + c.alpine * 0.35f * smooth(0.45f, 0.7f, fine) + c.forest * 0.06f, 0.0f, 1.0f);
            break;
    }
    return (uint8_t)clampf(v * 255.0f + 0.5f, 0.0f, 255.0f);
}

// The colour of a cell for the colour map (linear-ish sRGB 0..1).
void color_at(const World& w, int x, int z, float out[3]) {
    Eco c = eco_at(w, x, z);
    float e = w.cell_h(x, z);
    float wx = (float)x * w.cell, wz = (float)z * w.cell;
    float n = fbm(wx / 60.0f, wz / 60.0f, 4, 71);
    const float forest[3] = { 0.20f, 0.22f, 0.13f }, meadow[3] = { 0.34f, 0.43f, 0.19f }, heath[3] = { 0.38f, 0.35f, 0.24f },
                rock[3] = { 0.43f, 0.42f, 0.40f }, canopy[3] = { 0.07f, 0.11f, 0.06f }, snow[3] = { 0.90f, 0.92f, 0.95f }, bed[3] = { 0.24f, 0.24f, 0.20f };
    for (int k = 0; k < 3; k++) {
        float v = lerpf(forest[k], meadow[k], c.meadow);
        // Forest canopy: dark conifer green, so a forest still reads as one beyond the trees' draw distance.
        v = lerpf(v, canopy[k], c.forest * (1.0f - c.meadow) * 0.85f);
        v = lerpf(v, heath[k], c.alpine);
        v = lerpf(v, rock[k], c.rock * 0.9f);
        float sn = smooth(1250.0f, 1380.0f, e) * smooth(0.55f, 0.7f, n) * (1.0f - smooth(28.0f, 40.0f, w.slope(x, z)));
        v = lerpf(v, snow[k], sn);
        if (e < w.water + 0.5f) v = bed[k];
        out[k] = clampf(v * (0.85f + 0.3f * n), 0.0f, 1.0f);
    }
}

// Quaternion turning (0, 0, -1) to `d` (lights and cameras look along -z).
void look_quat(Vec3 d, float q[4]) {
    d = normalize(d);
    Vec3 a(0, 0, -1);
    Vec3 axis = cross(a, d);
    float s = length(axis), c = dot(a, d);
    if (s < 1e-6f) { q[0] = 0; q[1] = c > 0 ? 0 : 1; q[2] = 0; q[3] = c > 0 ? 1 : 0; return; }
    axis = axis * (1.0f / s);
    float ang = atan2f(s, c) * 0.5f;
    q[0] = axis.x * sinf(ang); q[1] = axis.y * sinf(ang); q[2] = axis.z * sinf(ang); q[3] = cosf(ang);
}

void png_write(void* ctx, void* data, int n) {
    Text* t = (Text*)ctx;
    if (t->n + (size_t)n + 1 > t->cap) {
        t->cap = (t->n + (size_t)n + 1) * 2;
        t->p = (char*)realloc(t->p, t->cap);
    }
    memcpy(t->p + t->n, data, (size_t)n);
    t->n += (size_t)n;
}

}  // namespace

int main(int argc, char** argv) {
    const char *tiles = nullptr, *out = nullptr, *layers_path = nullptr, *ground = nullptr, *name = "Are Valley";
    double lat = 63.405, lon = 13.080;
    float size = 8192.0f, cell = 2.0f;
    int zoom = 10;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tiles") && i + 1 < argc) tiles = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--layers") && i + 1 < argc) layers_path = argv[++i];
        else if (!strcmp(argv[i], "--ground") && i + 1 < argc) ground = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--center") && i + 2 < argc) { lat = atof(argv[++i]); lon = atof(argv[++i]); }
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) size = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--cell") && i + 1 < argc) cell = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--zoom") && i + 1 < argc) zoom = atoi(argv[++i]);
        else { fprintf(stderr, "fe-worldgen: unknown argument %s\n", argv[i]); return 2; }
    }
    if (!tiles || !out || !layers_path) {
        fprintf(stderr, "usage: fe-worldgen --tiles DIR --out PROJECT --layers LAYERS.cfg [--center LAT LON] [--size M] [--cell M] [--zoom Z] [--ground IMG]\n");
        return 2;
    }
    Layer layers[kFoliageLayers];
    if (!read_layers(layers_path, layers)) { fprintf(stderr, "fe-worldgen: cannot read %s\n", layers_path); return 1; }

    // ---- the grid: chunks of about 256 m (32..128 cells), as many as fit (at most 4096 cells). Big chunks keep
    // the meshes of a whole loaded map (one per chunk) well under the renderer's mesh cache.
    int cells_per_chunk = (int)(256.0f / cell + 0.5f);
    if (cells_per_chunk < 32) cells_per_chunk = 32;
    if (cells_per_chunk > 128) cells_per_chunk = 128;
    int side = (int)(size / cell + 0.5f);
    side = side / cells_per_chunk * cells_per_chunk;
    if (side > kTerrainMaxCells) side = kTerrainMaxCells;
    if (side < cells_per_chunk) side = cells_per_chunk;
    size = (float)side * cell;
    World w;
    w.side = side;
    w.cell = cell;
    w.size = size;
    w.h = (float*)malloc(sizeof(float) * (size_t)(side + 1) * (side + 1));
    if (!w.h) { fprintf(stderr, "fe-worldgen: out of memory\n"); return 1; }

    // ---- heights: the tiles, bicubic, plus fractal detail below the DEM's resolution
    Dem dem;
    dem.dir = tiles;
    dem.zoom = zoom;
    double cx, cy;
    to_pixel(lat, lon, zoom, &cx, &cy);
    double mpp = 40075016.686 * cos(lat * kPiD / 180.0) / (256.0 * (double)(1 << zoom));
    w.hmin = 1e30f;
    w.hmax = -1e30f;
    printf("fe-worldgen: %s, %.0f x %.0f m, %d x %d cells of %.1f m, elevation %.2f m per DEM pixel\n", name, (double)size, (double)size, side,
           side, (double)cell, mpp);
    for (int z = 0; z <= side; z++) {
        for (int x = 0; x <= side; x++) {
            double mx = ((double)x - side * 0.5) * cell, mz = ((double)z - side * 0.5) * cell;
            float e = dem.sample(cx + mx / mpp, cy + mz / mpp);
            float wx = (float)x * cell, wz = (float)z * cell;
            e += (fbm(wx / 40.0f, wz / 40.0f, 4, 5) - 0.5f) * 2.4f + (fbm(wx / 9.0f, wz / 9.0f, 2, 7) - 0.5f) * 0.5f;
            // A coarse DEM (zoom 10, ~70 m a pixel): the relief between its samples, more of it (and ridged)
            // up in the alpine part, none of it at a fine DEM.
            float coarse = fclamp(((float)mpp - 12.0f) / 50.0f, 0.0f, 1.0f);
            if (coarse > 0) {
                float alp = fclamp((e - 600.0f) / 500.0f, 0.0f, 1.0f);
                float ridge = 1.0f - fabsf(fbm(wx / 130.0f, wz / 130.0f, 4, 17) * 2.0f - 1.0f);
                e += coarse * ((fbm(wx / 240.0f, wz / 240.0f, 4, 13) - 0.5f) * 16.0f * (0.6f + alp) + (ridge - 0.5f) * 14.0f * alp);
            }
            w.h[(size_t)z * (side + 1) + x] = e;
            if (e < w.hmin) w.hmin = e;
            if (e > w.hmax) w.hmax = e;
        }
        if (z % 512 == 0) { printf("  heights %d%%\r", z * 100 / side); fflush(stdout); }
    }
    if (dem.missing) { fprintf(stderr, "fe-worldgen: %d elevation tiles missing (run fetch.py)\n", dem.missing); return 1; }
    // The lake: the DEM is flat at the water's level. Take a low percentile, then lower the bed under it.
    {
        size_t n = (size_t)(side + 1) * (side + 1);
        int hist[4096] = {};
        float range = w.hmax - w.hmin;
        for (size_t i = 0; i < n; i++) hist[(int)clampf((w.h[i] - w.hmin) / range * 4095.0f, 0, 4095)]++;
        size_t acc = 0, want = n / 50;           // 2%: the lake covers more than that of the map
        int k = 0;
        for (; k < 4096 && acc < want; k++) acc += (size_t)hist[k];
        w.water = w.hmin + range * (float)k / 4095.0f + 1.0f;
        for (size_t i = 0; i < n; i++)
            if (w.h[i] < w.water + 0.6f) w.h[i] = lerpf(w.h[i], w.water - 2.5f, smooth(w.water + 0.6f, w.water - 0.5f, w.h[i]));
    }
    printf("  heights done: %.0f .. %.0f m, lake at %.1f m\n", (double)w.hmin, (double)w.hmax, (double)w.water);

    // ---- the terrain data
    TerrainData td;
    if (!td.generate(side / cells_per_chunk, side / cells_per_chunk, cells_per_chunk, cells_per_chunk * cell, 0.0f)) {
        fprintf(stderr, "fe-worldgen: out of memory for the terrain\n");
        return 1;
    }
    memcpy(td.heights, w.h, sizeof(float) * (size_t)(side + 1) * (side + 1));
    size_t ncells = (size_t)side * side;
    long painted[kFoliageLayers] = {};
    for (int l = 0; l < kFoliageLayers; l++) {
        if (!layers[l].used) continue;
        td.foliage[l] = (uint8_t*)calloc(ncells, 1);
        if (!td.foliage[l]) { fprintf(stderr, "fe-worldgen: out of memory for foliage\n"); return 1; }
    }
    for (int z = 0; z < side; z++) {
        for (int x = 0; x < side; x++) {
            Eco c = eco_at(w, x, z);
            for (int l = 0; l < kFoliageLayers; l++) {
                if (!td.foliage[l]) continue;
                uint8_t d = density_for(layers[l].kind, w, x, z, c);
                td.foliage[l][(size_t)z * side + x] = d;
                painted[l] += d > 0;
            }
        }
        if (z % 512 == 0) { printf("  foliage %d%%\r", z * 100 / side); fflush(stdout); }
    }
    char path[1024];
    make_dirs(out, "terrain");
    make_dirs(out, "textures");
    make_dirs(out, "scenes");
    make_dirs(out, "scripts");
    size_t fsize = 0;
    uint8_t* fdata = td.save(&fsize);
    snprintf(path, sizeof path, "%s/terrain/are.fterrain", out);
    if (!fdata || !write_file(path, fdata, fsize)) { fprintf(stderr, "fe-worldgen: cannot write %s\n", path); return 1; }
    free(fdata);
    printf("  terrain: %s (%.1f MB)\n", path, (double)fsize / 1048576.0);
    for (int l = 0; l < kFoliageLayers; l++)
        if (layers[l].used) printf("  layer %d %-10s %5.1f%% of the map\n", l + 1, layers[l].name, 100.0 * (double)painted[l] / (double)ncells);

    // ---- the colour map (one texel per two cells, at most 2048) and the detail texture
    {
        int cm = side / 2 > 2048 ? 2048 : side / 2;
        uint8_t* px = (uint8_t*)malloc((size_t)cm * cm * 3);
        for (int y = 0; y < cm; y++)
            for (int x = 0; x < cm; x++) {
                float c[3];
                color_at(w, x * side / cm, y * side / cm, c);
                for (int k = 0; k < 3; k++) px[((size_t)y * cm + x) * 3 + k] = (uint8_t)(c[k] * 255.0f + 0.5f);
            }
        snprintf(path, sizeof path, "%s/textures/colormap.png", out);
        if (!stbi_write_png(path, cm, cm, 3, px, cm * 3)) { fprintf(stderr, "fe-worldgen: cannot write %s\n", path); return 1; }
        free(px);
        printf("  colour map: %d x %d\n", cm, cm);
    }
    bool have_detail = false;
    if (ground) {
        size_t n = 0;
        void* b = read_file(ground, &n);
        int gw = 0, gh = 0, gc = 0;
        uint8_t* g = b ? stbi_load_from_memory((const uint8_t*)b, (int)n, &gw, &gh, &gc, 3) : nullptr;
        free(b);
        if (g) {
            // grey, centred on 0.5 (neutral when multiplied by two), 512 x 512
            const int D = 512;
            float* lum = (float*)malloc(sizeof(float) * D * D);
            double mean = 0;
            for (int y = 0; y < D; y++)
                for (int x = 0; x < D; x++) {
                    const uint8_t* p = g + ((size_t)(y * gh / D) * gw + (size_t)(x * gw / D)) * 3;
                    float v = (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.0f;
                    lum[y * D + x] = v;
                    mean += v;
                }
            mean /= (double)D * D;
            uint8_t* o = (uint8_t*)malloc(D * D);
            for (int i = 0; i < D * D; i++) o[i] = (uint8_t)clampf((0.5f + (lum[i] - (float)mean) * 0.9f) * 255.0f, 0, 255);
            snprintf(path, sizeof path, "%s/textures/detail.png", out);
            have_detail = stbi_write_png(path, D, D, 1, o, D) != 0;
            free(o);
            free(lum);
            stbi_image_free(g);
        }
    }

    // ---- the camera's path: from the lake shore up through the forest to Åreskutan and back over the ridge
    const float half = size * 0.5f;
    auto ground_y = [&](float x, float z) {
        int ix = (int)clampf((x + half) / cell, 0, (float)side), iz = (int)clampf((z + half) / cell, 0, (float)side);
        return w.height(ix, iz);
    };
    // the summit: the highest point; the village: the lowest dry point near the centre
    float sx = 0, sz = 0, smax = -1e30f;
    for (int z = 0; z <= side; z += 4)
        for (int x = 0; x <= side; x += 4)
            if (w.height(x, z) > smax) { smax = w.height(x, z); sx = (float)x * cell - half; sz = (float)z * cell - half; }
    float vx = 0, vz = half * 0.15f;
    const int kPath = 10;
    Vec3 pts[kPath];
    const float frac[kPath][3] = {      // along village -> summit (t), sideways (s), height above the ground
        { -0.10f, -0.30f, 25 }, { 0.10f, -0.15f, 45 }, { 0.30f, 0.05f, 60 }, { 0.50f, 0.20f, 80 }, { 0.75f, 0.10f, 120 },
        { 0.95f, -0.05f, 160 }, { 1.05f, -0.30f, 220 }, { 0.80f, -0.55f, 160 }, { 0.45f, -0.60f, 90 }, { 0.10f, -0.50f, 40 } };
    Vec3 along(sx - vx, 0, sz - vz), sideways(-(sz - vz), 0, sx - vx);
    for (int i = 0; i < kPath; i++) {
        float x = vx + along.x * frac[i][0] + sideways.x * frac[i][1] * 0.5f;
        float z = vz + along.z * frac[i][0] + sideways.z * frac[i][1] * 0.5f;
        x = clampf(x, -half + 200, half - 200);
        z = clampf(z, -half + 200, half - 200);
        float y = ground_y(x, z);
        if (y < w.water) y = w.water;
        pts[i] = Vec3(x, y + frac[i][2], z);
    }

    // ---- the scene
    Text sc;
    sc.f("farcade-scene 1\nnext_id 20\n\n");
    // terrain (its corner at -half, -half)
    sc.f("object 1\n  name \"Terrain\"\n  kind terrain\n  position %g 0 %g\n  rotation 0 0 0 1\n  scale 1 1 1\n", -half, -half);
    sc.f("  module terrain\n    data \"terrain/are.fterrain\"\n    load_distance 3000\n    stream_in_editor true\n    lod_distance 96\n");
    sc.f("    texture \"textures/colormap.png\"\n    color 1 1 1\n    roughness 0.95\n    uv_scale %g\n", size);
    if (have_detail) sc.f("    detail_texture \"textures/detail.png\"\n    detail_scale 3\n    detail_strength 0.85\n");
    sc.f("    cast_shadows true\n    friction 0.8\n    layer 1\n    revision %d\n", td.revision);
    for (int l = 0; l < kFoliageLayers; l++) {
        const Layer& L = layers[l];
        if (!L.used) continue;
        int n = l + 1;
        sc.f("    foliage%d_name \"%s\"\n    foliage%d_model \"%s\"\n    foliage%d_density %g\n    foliage%d_scale_min %g\n    foliage%d_scale_max %g\n", n, L.name,
             n, L.models, n, L.density, n, L.smin, n, L.smax);
        sc.f("    foliage%d_distance %g\n    foliage%d_lod_distance %g\n    foliage%d_max_slope %g\n    foliage%d_align %s\n    foliage%d_shadows %s\n", n,
             L.distance, n, L.lod, n, L.slope, n, L.align ? "true" : "false", n, L.shadows ? "true" : "false");
        if (L.shader[0]) sc.f("    foliage%d_shader \"%s\"\n", n, L.shader);
    }
    sc.f("  end\nend\n\n");
    // the lake: one big flat mirror at the water's level (the ground hides it everywhere else)
    sc.f("object 2\n  name \"Lake Aresjon\"\n  kind mesh\n  position 0 %g 0\n  rotation 0 0 0 1\n  scale %g 1 %g\n  streaming always\n", w.water, size,
         size);
    sc.f("  mesh plane\n  color 0.06 0.11 0.13\n  roughness 0.04\n  metallic 0.7\n  unlit false\n  cast_shadows false\nend\n\n");
    // the sun: late afternoon from the south-west (light travels north-east and down)
    float q[4];
    look_quat(Vec3(0.55f, -0.42f, -0.72f), q);
    sc.f("object 3\n  name \"Sun\"\n  kind empty\n  position 0 1500 0\n  rotation %g %g %g %g\n  scale 1 1 1\n", q[0], q[1], q[2], q[3]);
    sc.f("  module light\n    type Directional\n    color 1 0.93 0.82\n    intensity 2.6\n    range 10\n    spot_angle 45\n    spot_blend 0.15\n    shadows true\n  end\nend\n\n");
    sc.f("object 4\n  name \"Sky\"\n  kind sky\n  position 0 0 0\n  rotation 0 0 0 1\n  scale 1 1 1\n");
    sc.f("  module sky\n    current true\n    mode Gradient\n    zenith 0.22 0.42 0.74\n    horizon 0.70 0.79 0.88\n    ground 0.30 0.30 0.27\n    sun_size 1.2\n    intensity 1\n    fog 0.0015\n  end\nend\n\n");
    sc.f("object 5\n  name \"Sky Light\"\n  kind skylight\n  position 0 0 0\n  rotation 0 0 0 1\n  scale 1 1 1\n");
    sc.f("  module skylight\n    current true\n    sky_color 0.55 0.66 0.82\n    ground_color 0.24 0.25 0.18\n    intensity 0.75\n    gi_intensity 1\n    gi_bounce 0.8\n  end\nend\n\n");
    sc.f("object 6\n  name \"Clouds\"\n  kind clouds\n  position 0 0 0\n  rotation 0 0 0 1\n  scale 1 1 1\n");
    sc.f("  module clouds\n    current true\n    quality Ultra\n    height %g\n    thickness 900\n    layers 2\n    layer_spacing 900\n    coverage 0.46\n", w.hmax + 700.0f);
    sc.f("    density 0.8\n    color 1 1 1\n    scale 1\n    wind 9 0 4\n    sun_glow 0.6\n    ambient 1\n    distance 40000\n  end\nend\n\n");
    sc.f("object 7\n  name \"Post Processing\"\n  kind post_process\n  position 0 0 0\n  rotation 0 0 0 1\n  scale 1 1 1\n");
    sc.f("  module post_process\n    current true\n    tonemap AgX\n    exposure 0\n    bloom 0.3\n    auto_exposure true\n  end\nend\n\n");
    look_quat(pts[1] - pts[0], q);
    sc.f("object 8\n  name \"Flythrough Camera\"\n  kind camera\n  position %g %g %g\n  rotation %g %g %g %g\n  scale 1 1 1\n", pts[0].x, pts[0].y, pts[0].z,
         q[0], q[1], q[2], q[3]);
    sc.f("  module camera\n    current true\n    projection Perspective\n    fov 65\n    ortho_size 5\n    near_plane 0.2\n    far_plane 9000\n  end\n");
    sc.f("  script \"scripts/flythrough.lua\"\n  end\nend\n");
    snprintf(path, sizeof path, "%s/scenes/main.fscene", out);
    if (!write_file(path, sc.p, sc.n)) { fprintf(stderr, "fe-worldgen: cannot write %s\n", path); return 1; }

    // ---- the flythrough script
    Text lua;
    lua.f("-- Flies the camera along a loop over the valley (made by fe-worldgen) and shows the frame time.\n");
    lua.f("-- B: a benchmark lap (logs the frame time min / average / max to the console). Space: pause.\n");
    lua.f("local path = {\n");
    for (int i = 0; i < kPath; i++) lua.f("  fe.vec3(%.1f, %.1f, %.1f),\n", pts[i].x, pts[i].y, pts[i].z);
    lua.f("}\nlocal lap_seconds = 120\n\n");
    lua.f("local function catmull(p0, p1, p2, p3, t)\n"
          "  local t2, t3 = t * t, t * t * t\n"
          "  return (p1 * 2 + (p2 - p0) * t + (p0 * 2 - p1 * 5 + p2 * 4 - p3) * t2 + (p1 * 3 - p0 - p2 * 3 + p3) * t3) * 0.5\n"
          "end\n\n"
          "local function at(u)\n"
          "  local n = #path\n"
          "  local f = (u %% 1) * n\n"
          "  local i = math.floor(f)\n"
          "  local t = f - i\n"
          "  local function p(k) return path[(k %% n) + 1] end\n"
          "  return catmull(p(i - 1), p(i), p(i + 1), p(i + 2), t)\n"
          "end\n\n"
          "function on_start(self)\n"
          "  self.u = 0\n"
          "  self.paused = false\n"
          "  self.bench = nil\n"
          "  self.avg = 0.016\n"
          "end\n\n"
          "function on_update(self, dt)\n"
          "  if fe.key_pressed(\"space\") then self.paused = not self.paused end\n"
          "  if fe.key_pressed(\"b\") and not self.bench then\n"
          "    self.bench = { t = 0, n = 0, sum = 0, min = 1e9, max = 0 }\n"
          "    self.u = 0\n"
          "    print(\"Benchmark lap started\")\n"
          "  end\n"
          "  self.avg = self.avg * 0.95 + dt * 0.05\n"
          "  if not self.paused then self.u = self.u + dt / lap_seconds end\n"
          "  local p = at(self.u)\n"
          "  local q = at(self.u + 0.004)\n"
          "  local d = (q - p):normalized()\n"
          "  self.object.position = p\n"
          "  local yaw = math.deg(math.atan(-d.x, -d.z))\n"
          "  local pitch = math.deg(math.asin(math.max(-1, math.min(1, d.y)))) - 6\n"
          "  self.object.rotation = fe.vec3(pitch, yaw, 0)\n"
          "  local b = self.bench\n"
          "  if b then\n"
          "    b.t = b.t + dt; b.n = b.n + 1; b.sum = b.sum + dt\n"
          "    if dt < b.min then b.min = dt end\n"
          "    if dt > b.max then b.max = dt end\n"
          "    if b.t >= lap_seconds then\n"
          "      print(string.format(\"Benchmark: %%d frames, %%.1f ms average, %%.1f ms best, %%.1f ms worst (%%.1f fps)\",\n"
          "        b.n, b.sum / b.n * 1000, b.min * 1000, b.max * 1000, b.n / b.sum))\n"
          "      self.bench = nil\n"
          "    end\n"
          "    fe.status(string.format(\"Benchmark lap: %%.0f s left, %%.1f ms\", lap_seconds - b.t, self.avg * 1000), b.t / lap_seconds)\n"
          "  else\n"
          "    fe.status(string.format(\"%%.1f ms (%%.0f fps)   B: benchmark lap   Space: pause\", self.avg * 1000, 1 / self.avg))\n"
          "  end\n"
          "end\n");
    snprintf(path, sizeof path, "%s/scripts/flythrough.lua", out);
    if (!write_file(path, lua.p, lua.n)) return 1;

    // ---- project.fe and graphics.cfg
    Text pj;
    pj.f("farcade-project 1\nname \"%s\"\nversion \"1.0\"\nentry_scene \"scenes/main.fscene\"\nwindow_width 1280\nwindow_height 720\ngraphics true\n", name);
    snprintf(path, sizeof path, "%s/project.fe", out);
    if (!write_file(path, pj.p, pj.n)) return 1;
    const char* gfx = "farcade-graphics 1\npreset ultra\n";
    snprintf(path, sizeof path, "%s/graphics.cfg", out);
    if (!write_file(path, gfx, strlen(gfx))) return 1;
    printf("fe-worldgen: wrote %s (docs/example-world.md: open it with farcade-editor.run --project, press Play to fly)\n", out);
    free(w.h);
    return 0;
}
