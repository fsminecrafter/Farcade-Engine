#include "render_bridge.h"
#include "model_cache.h"

#include <stdlib.h>

namespace fe {

RenderBridge::~RenderBridge() {
    free(items);
    free(lights);
}

void RenderBridge::build(Scene& sc, const Mat4& view, const Mat4& proj, const Vec3& eye, float near_plane, float far_plane) {
    scene = gfx::RenderScene();
    scene.camera.view = view;
    scene.camera.proj = proj;
    scene.camera.position = eye;
    scene.camera.near_plane = near_plane;
    scene.camera.far_plane = far_plane;
    int n = sc.count();
    terrain.update(sc, eye, playing);
    // A model object is one draw item per part (material).
    int need = n + terrain.item_count();
    for (int i = 0; i < n; i++)
        if (const Model* mod = Scene::model_for(sc.at(i))) need += mod->part_count;
    if (item_cap < need || light_cap < n) {
        int cap = need < 64 ? 64 : need * 2;
        items = (gfx::DrawItem*)realloc(items, sizeof(gfx::DrawItem) * cap);
        lights = (gfx::RenderLight*)realloc(lights, sizeof(gfx::RenderLight) * cap);
        item_cap = light_cap = cap;
    }
    int ni = 0, nl = 0;
    unloaded = 0;
    for (int i = 0; i < n; i++) {
        Object& o = sc.at(i);
        if (!sc.visible_in_tree(o)) continue;
        if (!terrain.loaded(sc, o)) { unloaded++; continue; }
        Mat4 m = sc.world_matrix(o);
        if (const Model* mod = Scene::model_for(o)) {
            // The object's color tints the material colors (the default grey 0.8 is neutral).
            Vec3 tint(fminf_(o.color.x * 1.25f, 1.0f), fminf_(o.color.y * 1.25f, 1.0f), fminf_(o.color.z * 1.25f, 1.0f));
            for (int pi = 0; pi < mod->part_count; pi++) {
                const ModelPart& pt = mod->parts[pi];
                gfx::DrawItem& d = items[ni++];
                d = gfx::DrawItem();
                d.mesh.positions = pt.positions;
                d.mesh.normals = pt.normals;
                d.mesh.uvs = pt.uvs;
                d.mesh.vertex_count = pt.vertex_count;
                d.mesh.key = (uint64_t)(uintptr_t)pt.positions;
                d.model = m;
                d.texture_path = o.texture_path[0] ? o.texture_path : (pt.texture[0] ? pt.texture : nullptr);
                d.shader_path = o.shader_path;
                d.color = Vec3(pt.color.x * tint.x, pt.color.y * tint.y, pt.color.z * tint.z);
                d.alpha_cut = o.texture_path[0] ? 0.0f : pt.alpha_cut;     // an object texture replaces the material's
                d.double_sided = pt.double_sided;
                d.roughness = o.roughness;
                d.metallic = o.metallic;
                d.unlit = o.unlit;
                d.cast_shadows = o.cast_shadows;
            }
        } else if (o.kind == ObjectKind::Mesh) {
            const MeshData& md = Scene::mesh_for(o);
            gfx::DrawItem& d = items[ni++];
            d = gfx::DrawItem();
            d.mesh.positions = md.positions;
            d.mesh.normals = md.normals;
            d.mesh.vertex_count = md.vertex_count;
            d.mesh.key = (uint64_t)(uintptr_t)md.positions;     // built-in meshes never change
            d.model = m;
            d.texture_path = o.texture_path;
            d.shader_path = o.shader_path;
            d.color = o.color;
            d.roughness = o.roughness;
            d.metallic = o.metallic;
            d.unlit = o.unlit;
            d.cast_shadows = o.cast_shadows;
        }
        if (o.modules.has(ModuleKind::Light)) {
            const Light& l = o.modules.light;
            gfx::RenderLight& r = lights[nl++];
            r = gfx::RenderLight();
            r.type = l.type;
            r.position = Vec3(m.m[12], m.m[13], m.m[14]);
            r.direction = normalize(transform_dir(m, Vec3(0, 0, -1)));
            r.color = l.color;
            r.intensity = l.intensity;
            r.range = l.range;
            r.spot_angle = l.spot_angle;
            r.spot_blend = l.spot_blend;
            r.shadows = l.shadows;
        }
    }
    for (int i = 0; i < terrain.item_count(); i++) items[ni++] = terrain.items()[i];
    scene.items = items;
    scene.item_count = ni;
    scene.lights = lights;
    scene.light_count = nl;
    if (const Sky* s = (const Sky*)sc.current_data(ModuleKind::Sky)) scene.sky = *s;
    if (const SkyLight* s = (const SkyLight*)sc.current_data(ModuleKind::SkyLight)) scene.sky_light = *s;
    if (const PostProcess* p = (const PostProcess*)sc.current_data(ModuleKind::PostProcess)) scene.post = *p;
    if (const Clouds* c = (const Clouds*)sc.current_data(ModuleKind::Clouds)) { scene.clouds = *c; scene.clouds_on = true; }
    // The current camera's screen shaders.
    if (const Camera* c = (const Camera*)sc.current_data(ModuleKind::Camera)) {
        const char* paths[3] = { c->shader1, c->shader2, c->shader3 };
        for (const char* pth : paths)
            if (pth[0]) scene.screen_shaders[scene.screen_shader_count++] = pth;
        scene.screen_params[0] = c->shader_params.x;
        scene.screen_params[1] = c->shader_params.y;
        scene.screen_params[2] = c->shader_params.z;
    }
}

bool RenderBridge::camera_view(Scene& sc, ObjectId cam, float aspect, Mat4* view, Mat4* proj, Vec3* eye, float* near_plane, float* far_plane) {
    Object* o = sc.get(cam);
    if (!o || o->kind != ObjectKind::Camera) return false;
    const Camera& c = o->modules.camera;
    Mat4 w = sc.world_matrix(*o);
    // The view is the inverse of the camera's world transform without scale.
    Vec3 t, s;
    Quat r;
    decompose(w, &t, &r, &s);
    Mat4 rigid = Mat4::trs(t, r, Vec3(1, 1, 1));
    if (!invert(rigid, view)) return false;
    *proj = c.projection == Projection::Perspective ? Mat4::perspective(radians(c.fov), aspect, c.near_plane, c.far_plane)
                                                    : Mat4::orthographic(c.ortho_size, aspect, c.near_plane, c.far_plane);
    *eye = t;
    *near_plane = c.near_plane;
    *far_plane = c.far_plane;
    return true;
}

}  // namespace fe
