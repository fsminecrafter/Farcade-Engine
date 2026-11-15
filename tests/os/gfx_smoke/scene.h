// The scene of the Minimal-OS smoke program (main.cpp), shared with
// tests/host/gfx_tests.cpp, which prints the checksum the host renders for it:
// a lit cube on a ground plane under a sun, a gradient sky, ACES.
#pragma once

#include "gfx/renderer.h"
#include "gfx/meshgen.h"

namespace fe::gfx {

constexpr int kSmokeWidth = 160, kSmokeHeight = 120;

struct SmokeScene {
    Vec3 cube_p[kCubeVertices], cube_n[kCubeVertices];
    Vec3 plane_p[kPlaneVertices], plane_n[kPlaneVertices];
    DrawItem items[2];
    RenderLight sun;
    RenderScene scene;

    void build() {
        make_cube(cube_p, cube_n, 0.5f);
        make_plane(plane_p, plane_n, 10.0f, -0.5f);
        items[0].mesh = RenderMesh{ plane_p, plane_n, kPlaneVertices, 101 };
        items[0].model = Mat4::identity();
        items[0].color = Vec3(0.4f, 0.45f, 0.4f);
        items[1].mesh = RenderMesh{ cube_p, cube_n, kCubeVertices, 102 };
        items[1].model = Mat4::rotation(Quat::axis_angle(Vec3(0, 1, 0), 0.5f));
        items[1].color = Vec3(0.85f, 0.3f, 0.25f);
        sun.type = LightType::Directional;
        sun.direction = normalize(Vec3(-0.6f, -0.7f, -0.4f));
        sun.intensity = 1.5f;
        scene.items = items;
        scene.item_count = 2;
        scene.lights = &sun;
        scene.light_count = 1;
        scene.camera.position = Vec3(2.2f, 1.6f, 3.0f);
        scene.camera.view = Mat4::look_at(scene.camera.position, Vec3(0, 0, 0), Vec3(0, 1, 0));
        scene.camera.proj = Mat4::perspective(radians(55.0f), (float)kSmokeWidth / (float)kSmokeHeight, 0.1f, 100.0f);
        scene.post.tonemap = Tonemap::ACES;
    }
};

// A point and a spot light with shadows next to the sun (the third checksum:
// the GPU tier's point / spot shadow atlas).
struct SmokeLocalLights {
    RenderLight lights[3];
    void apply(SmokeScene& s) {
        lights[0] = s.sun;
        lights[1].type = LightType::Point;
        lights[1].position = Vec3(1.2f, 1.0f, 0.9f);
        lights[1].color = Vec3(1.0f, 0.8f, 0.5f);
        lights[1].intensity = 2.0f;
        lights[1].range = 6.0f;
        lights[2].type = LightType::Spot;
        lights[2].position = Vec3(-1.4f, 2.4f, 0.6f);
        lights[2].direction = normalize(Vec3(1.4f, -2.4f, -0.6f));
        lights[2].color = Vec3(0.5f, 0.8f, 1.0f);
        lights[2].intensity = 3.0f;
        lights[2].range = 8.0f;
        lights[2].spot_angle = 60.0f;
        s.scene.lights = lights;
        s.scene.light_count = 3;
    }
};

inline uint32_t checksum(const uint32_t* px, int count) {
    uint32_t h = 2166136261u;                   // FNV-1a over the RGBA words
    for (int i = 0; i < count; i++) {
        uint32_t v = px[i];
        for (int b = 0; b < 4; b++) { h ^= (v >> (b * 8)) & 0xFFu; h *= 16777619u; }
    }
    return h;
}

inline int smoke_luma(uint32_t p) {
    int r = (int)(p & 0xFF), g = (int)((p >> 8) & 0xFF), b = (int)((p >> 16) & 0xFF);
    return (r * 3 + g * 6 + b) / 10;
}

// The pixel a world point projects to.
inline uint32_t smoke_pixel_at(const RenderScene& s, const Renderer& r, Vec3 p) {
    Vec4 c = s.camera.proj * (s.camera.view * Vec4{ p.x, p.y, p.z, 1 });
    int x = (int)((c.x / c.w * 0.5f + 0.5f) * (float)r.width());
    int y = (int)((1.0f - (c.y / c.w * 0.5f + 0.5f)) * (float)r.height());
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= r.width()) x = r.width() - 1;
    if (y >= r.height()) y = r.height() - 1;
    return r.pixels()[y * r.width() + x];
}

// Draws the smoke scene and checks the picture: returns nullptr when all is
// well (*sum gets the checksum of the first frame), else why not. The Renderer's
// own message is in r.error(). Shared by the Minimal-OS program and the host test.
inline const char* smoke_check(Renderer& r, SmokeScene& s, uint32_t* sum) {
    if (!r.init(kSmokeWidth, kSmokeHeight)) return "init failed";
    if (!r.render(s.scene)) return "render failed";
    if (r.stats().uploads != 2 || r.stats().draws != 2) return "first frame: wrong uploads or draws";
    *sum = checksum(r.pixels(), kSmokeWidth * kSmokeHeight);

    // The sky fills the empty top corner and differs from the cube; the cube's
    // sun-facing top is lit.
    uint32_t sky = r.pixels()[2 * kSmokeWidth + 2];
    uint32_t top = smoke_pixel_at(s.scene, r, Vec3(0, 0.5f, 0));
    if ((sky & 0xFFFFFF) == 0 || (top & 0xFFFFFF) == (sky & 0xFFFFFF)) return "sky and cube look the same";
    if (smoke_luma(top) < 40) return "the lit top of the cube is dark";
    int differing = 0;
    for (int i = 0; i < kSmokeWidth * kSmokeHeight; i++)
        if (r.pixels()[i] != sky) differing++;
    if (differing < kSmokeWidth * kSmokeHeight / 8) return "picture is almost flat";
    if ((top >> 24) != 0xFF) return "alpha is not 255";

    // A second frame reuses the cached meshes and draws the same picture.
    if (!r.render(s.scene)) return "second render failed";
    if (r.stats().uploads != 0) return "second frame uploaded meshes again";
    if (checksum(r.pixels(), kSmokeWidth * kSmokeHeight) != *sum) return "second frame differs";

    // Resize keeps the meshes.
    if (!r.resize(kSmokeWidth / 2, kSmokeHeight / 2)) return "resize failed";
    if (!r.render(s.scene) || r.stats().uploads != 0) return "render after resize";
    return nullptr;
}

}  // namespace fe::gfx
