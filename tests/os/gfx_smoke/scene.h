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

inline uint32_t checksum(const uint32_t* px, int count) {
    uint32_t h = 2166136261u;                   // FNV-1a over the RGBA words
    for (int i = 0; i < count; i++) {
        uint32_t v = px[i];
        for (int b = 0; b < 4; b++) { h ^= (v >> (b * 8)) & 0xFFu; h *= 16777619u; }
    }
    return h;
}

}  // namespace fe::gfx
