// Fills a FarcadeGL RenderScene from the editor's Scene: the mesh objects,
// the lights (Light modules at their objects' world transforms) and the
// current sky, sky light and post processing.
#pragma once

#include "gfx/renderer.h"
#include "scene.h"
#include "terrain_bridge.h"

namespace fe {

struct RenderBridge {
    // Storage the RenderScene points into (kept between frames).
    gfx::DrawItem* items = nullptr;
    int item_cap = 0;
    gfx::RenderLight* lights = nullptr;
    int light_cap = 0;
    gfx::RenderScene scene;
    // Terrains (chunks, foliage) and streaming, updated by build() from the eye.
    TerrainView terrain;
    bool playing = false;               // streaming applies to every terrain, Unloaded objects stay out
    int unloaded = 0;                   // objects left out by streaming in the last build()

    ~RenderBridge();
    // The view: from the given matrices (the editor's own camera).
    void build(Scene& sc, const Mat4& view, const Mat4& proj, const Vec3& eye, float near_plane, float far_plane);
    // The view of a Camera object (its world transform and settings), for
    // the game view. False when the object is not a camera.
    static bool camera_view(Scene& sc, ObjectId cam, float aspect, Mat4* view, Mat4* proj, Vec3* eye, float* near_plane, float* far_plane);
};

}  // namespace fe
