// Small triangle-list meshes for the renderer's tools, tests and smoke
// programs: counter-clockwise (outward facing) triangles with normals, in
// caller-provided arrays (no allocation).
#pragma once

#include "core/math.h"

namespace fe::gfx {

constexpr int kCubeVertices = 36;
constexpr int kPlaneVertices = 6;
inline int sphere_vertices(int segments, int rings) { return segments * rings * 6; }

// A cube with edge 2 * half, centered at the origin.
inline void make_cube(Vec3* pos, Vec3* nrm, float half = 0.5f) {
    static const Vec3 n[6] = { Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(0, -1, 0), Vec3(0, 0, 1), Vec3(0, 0, -1) };
    static const Vec3 u[6] = { Vec3(0, 1, 0), Vec3(0, 0, 1), Vec3(0, 0, 1), Vec3(1, 0, 0), Vec3(1, 0, 0), Vec3(0, 1, 0) };
    static const Vec3 v[6] = { Vec3(0, 0, 1), Vec3(0, 1, 0), Vec3(1, 0, 0), Vec3(0, 0, 1), Vec3(0, 1, 0), Vec3(1, 0, 0) };
    static const int corner[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    int k = 0;
    for (int f = 0; f < 6; f++) {
        for (int c = 0; c < 6; c++) {
            pos[k] = (n[f] + u[f] * (float)corner[c][0] + v[f] * (float)corner[c][1]) * half;
            nrm[k] = n[f];
            k++;
        }
    }
}

// A square in the XZ plane at height y facing up, edge 2 * half.
inline void make_plane(Vec3* pos, Vec3* nrm, float half, float y = 0) {
    static const int c[6][2] = { { -1, 1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };
    for (int i = 0; i < 6; i++) {
        pos[i] = Vec3((float)c[i][0] * half, y, (float)c[i][1] * half);
        nrm[i] = Vec3(0, 1, 0);
    }
}

// A UV sphere: sphere_vertices(segments, rings) vertices.
inline void make_sphere(Vec3* pos, Vec3* nrm, float radius, int segments, int rings) {
    auto point = [&](int i, int j) {
        float th = kPi * (float)i / (float)rings, ph = 2.0f * kPi * (float)j / (float)segments;
        return Vec3(fsin(th) * fcos(ph), fcos(th), fsin(th) * fsin(ph));
    };
    int k = 0;
    for (int i = 0; i < rings; i++) {
        for (int j = 0; j < segments; j++) {
            Vec3 a = point(i, j), b = point(i, j + 1), c = point(i + 1, j), d = point(i + 1, j + 1);
            const Vec3 tri[6] = { a, b, c, b, d, c };
            for (const Vec3& p : tri) { pos[k] = p * radius; nrm[k] = p; k++; }
        }
    }
}

}  // namespace fe::gfx
