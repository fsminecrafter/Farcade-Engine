// The demo scene of fe-gfx-shot: ground, cubes, spheres, three lights, gradient sky.
#pragma once
#include "gfx/renderer.h"
#include "gfx/meshgen.h"

namespace fe::gfx {

struct DemoScene {
    static constexpr int kSeg = 32, kRings = 16;
    Vec3 cube_p[kCubeVertices], cube_n[kCubeVertices];
    Vec3 plane_p[kPlaneVertices], plane_n[kPlaneVertices];
    Vec3 sph_p[kSeg * kRings * 6], sph_n[kSeg * kRings * 6];
    DrawItem items[8];
    RenderLight lights[3];
    RenderScene scene;

    void build(float aspect) {
        make_cube(cube_p, cube_n, 0.5f);
        make_plane(plane_p, plane_n, 12.0f, 0.0f);
        make_sphere(sph_p, sph_n, 0.5f, kSeg, kRings);
        RenderMesh cube{ cube_p, cube_n, kCubeVertices, 1 };
        RenderMesh plane{ plane_p, plane_n, kPlaneVertices, 2 };
        RenderMesh sphere{ sph_p, sph_n, (uint32_t)sphere_vertices(kSeg, kRings), 3 };
        int n = 0;
        auto add = [&](const RenderMesh& m, Vec3 pos, Vec3 scl, float yaw, Vec3 col, float rough, bool unlit = false) {
            DrawItem& d = items[n++];
            d.mesh = m;
            d.model = Mat4::trs(pos, Quat::axis_angle(Vec3(0, 1, 0), yaw), scl);
            d.color = col;
            d.roughness = rough;
            d.unlit = unlit;
        };
        add(plane, Vec3(0, 0, 0), Vec3(1, 1, 1), 0, Vec3(0.42f, 0.5f, 0.38f), 0.9f);
        add(cube, Vec3(-1.6f, 0.5f, -0.5f), Vec3(1, 1, 1), 0.4f, Vec3(0.85f, 0.25f, 0.2f), 0.6f);
        add(cube, Vec3(1.4f, 0.75f, -1.8f), Vec3(1.5f, 1.5f, 1.5f), -0.3f, Vec3(0.25f, 0.45f, 0.85f), 0.35f);
        add(sphere, Vec3(0.1f, 0.5f, 0.6f), Vec3(1, 1, 1), 0, Vec3(0.9f, 0.8f, 0.3f), 0.25f);
        add(sphere, Vec3(-0.6f, 0.3f, 2.0f), Vec3(0.6f, 0.6f, 0.6f), 0, Vec3(0.3f, 0.8f, 0.45f), 0.8f);
        add(sphere, Vec3(2.6f, 0.4f, 0.8f), Vec3(0.8f, 0.8f, 0.8f), 0, Vec3(1.0f, 0.4f, 0.8f), 0.5f, true);
        scene.items = items;
        scene.item_count = n;

        RenderLight& sun = lights[0];
        sun.type = LightType::Directional;
        sun.direction = normalize(Vec3(-0.3f, -0.21f, 0.93f));   // from behind the scene, low: the sun disc shows
        sun.color = Vec3(1.0f, 0.93f, 0.8f);
        sun.intensity = 1.6f;
        RenderLight& pt = lights[1];
        pt.type = LightType::Point;
        pt.position = Vec3(-0.5f, 1.4f, 1.2f);
        pt.color = Vec3(1.0f, 0.45f, 0.2f);
        pt.intensity = 2.2f;
        pt.range = 4.5f;
        RenderLight& sp = lights[2];
        sp.type = LightType::Spot;
        sp.position = Vec3(3.0f, 3.2f, 1.5f);
        sp.direction = normalize(Vec3(-0.5f, -1.0f, -0.2f));
        sp.color = Vec3(0.45f, 0.7f, 1.0f);
        sp.intensity = 4.0f;
        sp.range = 9.0f;
        sp.spot_angle = 40.0f;
        sp.spot_blend = 0.3f;
        scene.lights = lights;
        scene.light_count = 3;

        scene.camera.position = Vec3(1.2f, 1.7f, 6.6f);
        scene.camera.view = Mat4::look_at(scene.camera.position, Vec3(0, 1.1f, 0), Vec3(0, 1, 0));
        scene.camera.proj = Mat4::perspective(radians(50.0f), aspect, 0.1f, 200.0f);
        scene.sky.zenith = Vec3(0.12f, 0.30f, 0.65f);
        scene.sky.horizon = Vec3(0.55f, 0.68f, 0.85f);
        scene.sky.ground = Vec3(0.22f, 0.22f, 0.22f);
        scene.sky_light.intensity = 0.45f;
        scene.post.tonemap = Tonemap::ACES;
    }
};

}  // namespace fe::gfx
