// The 3D viewport: grid, objects, picking, camera controls and gizmos.
//
// For now the scene is drawn by a small depth-buffered CPU renderer
// (view3d.h) into an image, the same on the host and on Minimal-OS. The
// FarcadeGL renderer replaces that drawing later; picking, camera and
// gizmos stay.
#include "editor.h"
#include "render_bridge.h"
#include "model_cache.h"
#include "icons.h"
#include "swraster.h"
#include "view3d.h"
#include "fx_bridge.h"
#include "view_quality.h"
#include "platform/platform.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imguizmo/ImGuizmo.h"

#include <stdlib.h>
#include <string.h>

namespace fe {

namespace {

void* read_render_asset(const char* path, size_t* size, void* user) {
    return ((Editor*)user)->read_script_text(path, size);
}

// 0xRRGGBB for a base color under a light factor; selected objects get a
// touch of the accent color.
uint32_t shade(const Vec3& base, float light, bool selected) {
    Vec3 c = base * light;
    if (selected) c = c * 0.8f + Vec3(0.30f, 0.55f, 1.0f) * 0.2f;
    auto ch = [](float v) { return (uint32_t)(fclamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return ch(c.x) << 16 | ch(c.y) << 8 | ch(c.z);
}

struct View {
    ImVec2 pos, size;
    Mat4 view, proj, viewproj;
    float near_z = 0.1f;
    ImVec2 to_screen(const Vec3& view_space) const {
        Vec4 c = proj * Vec4{ view_space.x, view_space.y, view_space.z, 1.0f };
        float iw = 1.0f / c.w;
        return ImVec2(pos.x + (c.x * iw * 0.5f + 0.5f) * size.x, pos.y + (0.5f - c.y * iw * 0.5f) * size.y);
    }
};

// Draws a world-space line, clipped against the near plane.
void line3(ImDrawList* dl, const View& v, Vec3 a, Vec3 b, ImU32 col, float thickness) {
    a = transform_point(v.view, a);
    b = transform_point(v.view, b);
    float zn = -v.near_z;
    if (a.z > zn && b.z > zn) return;
    if (a.z > zn) a = a + (b - a) * ((zn - a.z) / (b.z - a.z));
    else if (b.z > zn) b = b + (a - b) * ((zn - b.z) / (a.z - b.z));
    dl->AddLine(v.to_screen(a), v.to_screen(b), col, thickness);
}

Ray mouse_ray(const View& v, ImVec2 mouse) {
    float nx = (mouse.x - v.pos.x) / v.size.x * 2.0f - 1.0f;
    float ny = 1.0f - (mouse.y - v.pos.y) / v.size.y * 2.0f;
    Mat4 inv;
    invert(v.viewproj, &inv);
    Vec4 n = inv * Vec4{ nx, ny, -1.0f, 1.0f };
    Vec4 f = inv * Vec4{ nx, ny, 1.0f, 1.0f };
    Vec3 pn(n.x / n.w, n.y / n.w, n.z / n.w), pf(f.x / f.w, f.y / f.w, f.z / f.w);
    return { pn, normalize(pf - pn) };
}

bool ray_ground(const Ray& r, Vec3* hit) {
    if (r.dir.y > -1e-5f && r.dir.y < 1e-5f) return false;
    float t = -r.origin.y / r.dir.y;
    if (t < 0) return false;
    *hit = r.origin + r.dir * t;
    return true;
}

Quat conjugate(const Quat& q) { return { -q.x, -q.y, -q.z, q.w }; }

}  // namespace

// Everything the viewport image depends on, hashed (FNV-1a).
uint64_t Editor::view_hash(int w, int h) const {
    uint64_t hsh = 1469598103934665603ull;
    auto mix = [&](const void* data, size_t n) {
        const uint8_t* b = (const uint8_t*)data;
        for (size_t i = 0; i < n; i++) { hsh ^= b[i]; hsh *= 1099511628211ull; }
    };
    mix(&w, sizeof(w));
    mix(&h, sizeof(h));
    mix(&camera, sizeof(camera));
    mix(&view_mode, sizeof(view_mode));
    bool flags[4] = { settings.show_grid, view_shadows_now(), view_supersample_now(), settings.dark_theme };
    mix(flags, sizeof(flags));
    mix(&settings.grid_size, sizeof(settings.grid_size));
    Scene& sc = const_cast<Scene&>(scene);
    for (int i = 0; i < sc.count(); i++) {
        const Object& o = sc.at(i);
        mix(&o.id, sizeof(o.id));
        mix(&o.position, sizeof(o.position));
        mix(&o.rotation, sizeof(o.rotation));
        mix(&o.scale, sizeof(o.scale));
        mix(&o.color, sizeof(o.color));
        float mat[2] = { o.roughness, o.metallic };
        bool matf[2] = { o.unlit, o.cast_shadows };
        mix(mat, sizeof(mat));
        mix(matf, sizeof(matf));
        mix(&o.mesh, sizeof(o.mesh));
        mix(o.texture_path, sizeof(o.texture_path));
        mix(o.shader_path, sizeof(o.shader_path));
        mix(&o.kind, sizeof(o.kind));
        mix(&o.modules.present, sizeof(o.modules.present));
        mix(&o.modules.light, sizeof(o.modules.light));
        mix(&o.modules.camera, sizeof(o.modules.camera));
        mix(&o.modules.sky, sizeof(o.modules.sky));
        mix(&o.modules.sky_light, sizeof(o.modules.sky_light));
        mix(&o.modules.post_process, sizeof(o.modules.post_process));
        mix(&o.visible, sizeof(o.visible));
        mix(&o.parent, sizeof(o.parent));
    }
    for (int i = 0; i < sc.selected_count(); i++) { ObjectId id = sc.selected(i); mix(&id, sizeof(id)); }
    ObjectId act = sc.active();
    mix(&act, sizeof(act));
    return hsh;
}

// Lines for what has no mesh: a camera's frustum, a light's shape, a small
// cross for empties and the scene settings objects.
void Editor::draw_object_shape(View3D& view3d, const Object& o, const Mat4& model) {
    bool sel = scene.is_selected(o.id);
    uint32_t col = sel ? 0xFFB13B : (settings.dark_theme ? 0xAEB6C2 : 0x4A515C);
    int alpha = sel ? 255 : 170;
    auto P = [&](float x, float y, float z) { return transform_point(model, Vec3(x, y, z)); };
    auto L = [&](const Vec3& a, const Vec3& b) { view3d.line(a, b, col, alpha); };
    if (o.modules.has(ModuleKind::Light)) {
        const Light& l = o.modules.light;
        uint32_t lc = sel ? col : 0xF2C94C;
        auto LL = [&](const Vec3& a, const Vec3& b) { view3d.line(a, b, lc, alpha); };
        if (l.type == LightType::Directional) {
            // A few parallel rays along -Z.
            static const float off[5][2] = { { 0, 0 }, { 0.25f, 0 }, { -0.25f, 0 }, { 0, 0.25f }, { 0, -0.25f } };
            for (auto& d : off) {
                LL(P(d[0], d[1], 0), P(d[0], d[1], -1.2f));
                LL(P(d[0], d[1], -1.2f), P(d[0] + 0.07f, d[1], -1.05f));
                LL(P(d[0], d[1], -1.2f), P(d[0] - 0.07f, d[1], -1.05f));
            }
        } else if (l.type == LightType::Point) {
            for (int axis = 0; axis < 3; axis++)
                for (int i = 0; i < 16; i++) {
                    float a0 = 2 * kPi * i / 16, a1 = 2 * kPi * (i + 1) / 16, r = 0.25f;
                    float c0 = fcos(a0) * r, s0 = fsin(a0) * r, c1 = fcos(a1) * r, s1 = fsin(a1) * r;
                    if (axis == 0) LL(P(c0, s0, 0), P(c1, s1, 0));
                    else if (axis == 1) LL(P(c0, 0, s0), P(c1, 0, s1));
                    else LL(P(0, c0, s0), P(0, c1, s1));
                }
        } else {
            float d = l.range < 2.0f ? l.range : 2.0f;
            float r = ftan(radians(l.spot_angle * 0.5f)) * d;
            Vec3 tip = P(0, 0, 0);
            for (int i = 0; i < 24; i++) {
                float a0 = 2 * kPi * i / 24, a1 = 2 * kPi * (i + 1) / 24;
                LL(P(fcos(a0) * r, fsin(a0) * r, -d), P(fcos(a1) * r, fsin(a1) * r, -d));
                if (i % 6 == 0) LL(tip, P(fcos(a0) * r, fsin(a0) * r, -d));
            }
        }
    }
    switch (o.kind) {
        case ObjectKind::Camera: {
            const Camera& c = o.modules.camera;
            float d = 0.8f, hh, hw;
            if (c.projection == Projection::Perspective) hh = ftan(radians(c.fov * 0.5f)) * d;
            else hh = 0.45f;
            hw = hh * (16.0f / 9.0f);
            uint32_t cc = sel ? col : (c.current ? 0x6EA3FF : col);
            auto C = [&](const Vec3& a, const Vec3& b) { view3d.line(a, b, cc, alpha); };
            Vec3 e = P(0, 0, 0);
            Vec3 q[4] = { P(-hw, -hh, -d), P(hw, -hh, -d), P(hw, hh, -d), P(-hw, hh, -d) };
            if (c.projection == Projection::Orthographic) {
                Vec3 b[4] = { P(-hw, -hh, 0), P(hw, -hh, 0), P(hw, hh, 0), P(-hw, hh, 0) };
                for (int i = 0; i < 4; i++) { C(b[i], q[i]); C(b[i], b[(i + 1) % 4]); }
            } else {
                for (int i = 0; i < 4; i++) C(e, q[i]);
            }
            for (int i = 0; i < 4; i++) C(q[i], q[(i + 1) % 4]);
            // Which way is up: a small triangle on the top edge.
            C(P(-hw * 0.3f, hh * 1.1f, -d), P(0, hh * 1.45f, -d));
            C(P(hw * 0.3f, hh * 1.1f, -d), P(0, hh * 1.45f, -d));
            C(P(-hw * 0.3f, hh * 1.1f, -d), P(hw * 0.3f, hh * 1.1f, -d));
            break;
        }
        case ObjectKind::Empty:
            if (o.modules.has(ModuleKind::Light)) break;
            L(P(-0.25f, 0, 0), P(0.25f, 0, 0));
            L(P(0, -0.25f, 0), P(0, 0.25f, 0));
            L(P(0, 0, -0.25f), P(0, 0, 0.25f));
            break;
        default: break;
    }
}

void Editor::render_view(View3D& view3d, const void* view_ptr) {
    const View& v = *(const View*)view_ptr;
    double (*clock)() = perf_clock ? perf_clock : platform::time_seconds;
    double t0 = clock();

    uint32_t top = settings.dark_theme ? 0x2B3038 : 0xC9D0DA;
    uint32_t bottom = settings.dark_theme ? 0x16181C : 0xEEF1F5;
    view3d.begin(render_w_, render_h_, v.view, v.proj, top, bottom, view_supersample_now() ? 2 : 1);
    bool wire = view_mode == ViewMode::Wireframe;
    {
        uint16_t sel_ids[256];
        int nsel = 0;
        for (int s2 = 0; s2 < scene.selected_count() && nsel < 256; s2++) sel_ids[nsel++] = (uint16_t)scene.selected(s2);
        view3d.select(sel_ids, wire ? 0 : nsel, wire ? 0 : (uint16_t)scene.active());
    }
    int wire_tris = 0;
    for (int i = 0; i < scene.count(); i++) {
        Object& o = scene.at(i);
        if (!scene.visible_in_tree(o)) continue;
        Mat4 model = scene.world_matrix(o);
        draw_object_shape(view3d, o, model);
        if (o.kind != ObjectKind::Mesh) continue;
        const MeshData& mesh = Scene::mesh_for(o);
        if (!wire) {
            view3d.mesh(mesh.positions, mesh.normals, mesh.vertex_count, model, o.color, (uint16_t)o.id, view_mode == ViewMode::Unlit || o.unlit);
            continue;
        }
        bool sel = scene.is_selected(o.id);
        uint32_t wc = sel ? 0x6EA3FF : (settings.dark_theme ? 0xC8CDD5 : 0x3A3F47);
        for (uint32_t t = 0; t + 2 < mesh.vertex_count; t += 3) {
            Vec3 a = transform_point(model, mesh.positions[t]);
            Vec3 b = transform_point(model, mesh.positions[t + 1]);
            Vec3 c = transform_point(model, mesh.positions[t + 2]);
            wire_tris++;
            view3d.line(a, b, wc, 190);
            view3d.line(b, c, wc, 190);
            view3d.line(c, a, wc, 190);
        }
    }
    // Shadows on the ground from everything that is not flat on it.
    if (view_shadows_now() && !wire && view_mode != ViewMode::Unlit) {
        for (int i = 0; i < scene.count(); i++) {
            Object& o = scene.at(i);
            if (!scene.visible_in_tree(o) || o.kind != ObjectKind::Mesh || o.mesh == MeshKind::Plane) continue;
            const MeshData& mesh = Scene::mesh_for(o);
            view3d.shadow(mesh.positions, mesh.vertex_count, scene.world_matrix(o));
        }
    }
    if (settings.show_grid) view3d.grid(settings.grid_size, settings.dark_theme);
    // Shadows, smoothing, and the selection outline (the active object brighter).
    view3d.finish(0xFFB13B, 0xD98A1C, settings.dark_theme ? 0.42f : 0.30f);
    view_triangles = view3d.triangles() + wire_tris;
    view_ms = (float)((clock() - t0) * 1000.0);
    view_renders++;
}

void Editor::panel_viewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    View v;
    v.pos = ImGui::GetCursorScreenPos();
    v.size = ImGui::GetContentRegionAvail();
    if (v.size.x < 16 || v.size.y < 16) { ImGui::End(); return; }
    view_rect[0] = v.pos.x; view_rect[1] = v.pos.y; view_rect[2] = v.size.x; view_rect[3] = v.size.y;
    v.view = camera.view();
    v.proj = camera.projection(v.size.x / v.size.y);
    Vec3 eye = camera.eye();
    float znear = 0.1f, zfar = 500.0f;
    // In Play, the view of the current Camera object (the game's view).
    looking_through_camera = false;
    if (playing() && settings.play_through_camera) {
        ObjectId cam = scene.current(ModuleKind::Camera);
        if (cam && RenderBridge::camera_view(scene, cam, v.size.x / v.size.y, &v.view, &v.proj, &eye, &znear, &zfar))
            looking_through_camera = true;
    }
    v.viewproj = v.proj * v.view;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 end(v.pos.x + v.size.x, v.pos.y + v.size.y);
    dl->PushClipRect(v.pos, end, true);

    // The scene into the viewport image, redrawn only when something it
    // shows has changed (the CPU preview is not cheap; idle frames are free).
    // With adaptive_view the image is rendered at 1/d of the size while the
    // scene keeps changing (view_quality.h) and once more, sharper, when it
    // stopped.
    static View3D view3d;
    static uint64_t last_hash = 0, last_base = 0;
    static gfx::Renderer fgl;
    static RenderBridge bridge;
    static SwTexture fgl_tex, shown_tex;
    static SwUpscaler upscaler;
    static ViewQuality quality_fgl(true), quality_cpu(false);
    static bool fgl_failed = false;
    bool use_fgl = view_renderer == ViewRenderer::FarcadeGL && !fgl_failed;
    ViewQuality& quality = use_fgl ? quality_fgl : quality_cpu;
    const int full_w = (int)v.size.x, full_h = (int)v.size.y;
    uint64_t hash, base;
    {
        // base: what the image shows, as if nothing were dragged (shadows and
        // smoothing come back after a drag, that alone is no scene change).
        bool keep_a = view_interacting_, keep_b = bench_interacting;
        view_interacting_ = bench_interacting = false;
        base = view_hash(full_w, full_h);
        view_interacting_ = keep_a;
        bench_interacting = keep_b;
        hash = view_hash(full_w, full_h);
        uint8_t extra[2] = { (uint8_t)use_fgl, (uint8_t)looking_through_camera };
        uint64_t* hps[2] = { &hash, &base };
        for (uint64_t* hp : hps) {
            for (uint8_t eb : extra) { *hp ^= eb; *hp *= 1099511628211ull; }
            // The graphics settings change the picture.
            const uint8_t* gb = (const uint8_t*)&settings.graphics;
            for (size_t i = 0; i < sizeof(settings.graphics); i++) { *hp ^= gb[i]; *hp *= 1099511628211ull; }
            if (looking_through_camera)
                for (int i = 0; i < 16; i++) { uint32_t u; memcpy(&u, &v.view.m[i], 4); *hp ^= u; *hp *= 1099511628211ull; }
        }
    }
    int div = 1;
    bool moving = false;
    if (adaptive_view) {
        moving = view_interacting() || base != last_base;
        div = moving ? quality.interactive_div(full_w, full_h) : quality.settled_div(full_w, full_h);
        view_div_target = quality.settled_div(full_w, full_h);
    }
    if (div < view_scale_div) div = view_scale_div;
    last_base = base;
    { uint8_t d8 = (uint8_t)(div | (use_fgl && moving ? 0x80 : 0)); hash ^= d8; hash *= 1099511628211ull; }
    // FarcadeGL with adaptive_view: once the view stopped changing, the picture
    // is redrawn at full size in bands (a few rows per frame, so the UI stays
    // alive); a change of the view drops what is left of it.
    static bool band_final = true, banding = false;
    static int band_y = 0, band_rows = 0;
    // Bands need the base tier without fog (the GPU tier's passes work on
    // whole images): with those on, settled views are rendered whole.
    fgl.set_settings(settings.graphics);
    const Sky* cur_sky = (const Sky*)scene.current_data(ModuleKind::Sky);
    const bool whole_only = fgl.features().gpu_tier || (cur_sky && cur_sky->fog > 0.0f);
    const bool progressive = adaptive_view && use_fgl && !whole_only;
    bool need = hash != last_hash || (!use_fgl && !view3d.width()) || (use_fgl && !fgl_tex.pixels);
    if (progressive) {
        need = (moving && hash != last_hash) || !fgl_tex.pixels;
        if (moving) { band_final = false; banding = false; }
    }
    view_final = !progressive || (band_final && !need);
    bool do_band = progressive && !need && !band_final && shown_tex.pixels && fgl.ready();
    if (do_band) {
        double (*clock)() = perf_clock ? perf_clock : platform::time_seconds;
        double t0 = clock();
        const uint32_t* dst_base = nullptr;
        if (!banding) {
            // Start: the picture shown now becomes the full-size buffer the bands overwrite.
            int rows = (int)(250.0 / (quality.cost_ms_per_pixel(false) > 0 ? quality.cost_ms_per_pixel(false) * full_w : 0.05 * full_w));
            if (rows < 8) rows = 8;
            if (rows > full_h) rows = full_h;
            band_rows = rows;
            band_y = 0;
            banding = true;
            const uint32_t* cur = (const uint32_t*)shown_tex.pixels;
            if (shown_tex.format == SwTexture::XRGB8 && shown_tex.width == full_w && shown_tex.height == full_h && cur == upscaler.buffer(full_w, full_h)) {
                dst_base = cur;
            } else {
                dst_base = upscaler.run(cur, shown_tex.width, shown_tex.height, full_w, full_h);
            }
            shown_tex.format = SwTexture::XRGB8;
            shown_tex.pixels = (const uint8_t*)dst_base;
            shown_tex.width = shown_tex.pitch = full_w;
            shown_tex.height = full_h;
        }
        uint32_t* disp = upscaler.buffer(full_w, full_h);
        bool ok = disp && ((fgl.width() == full_w && fgl.height() == full_h) || fgl.resize(full_w, full_h));
        int y = band_y + band_rows > full_h ? full_h - band_rows : band_y;
        if (ok) {
            bridge.build(scene, v.view, v.proj, eye, znear, zfar);
            static ModelAssetReader assets{ read_render_asset, this };
            bridge.scene.read_asset = model_asset_reader;
            bridge.scene.asset_user = &assets;
            bridge.scene.draft = false;
            ok = fgl.render_rows(bridge.scene, y, band_rows, disp + (size_t)y * full_w);
        }
        if (ok) {
            view_ms = (float)((clock() - t0) * 1000.0);
            view_renders++;
            fgl_stats = fgl.stats();
            quality.report((long)full_w * band_rows, view_ms, false);
            shown_tex.version++;
            band_y += band_rows;
            if (band_y >= full_h) { band_final = true; banding = false; view_div_shown = 1; view_final = true; }
        } else {
            band_final = true;
            banding = false;
        }
    }
    // A refine render only when it brings something (never to the same size).
    if (need) {
        last_hash = hash;
        int rw = div > 1 ? (full_w + div - 1) / div : full_w, rh = div > 1 ? (full_h + div - 1) / div : full_h;
        if (rw < 16) rw = 16;
        if (rh < 16) rh = 16;
        render_w_ = rw;
        render_h_ = rh;
        double (*clock)() = perf_clock ? perf_clock : platform::time_seconds;
        double t0 = clock();
        const uint32_t* rgba = nullptr;
        if (use_fgl) {
            bool first = !fgl.ready();
            bool ok = fgl.ready() ? ((fgl.width() == rw && fgl.height() == rh) || fgl.resize(rw, rh)) : fgl.init(rw, rh);
            if (first) { fgl_init_ms = fgl.init_ms(); fgl_init_shader_ms = fgl.init_shader_ms(); }
            if (ok) {
                fgl.set_settings(settings.graphics);
                bridge.build(scene, v.view, v.proj, eye, znear, zfar);
                static ModelAssetReader assets{ read_render_asset, this };
                bridge.scene.read_asset = model_asset_reader;
                bridge.scene.asset_user = &assets;
                bridge.scene.draft = adaptive_view && moving;       // cheaper shading for a picture that is replaced at once
                ok = fgl.render(bridge.scene);
                // What runs, for Settings > Graphics.
                snprintf(fgl_backend, sizeof(fgl_backend), "%s", fgl.backend());
                fgl_features = fgl.features();
            }
            if (ok) {
                rgba = fgl.pixels();
                view_triangles = fgl.stats().triangles;
                fgl_stats = fgl.stats();
                if (const char* w = fgl.take_warning()) log(LogLevel::Warning, "FarcadeGL: %s", w);
            } else {
                log(LogLevel::Error, "FarcadeGL: %s (back to the CPU preview)", fgl.error());
                fgl_failed = true;
                view_renderer = ViewRenderer::CpuPreview;
                use_fgl = false;
                div = 1;
                render_w_ = full_w;
                render_h_ = full_h;
                render_view(view3d, &v);
            }
        } else {
            render_view(view3d, &v);
        }
        view_ms = (float)((clock() - t0) * 1000.0);
        view_renders++;
        view_div_shown = div;
        banding = false;
        if (use_fgl && rgba) quality.report((long)rw * rh, view_ms, adaptive_view && moving);
        else if (!use_fgl) quality_cpu.report((long)view3d.width() * view3d.height(), view_ms, false);
        // What the window shows: the image itself, or the small one stretched.
        SwTexture& t = shown_tex;
        const uint32_t* src = use_fgl ? rgba : (const uint32_t*)view3d.texture()->pixels;
        int sw = use_fgl ? rw : view3d.width(), sh = use_fgl ? rh : view3d.height();
        if (div > 1 && src) {
            const uint32_t* up = upscaler.run(src, sw, sh, full_w, full_h);
            t.format = SwTexture::XRGB8;
            t.pixels = (const uint8_t*)up;
            t.width = full_w;
            t.height = full_h;
            t.pitch = full_w;
        } else {
            t.format = SwTexture::RGBA8;
            t.pixels = (const uint8_t*)src;
            t.width = sw;
            t.height = sh;
            t.pitch = sw;
        }
        t.opaque = true;
        t.linear = false;
        t.version++;
        fgl_tex.pixels = use_fgl ? (const uint8_t*)rgba : nullptr;     // "there is an image" marker
    }
    if (shown_tex.pixels) dl->AddImage((ImTextureID)SwRenderer::texture_id(&shown_tex), v.pos, ImVec2(v.pos.x + shown_tex.width, v.pos.y + shown_tex.height));
    // Particles, billboards, text and the gizmos of the effects modules.
    fx()->draw_overlay(*this, dl, v.view, v.proj, v.pos.x, v.pos.y, v.size.x, v.size.y, eye);
    // Icons for objects without a mesh (cameras, lights, sky ...), where
    // they are: they are picked by clicking near them.
    {
        float fs = ImGui::GetFontSize();
        for (int i = 0; i < scene.count(); i++) {
            Object& o = scene.at(i);
            if (o.kind == ObjectKind::Mesh || !scene.visible_in_tree(o) || looking_through_camera) continue;
            Mat4 m = scene.world_matrix(o);
            Vec4 c = v.viewproj * Vec4{ m.m[12], m.m[13], m.m[14], 1.0f };
            if (c.w <= 0.05f) continue;
            float sx = v.pos.x + (c.x / c.w * 0.5f + 0.5f) * v.size.x;
            float sy = v.pos.y + (0.5f - c.y / c.w * 0.5f) * v.size.y;
            bool sel = scene.is_selected(o.id);
            float r = fs * 0.85f;
            dl->AddCircleFilled(ImVec2(sx, sy), r, sel ? pal.accent : (pal.overlay & 0xE0FFFFFFu), 20);
            dl->AddCircle(ImVec2(sx, sy), r, pal.border, 20);
            draw_icon(dl, object_icon(o), ImVec2(sx - fs * 0.5f, sy - fs * 0.5f), fs, sel ? IM_COL32(255, 255, 255, 255) : pal.text);
            char key[32];
            snprintf(key, sizeof(key), "vicon:%u", o.id);
            track_rect(key, sx - r, sy - r, r * 2, r * 2);
        }
    }

    // Gizmo on the active object. It comes before the input area: ImGuizmo
    // only starts a drag when no other item is hovered, so the input area is
    // left out while the mouse is over the gizmo or dragging it.
    bool gizmo_active = false;
    ImGuiIO& gio = ImGui::GetIO();
    Object* act = scene.get(scene.active());
    // The collider's shape and its handles come first: a handle under the
    // mouse wins over the gizmo.
    if (!looking_through_camera && collider_overlay(v.view, v.proj, v.pos.x, v.pos.y, v.size.x, v.size.y)) gizmo_active = true;
    if (act && tool != Tool::Select && !gizmo_active) {
        ImGuizmo::SetDrawlist(dl);
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetRect(v.pos.x, v.pos.y, v.size.x, v.size.y);
        ImGuizmo::SetGizmoSizeClipSpace(0.17f);
        ImGuizmo::Style& gs = ImGuizmo::GetStyle();
        gs.TranslationLineThickness = 3.0f;
        gs.TranslationLineArrowSize = 7.0f;
        gs.RotationLineThickness = 2.5f;
        gs.RotationOuterLineThickness = 2.5f;
        gs.ScaleLineThickness = 3.0f;
        gs.ScaleLineCircleSize = 6.5f;
        gs.CenterCircleSize = 5.0f;
        gs.Colors[ImGuizmo::DIRECTION_X] = ImGui::ColorConvertU32ToFloat4(pal.axis[0]);
        gs.Colors[ImGuizmo::DIRECTION_Y] = ImGui::ColorConvertU32ToFloat4(pal.axis[1]);
        gs.Colors[ImGuizmo::DIRECTION_Z] = ImGui::ColorConvertU32ToFloat4(pal.axis[2]);
        gs.Colors[ImGuizmo::PLANE_X] = ImGui::ColorConvertU32ToFloat4(pal.axis[0] & 0x80FFFFFF);
        gs.Colors[ImGuizmo::PLANE_Y] = ImGui::ColorConvertU32ToFloat4(pal.axis[1] & 0x80FFFFFF);
        gs.Colors[ImGuizmo::PLANE_Z] = ImGui::ColorConvertU32ToFloat4(pal.axis[2] & 0x80FFFFFF);
        gs.Colors[ImGuizmo::SELECTION] = ImVec4(1.0f, 0.78f, 0.25f, 0.9f);
        ImGuizmo::OPERATION op = tool == Tool::Move ? ImGuizmo::TRANSLATE : (tool == Tool::Rotate ? ImGuizmo::ROTATE : ImGuizmo::SCALE);
        ImGuizmo::MODE mode = (gizmo_local || tool == Tool::Scale) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
        bool snap = settings.snap != gio.KeyCtrl;
        float step = tool == Tool::Move ? settings.snap_move : (tool == Tool::Rotate ? settings.snap_rotate : settings.snap_scale);
        float snap3[3] = { step, step, step };
        // The gizmo works on world matrices; the result goes back into each
        // object's transform relative to its parent.
        Mat4 old_world = scene.world_matrix(*act);
        Mat4 m = old_world;
        Vec3 old_scale = act->scale;
        // The other selected objects that are not under a selected parent
        // (those move with their parent), with their world matrices now.
        ObjectId others[256];
        Mat4 others_world[256];
        int n_others = 0;
        for (int s = 0; s < scene.selected_count() && n_others < 256; s++) {
            ObjectId id = scene.selected(s);
            Object* o = scene.get(id);
            if (!o || o == act || scene.has_selected_ancestor(id)) continue;
            others[n_others] = id;
            others_world[n_others++] = scene.world_matrix(*o);
        }
        if (ImGuizmo::Manipulate(v.view.m, v.proj.m, op, mode, m.m, nullptr, snap ? snap3 : nullptr)) {
            if (tool == Tool::Move && snap && mode == ImGuizmo::WORLD) {
                // Keep world positions on the snap grid, not just the steps.
                auto q = [&](float x) { float k = x / step; long n = (long)(k + (k >= 0 ? 0.5f : -0.5f)); return n * step; };
                m.m[12] = q(m.m[12]); m.m[13] = q(m.m[13]); m.m[14] = q(m.m[14]);
            }
            Mat4 inv_old, delta;
            if (invert(old_world, &inv_old)) delta = m * inv_old;      // the change, in world space
            // The rest of the selection follows: moved and rotated about the
            // active object; scaled along their own axes.
            Mat4 local_new, inv_parent;
            bool ok = invert(scene.parent_world_matrix(*act), &inv_parent);
            Vec3 tpos; Quat trot; Vec3 new_scale = old_scale;
            if (ok) { local_new = inv_parent * m; decompose(local_new, &tpos, &trot, &new_scale); }
            Vec3 ratio(old_scale.x != 0 ? new_scale.x / old_scale.x : 1, old_scale.y != 0 ? new_scale.y / old_scale.y : 1,
                       old_scale.z != 0 ? new_scale.z / old_scale.z : 1);
            for (int k = 0; k < n_others; k++) {
                Object* o = scene.get(others[k]);
                if (!o) continue;
                if (tool == Tool::Scale) { o->scale = mul(o->scale, ratio); continue; }
                Mat4 ip;
                if (invert(scene.parent_world_matrix(*o), &ip)) o->set_matrix(ip * (delta * others_world[k]));
            }
            // The active object last: its parent may have just moved with the selection.
            if (invert(scene.parent_world_matrix(*act), &inv_parent)) act->set_matrix(inv_parent * m);
        }
        gizmo_active = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetCursorScreenPos(v.pos);
    if (gizmo_active) ImGui::Dummy(v.size);
    else ImGui::InvisibleButton("##view", v.size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                                      ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = !gizmo_active && ImGui::IsItemHovered();

    // Files dropped on the viewport: onto an object, or a model onto the ground.
    if (!gizmo_active && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("FE_FILE")) {
            const char* path = (const char*)p->Data;
            Ray r = mouse_ray(v, io.MousePos);
            ObjectId hit = scene.pick(r);
            if (hit != kNoObject) apply_file_to_object(path, hit);
            else if (path_is_model(path)) {
                Vec3 at;
                if (!ray_ground(r, &at)) at = camera.target;
                ObjectId id = create_object(path_filename(path), MeshKind::Model, at + Vec3(0, 0.5f, 0));
                apply_file_to_object(path, id);
            } else {
                log(LogLevel::Warning, "Drop %s on an object", path_filename(path));
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Camera: right-drag orbits, middle-drag (or shift + right) pans, wheel zooms.
    if (hovered && !gizmo_active) {
        if (io.MouseWheel != 0) camera.distance = fclamp(camera.distance * (io.MouseWheel > 0 ? 0.85f : 1.18f), 0.5f, 400.0f);
    }
    bool camera_drag = false;
    if (!gizmo_active && ImGui::IsItemActive()) {
        ImVec2 d = io.MouseDelta;
        camera_drag = ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        bool pan = ImGui::IsMouseDown(ImGuiMouseButton_Middle) || (ImGui::IsMouseDown(ImGuiMouseButton_Right) && io.KeyShift);
        if (pan) {
            float k = camera.distance * 0.0016f;
            Mat4 inv;
            invert(v.view, &inv);
            Vec3 right = normalize(transform_dir(inv, Vec3(1, 0, 0)));
            Vec3 up = normalize(transform_dir(inv, Vec3(0, 1, 0)));
            camera.target -= right * (d.x * k);
            camera.target += up * (d.y * k);
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            camera.yaw -= d.x * 0.3f;
            camera.pitch = fclamp(camera.pitch + d.y * 0.3f, -89.0f, 89.0f);
        }
    }

    // Click (without dragging) selects; shift-click adds or removes.
    if (hovered && !gizmo_active && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0).x * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0).x +
                ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0).y * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0).y < 16.0f) {
        ObjectId hit = scene.pick(mouse_ray(v, io.MousePos));
        if (io.KeyShift) { if (hit != kNoObject) scene.toggle(hit); }
        else scene.select_only(hit);
    }

    // Play mode: a frame around the view.
    if (playing()) {
        dl->AddRect(ImVec2(v.pos.x + 1, v.pos.y + 1), ImVec2(end.x - 1, end.y - 1), pal.play, 0, 0, 2.0f);
    }
    if (scene.count() == 0) {
        const char* hint = "Create > Cube to add an object, or drop a .mof file here";
        ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(v.pos.x + (v.size.x - ts.x) * 0.5f, v.pos.y + v.size.y * 0.5f), pal.text_dim, hint);
    }
    dl->PopClipRect();

    // While the camera or an object is being dragged, the next frames render
    // at full speed (no supersampling or shadows); the full image comes back
    // when the drag ends (the setting is part of the hash).
    view_interacting_ = (!gizmo_active && camera_drag) || ImGuizmo::IsUsing();

    // Floating toolbars and the orientation gizmo, on top of the view.
    viewport_overlays(v.pos.x, v.pos.y, v.size.x, v.size.y);
    ImGui::End();
}

// ---------------------------------------------------------------- overlays

namespace {

// A floating rounded panel at (x, y) of size (w, h), as a child window so
// that clicks on it never reach the view below.
bool begin_overlay(const char* id, float x, float y, float w, float h) {
    const Palette& pal = ui_palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x + 1, y + 2), ImVec2(x + w + 1, y + h + 3), pal.shadow, 8.0f);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(pal.overlay));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(pal.border));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 7.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
    bool open = ImGui::BeginChild(id, ImVec2(w, h), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
    return open;
}

void overlay_separator(float h) {
    ImGui::SameLine(0, 4);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 4), ImVec2(p.x, p.y + h - 4), ui_palette().border, 1.0f);
    ImGui::Dummy(ImVec2(1, h));
    ImGui::SameLine(0, 4);
}

}  // namespace

void Editor::viewport_overlays(float x, float y, float w, float h) {
    float bs = ImGui::GetFrameHeight() + 2;
    float pad = 4;
    float margin = 10;

    // Tools: select, move, rotate, scale | world/local | snap | grid.
    float snap_w = 58 * settings.ui_scale;
    float fs = ImGui::GetFontSize();
    float space_w = ImGui::CalcTextSize("World").x + ImGui::CalcTextSize("Local").x + 2 * (fs + 18);
    float tools_w = pad * 2 + bs * 4 + 2 * 3 + 9 + space_w + 9 + bs + 2 + snap_w + 9 + bs;
    if (w > tools_w + 140 && begin_overlay("##tools", x + margin, y + margin, tools_w, bs + pad * 2)) {
        struct ToolDef { Tool tool; Icon icon; const char* tip; const char* key; };
        static const ToolDef tools[] = {
            { Tool::Select, Icon::Pointer, "Select (Q)", "tool:select" }, { Tool::Move, Icon::Move, "Move (W)", "tool:move" },
            { Tool::Rotate, Icon::Rotate, "Rotate (E)", "tool:rotate" }, { Tool::Scale, Icon::Scale, "Scale (R)", "tool:scale" },
        };
        for (int i = 0; i < 4; i++) {
            if (i) ImGui::SameLine(0, 2);
            if (icon_button(tools[i].icon, tools[i].tip, tool == tools[i].tool, bs)) tool = tools[i].tool;
            track(tools[i].key);
        }
        overlay_separator(bs);
        {
            // World | Local: which axes the gizmo uses (X toggles). Scale is
            // always along the object's own axes.
            bool scale = tool == Tool::Scale;
            bool local = gizmo_local || scale;
            ImGui::BeginDisabled(scale);
            for (int k = 0; k < 2; k++) {
                if (k) ImGui::SameLine(0, 0);
                const char* label = k ? "Local" : "World";
                bool on = (k == 1) == local;
                ImVec2 bp = ImGui::GetCursorScreenPos();
                float fs = ImGui::GetFontSize();
                float bw = ImGui::CalcTextSize(label).x + fs + 18;
                if (ImGui::InvisibleButton(k ? "##local" : "##world", ImVec2(bw, bs))) gizmo_local = k == 1;
                bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
                if (hov) ImGui::SetTooltip(scale ? "Scale always uses the object's own axes" :
                                           (k ? "Move and rotate along the object's own axes (X)" : "Move and rotate along the world axes (X)"));
                track(k ? "tool:local" : "tool:world");
                ImDrawList* odl = ImGui::GetWindowDrawList();
                if (on) odl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + bs), pal.accent_soft, 4.0f);
                else if (hov && !scale) odl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + bs), pal.hover, 4.0f);
                ImU32 col = on ? pal.accent_text : (scale ? pal.text_faint : pal.text_dim);
                draw_icon(odl, k ? Icon::Local : Icon::World, ImVec2(bp.x + 7, bp.y + (bs - fs) * 0.5f), fs, col);
                odl->AddText(ImVec2(bp.x + fs + 11, bp.y + (bs - fs) * 0.5f), on ? pal.text : col, label);
            }
            ImGui::EndDisabled();
        }
        overlay_separator(bs);
        if (icon_button(Icon::Magnet, "Snap (hold Ctrl to invert)", settings.snap, bs)) settings.snap = !settings.snap;
        track("tool:snap");
        ImGui::SameLine(0, 2);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (bs - ImGui::GetFrameHeight()) * 0.5f);
        ImGui::SetNextItemWidth(snap_w);
        float* snapv = tool == Tool::Rotate ? &settings.snap_rotate : (tool == Tool::Scale ? &settings.snap_scale : &settings.snap_move);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(pal.frame));
        ImGui::DragFloat("##snapv", snapv, tool == Tool::Rotate ? 1.0f : 0.05f, 0.01f, 360.0f, tool == Tool::Rotate ? "%.0f\xC2\xB0" : "%.2f");
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Snap step for the current tool");
        overlay_separator(bs);
        if (icon_button(Icon::Grid, "Grid (G)", settings.show_grid, bs)) settings.show_grid = !settings.show_grid;
        track("tool:grid");
        ImGui::EndChild();
    }

    // View menu: fixed views of the scene.
    {
        const char* label = "Perspective";
        float lw = ImGui::CalcTextSize(label).x + ImGui::GetFontSize() + 22;
        float vx = x + margin + tools_w + 8;
        if (w > tools_w + lw + 140 && begin_overlay("##viewmenu", vx, y + margin, lw, bs + pad * 2)) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##view", ImVec2(lw - pad * 2, bs))) ImGui::OpenPopup("##viewpopup");
            track("view:menu");
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (ImGui::IsItemHovered()) dl->AddRectFilled(p, ImVec2(p.x + lw - pad * 2, p.y + bs), pal.hover, 4.0f);
            float fs = ImGui::GetFontSize();
            dl->AddText(ImVec2(p.x + 7, p.y + (bs - fs) * 0.5f), pal.text, label);
            draw_icon(dl, Icon::ChevronDown, ImVec2(p.x + lw - pad * 2 - fs - 4, p.y + (bs - fs) * 0.5f), fs * 0.9f, pal.text_faint);
            if (ImGui::BeginPopup("##viewpopup")) {
                struct V { const char* n; float yaw, pitch; } views[] = {
                    { "Top", camera.yaw, 89.9f }, { "Bottom", camera.yaw, -89.9f }, { "Front", 0, 0 },
                    { "Back", 180, 0 }, { "Right", 90, 0 }, { "Left", -90, 0 },
                };
                for (const V& vw : views) {
                    if (menu_item(vw.n)) set_view(vw.yaw, vw.pitch);
                }
                ImGui::Separator();
                if (menu_item("Frame selection", "F", false, scene.selected_count() > 0)) frame_selection();
                if (menu_item("Reset view")) camera = ViewCamera();
                ImGui::EndPopup();
            }
            ImGui::EndChild();
        }
    }

    // Orientation gizmo in the top-right corner.
    float gsz = 84 * settings.ui_scale;
    if (w > gsz * 3 && h > gsz * 2) orientation_gizmo(x + w - gsz - margin * 0.5f, y + margin * 0.5f, gsz);
}

void Editor::orientation_gizmo(float x, float y, float size) {
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::BeginChild("##orient", ImVec2(size, size), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    if (!open) { ImGui::EndChild(); return; }
    ImVec2 c(x + size * 0.5f, y + size * 0.5f);
    float r = size * 0.5f - 11;         // axis length
    float br = 8.5f * settings.ui_scale; // bubble radius
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::InvisibleButton("##orient_btn", ImVec2(size, size), ImGuiButtonFlags_MouseButtonLeft);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    if (active) view_interacting_ = true;
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered || active) dl->AddCircleFilled(c, size * 0.5f - 1, pal.overlay, 32);

    // The six axis ends in view space, drawn back to front.
    Mat4 view = camera.view();
    struct End { Vec3 d; int axis; bool positive; ImVec2 p; float depth; };
    End ends[6];
    for (int i = 0; i < 6; i++) {
        Vec3 dir(0, 0, 0);
        (&dir.x)[i / 2] = (i & 1) ? -1.0f : 1.0f;
        Vec3 d = transform_dir(view, dir);
        ends[i] = { dir, i / 2, (i & 1) == 0, ImVec2(c.x + d.x * r, c.y - d.y * r), d.z };
    }
    for (int i = 0; i < 6; i++)
        for (int j = i + 1; j < 6; j++)
            if (ends[j].depth < ends[i].depth) { End t = ends[i]; ends[i] = ends[j]; ends[j] = t; }
    static const char* names[3] = { "X", "Y", "Z" };
    static const char* keys[6] = { "axis:+x", "axis:-x", "axis:+y", "axis:-y", "axis:+z", "axis:-z" };
    int hit = -1;
    for (int i = 5; i >= 0 && hovered; i--) {      // front-most first
        float dx = io.MousePos.x - ends[i].p.x, dy = io.MousePos.y - ends[i].p.y;
        if (dx * dx + dy * dy <= br * br) { hit = i; break; }
    }
    for (int i = 0; i < 6; i++) {
        const End& e = ends[i];
        ImU32 col = pal.axis[e.axis];
        if (e.positive) dl->AddLine(c, e.p, col, 2.0f);
        if (e.positive) {
            dl->AddCircleFilled(e.p, br, col, 20);
            ImVec2 ts = ImGui::CalcTextSize(names[e.axis]);
            dl->AddText(ImVec2(e.p.x - ts.x * 0.5f, e.p.y - ts.y * 0.5f), IM_COL32(16, 18, 21, 255), names[e.axis]);
        } else {
            dl->AddCircleFilled(e.p, br * 0.8f, (col & 0x00FFFFFF) | 0x50000000, 20);
            dl->AddCircle(e.p, br * 0.8f, col, 20, 1.5f);
        }
        if (i == hit) dl->AddCircle(e.p, br + 2, IM_COL32(255, 255, 255, 220), 20, 1.5f);
        int k = e.axis * 2 + (e.positive ? 0 : 1);
        track_rect(keys[k], e.p.x - br, e.p.y - br, br * 2, br * 2);
    }
    if (hit >= 0) {
        const End& e = ends[hit];
        static const char* tips[6] = { "View from +X (right)", "View from -X (left)", "View from +Y (top)", "View from -Y (bottom)",
                                       "View from +Z (front)", "View from -Z (back)" };
        ImGui::SetTooltip("%s", tips[e.axis * 2 + (e.positive ? 0 : 1)]);
    }
    // A click on an axis looks along it; dragging the gizmo orbits.
    if (ImGui::IsItemDeactivated() && hit >= 0) {
        ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0);
        if (dd.x * dd.x + dd.y * dd.y < 16) {
            const End& e = ends[hit];
            float s = e.positive ? 1.0f : -1.0f;
            if (e.axis == 0) set_view(90 * s, 0);
            else if (e.axis == 1) set_view(camera.yaw, 89.9f * s);
            else set_view(e.positive ? 0 : 180, 0);
        }
    }
    if (active && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
        camera.yaw -= io.MouseDelta.x * 0.4f;
        camera.pitch = fclamp(camera.pitch + io.MouseDelta.y * 0.4f, -89.9f, 89.9f);
    }
    ImGui::EndChild();
}

}  // namespace fe
