// Tests of the FarcadeGL renderer (engine/gfx) on the host FGPU device:
// the device itself, the projection convention, lighting, sky, post
// processing, mesh caching, resizing and limits.
#include "gfx/renderer.h"
#include "gfx/meshgen.h"
#include "gfx/host/farcadegl_host.h"
#include "../os/gfx_smoke/scene.h"
#include "mof_check.h"

#include <math.h>

using namespace fe;
using namespace fe::gfx;

namespace {

constexpr int W = 160, H = 120;

struct Px { int r, g, b; };

Px px(const Renderer& r, int x, int y) {
    uint32_t v = r.pixels()[y * r.width() + x];
    return { (int)(v & 0xFF), (int)((v >> 8) & 0xFF), (int)((v >> 16) & 0xFF) };
}

int lum(const Px& p) { return (p.r * 3 + p.g * 6 + p.b) / 10; }
bool near_px(const Px& p, int r, int g, int b, int tol) {
    return abs(p.r - r) <= tol && abs(p.g - g) <= tol && abs(p.b - b) <= tol;
}
// 8 bit sRGB of a linear value.
int srgb8(float c) {
    c = c < 0 ? 0 : (c > 1 ? 1 : c);
    float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
    return (int)(s * 255.0f + 0.5f);
}
float lin(float s) { return s <= 0.04045f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f); }

// No tonemap, no grading: pixel = sRGB(linear color).
PostProcess plain_post() {
    PostProcess p;
    p.exposure = 0;
    p.tonemap = Tonemap::None;
    p.contrast = 1;
    p.saturation = 1;
    p.tint = Vec3(1, 1, 1);
    p.vignette = 0;
    return p;
}

struct TestScene {
    Vec3 cube_p[kCubeVertices], cube_n[kCubeVertices];
    Vec3 plane_p[kPlaneVertices], plane_n[kPlaneVertices];
    DrawItem items[2];
    RenderLight lights[16];
    RenderScene scene;

    TestScene() {
        make_cube(cube_p, cube_n, 0.5f);
        make_plane(plane_p, plane_n, 20.0f, 0.0f);
        items[0].mesh = RenderMesh{ cube_p, cube_n, kCubeVertices, 11 };
        items[0].model = Mat4::identity();
        items[0].color = Vec3(0.8f, 0.8f, 0.8f);
        items[1].mesh = RenderMesh{ plane_p, plane_n, kPlaneVertices, 12 };
        items[1].model = Mat4::identity();
        items[1].color = Vec3(0.8f, 0.8f, 0.8f);
        scene.items = items;
        scene.item_count = 1;
        scene.lights = lights;
        scene.light_count = 0;
        scene.post = plain_post();
        scene.sky_light.intensity = 0;
        scene.sky.mode = SkyMode::Color;
        scene.sky.zenith = Vec3(0.1f, 0.1f, 0.1f);
        camera(Vec3(2.2f, 1.8f, 3.0f), Vec3(0, 0, 0));
    }

    void camera(Vec3 eye, Vec3 target, float fov_deg = 60.0f, float aspect = (float)W / H) {
        scene.camera.position = eye;
        scene.camera.view = Mat4::look_at(eye, target, Vec3(0, 1, 0));
        scene.camera.proj = Mat4::perspective(radians(fov_deg), aspect, 0.1f, 200.0f);
    }

    // The pixel a world point lands on, from the engine's matrices.
    bool project(Vec3 p, float* x, float* y, int w = W, int h = H) const {
        Vec4 c = scene.camera.proj * (scene.camera.view * Vec4{ p.x, p.y, p.z, 1 });
        if (c.w <= 0) return false;
        *x = (c.x / c.w * 0.5f + 0.5f) * (float)w;
        *y = (1.0f - (c.y / c.w * 0.5f + 0.5f)) * (float)h;
        return true;
    }
    Px at(const Renderer& r, Vec3 p) const {
        float x, y;
        if (!project(p, &x, &y, r.width(), r.height())) return { -999, -999, -999 };
        return px(r, (int)x, (int)y);
    }
};

RenderLight dir_light(Vec3 travel, Vec3 color = Vec3(1, 1, 1), float intensity = 1) {
    RenderLight l;
    l.type = LightType::Directional;
    l.direction = normalize(travel);
    l.color = color;
    l.intensity = intensity;
    return l;
}

bool render_ok(Renderer& r, const RenderScene& s) {
    bool ok = r.render(s);
    if (!ok) fprintf(stderr, "render failed: %s\n", r.error());
    return ok;
}

// ---------------------------------------------------------------- the device

void test_device() {
    fe_fgpu_host_stats_t before, after;
    fe_fgpu_host_stats(&before);

    syscall_gpu_info_t info;
    CHECK(fglGetDeviceInfo(&info) == 0);
    CHECK(fglHasShaders());
    CHECK(!fglHasHardware3D());
    CHECK(info.version == SYS_GPU_ABI_VERSION);

    FGLContextDesc d;
    memset(&d, 0, sizeof d);
    d.width = 32; d.height = 24; d.depth = 1;
    FGLContext* gl = fglCreateContext(&d);
    CHECK(gl != nullptr);
    if (!gl) return;
    // Clear color and read back (XRGB: 0xFFRRGGBB).
    fglClearColor(gl, 0.2f, 0.4f, 0.6f);
    fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
    CHECK(fglSubmit(gl) == 0);
    uint32_t buf[32 * 24];
    CHECK(fglReadPixels(gl, buf, 32 * 4) == 0);
    CHECK_EQ(buf[0] & 0xFFFFFF, 0x336699);
    CHECK_EQ(buf[32 * 24 - 1] & 0xFFFFFF, 0x336699);

    // A vertex-colored triangle with a built-in program (CCW, covers the center).
    FGLVertex tri[3] = {
        { -0.8f, -0.8f, 0, 0, 0, 1, 1, 0, 0, 1 },
        { 0.8f, -0.8f, 0, 0, 0, 1, 1, 0, 0, 1 },
        { 0.0f, 0.8f, 0, 0, 0, 1, 1, 0, 0, 1 },
    };
    FGLBuffer vb = fglCreateBuffer(gl, sizeof tri);
    CHECK(vb != 0);
    CHECK(fglBufferData(gl, vb, 0, tri, sizeof tri) == 0);
    fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
    fglEnable(gl, FGL_DEPTH_TEST);
    fglUseProgram(gl, FGL_PROGRAM_COLOR);
    fglDraw(gl, vb, 0, 3);
    CHECK(fglSubmit(gl) == 0);
    CHECK(gl->last_triangles == 1);
    CHECK(fglReadPixels(gl, buf, 32 * 4) == 0);
    CHECK_EQ(buf[12 * 32 + 16] & 0xFFFFFF, 0xFF0000);
    CHECK_EQ(buf[0] & 0xFFFFFF, 0x336699);

    // A shader program draws too: its uniform ends up in the pixel.
    const char* vs = "attribute vec3 position;\nvoid main() { gl_Position = vec4(position, 1.0); }\n";
    const char* fs = "uniform vec4 u_c;\nvoid main() { gl_FragColor = u_c; }\n";
    FGLShader* sh = fglCreateShader(gl, vs, fs);
    CHECK(sh != nullptr);
    if (sh) {
        fglUniform4f(sh, fglUniformLocation(sh, "u_c"), 0.0f, 1.0f, 0.0f, 1.0f);
        fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
        fglUseShader(gl, sh);
        fglDraw(gl, vb, 0, 3);
        CHECK(fglSubmit(gl) == 0);
        CHECK(fglReadPixels(gl, buf, 32 * 4) == 0);
        CHECK_EQ(buf[12 * 32 + 16] & 0xFFFFFF, 0x00FF00);
        fglDestroyShader(gl, sh);
    }
    // A shader that does not compile says why.
    CHECK(fglCreateShader(gl, vs, "void main() { gl_FragColor = nope; }\n") == nullptr);
    CHECK(fglShaderLog(gl)[0] != 0);

    // Presenting draws nothing and the frame stays readable.
    uint32_t presents = 0;
    fe_fgpu_host_stats(&after);
    presents = after.presents;
    CHECK(fglPresent(gl) == 0);
    fe_fgpu_host_stats(&after);
    CHECK_EQ(after.presents, presents + 1);
    CHECK(fglReadPixels(gl, buf, 32 * 4) == 0);

    // The kernel's rejections.
    syscall_gpu_request_t r;
    memset(&r, 0, sizeof r);
    r.op = SYS_GPU_CREATE_BUFFER;
    r.bytes = FGPU_MAX_BUFFER_BYTES + 1;
    r.size = sizeof r; r.version = SYS_GPU_ABI_VERSION;
    CHECK(fe_fgpu_host_syscall(&r) == (uint64_t)-4);                // SYS_ERR_INVAL: over 16 MiB
    r.bytes = 0;
    CHECK(fe_fgpu_host_syscall(&r) == (uint64_t)-4);
    r.bytes = 1024;
    CHECK(fe_fgpu_host_syscall(&r) == SYS_SUCCESS);
    uint32_t stale = r.handle;
    r.op = SYS_GPU_DESTROY_BUFFER;
    r.handle = stale;
    CHECK(fe_fgpu_host_syscall(&r) == SYS_SUCCESS);
    CHECK(fe_fgpu_host_syscall(&r) == (uint64_t)-3);                // SYS_ERR_NOTFOUND: stale handle
    r.op = 999;
    CHECK(fe_fgpu_host_syscall(&r) == (uint64_t)-4);
    r.version = SYS_GPU_ABI_VERSION + 1;
    r.op = SYS_GPU_CREATE_BUFFER;
    r.bytes = 16;
    CHECK(fe_fgpu_host_syscall(&r) == (uint64_t)-4);                // unknown ABI version
    r.version = SYS_GPU_ABI_VERSION;
    // Submits: 3 bytes (not a multiple of 4) and 1 MiB + 4 are refused, a bad command too.
    uint32_t words[4] = { 0, 0, 0, 0 };
    r.op = SYS_GPU_SUBMIT; r.data = words; r.bytes = 3;
    CHECK(fe_fgpu_host_syscall(&r) == (uint64_t)-4);
    r.bytes = FGPU_MAX_COMMAND_BYTES + 4;
    CHECK(fe_fgpu_host_syscall(&r) == (uint64_t)-4);
    words[0] = FGPU_CMD(77, 1);
    r.bytes = 4;
    CHECK(fe_fgpu_host_syscall(&r) == (uint64_t)-4);
    // A draw into a destroyed buffer is NOTFOUND.
    FGLBuffer gone = fglCreateBuffer(gl, 120);
    fglDestroyBuffer(gl, gone);
    fglDraw(gl, gone, 0, 3);
    CHECK(fglSubmit(gl) == -1);
    CHECK_EQ(gl->error, (int)(uint64_t)-3);
    // Quota: 1024 surfaces per program.
    FGLSurface s[1030];
    int made = 0;
    for (; made < 1030; made++) {
        s[made] = fgl__surface(2, 2, FGPU_FORMAT_XRGB8888, 0);
        if (!s[made]) break;
    }
    CHECK(made >= 1000 && made < 1030);
    for (int i = 0; i < made; i++) fgl__destroy(SYS_GPU_DESTROY_SURFACE, s[i]);
    // Surface limits.
    CHECK(fgl__surface(FGPU_MAX_SURFACE_SIZE + 1, 4, FGPU_FORMAT_XRGB8888, 0) == 0);
    CHECK(fgl__surface(0, 4, FGPU_FORMAT_XRGB8888, 0) == 0);
    CHECK(fgl__surface(4, 4, 77, 0) == 0);

    fglDestroyBuffer(gl, vb);
    fglDestroyContext(gl);
    fe_fgpu_host_stats(&after);
    CHECK_EQ(after.surfaces, before.surfaces);
    CHECK_EQ(after.buffers, before.buffers);
    CHECK_EQ(after.programs, before.programs);
}

// ----------------------------------------------------------- the renderer

void test_projection() {
    // A small unlit cube at a known point: its pixels must be centered where the
    // engine's own matrices put the point (x right, y up, camera down -z).
    const Vec3 pts[3] = { Vec3(1.0f, 0.5f, -5.0f), Vec3(-1.5f, -0.7f, -4.0f), Vec3(0.0f, 0.0f, -8.0f) };
    for (const Vec3& p : pts) {
        TestScene t;
        t.camera(Vec3(0, 0, 0), Vec3(0, 0, -1));
        t.items[0].model = Mat4::translation(p) * Mat4::scale(Vec3(0.5f, 0.5f, 0.5f));
        t.items[0].unlit = true;
        t.items[0].color = Vec3(1, 0, 1);
        Renderer r;
        CHECK(r.init(W, H));
        if (!render_ok(r, t.scene)) return;
        double sx = 0, sy = 0, n = 0;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            Px c = px(r, x, y);
            if (c.r > 200 && c.g < 60 && c.b > 200) { sx += x + 0.5; sy += y + 0.5; n++; }
        }
        CHECK(n > 20);
        float ex = 0, ey = 0;
        CHECK(t.project(p, &ex, &ey));
        CHECK(fabs(sx / n - ex) < 1.0);
        CHECK(fabs(sy / n - ey) < 1.0);
    }
    // And a spot check by hand: (1, 0, -5) with 60 degrees vertical, aspect 4:3.
    float ex = (1.0f / 5.0f / (tanf(radians(30.0f)) * ((float)W / H)) * 0.5f + 0.5f) * W;
    TestScene t;
    t.camera(Vec3(0, 0, 0), Vec3(0, 0, -1));
    float px_, py_;
    CHECK(t.project(Vec3(1, 0, -5), &px_, &py_));
    CHECK(fabs(px_ - ex) < 0.05);
    CHECK(fabs(py_ - H * 0.5) < 0.05);
}

void test_lighting() {
    Renderer r;
    CHECK(r.init(W, H));
    TestScene t;
    const Vec3 px_face(0.5f, 0, 0), py_face(0, 0.5f, 0), pz_face(0, 0, 0.5f);

    // Sky color in an empty corner (flat sky, linear 0.2 0.4 0.6).
    t.scene.sky.zenith = Vec3(0.2f, 0.4f, 0.6f);
    t.scene.lights = t.lights; t.scene.light_count = 0;
    if (!render_ok(r, t.scene)) return;
    CHECK(near_px(px(r, 2, 2), srgb8(0.2f), srgb8(0.4f), srgb8(0.6f), 2));
    CHECK(near_px(px(r, W - 3, H - 3), srgb8(0.2f), srgb8(0.4f), srgb8(0.6f), 2));
    t.scene.sky.zenith = Vec3(0.1f, 0.1f, 0.1f);

    // A directional light along -x: the +x face is lit, the others get no light.
    t.lights[0] = dir_light(Vec3(-1, 0, 0));
    t.scene.light_count = 1;
    if (!render_ok(r, t.scene)) return;
    Px lit = t.at(r, px_face), top = t.at(r, py_face), front = t.at(r, pz_face);
    CHECK(near_px(lit, srgb8(lin(0.8f)), srgb8(lin(0.8f)), srgb8(lin(0.8f)), 12));
    CHECK(lum(top) < 6);
    CHECK(lum(front) < 6);
    CHECK(lum(lit) > 150);
    // The ground plane is not in this scene (item_count 1): the corner is the sky.
    CHECK(near_px(px(r, 2, 2), srgb8(0.1f), srgb8(0.1f), srgb8(0.1f), 2));

    // The light's color tints the result.
    t.lights[0] = dir_light(Vec3(-1, 0, 0), Vec3(1, 0, 0));
    if (!render_ok(r, t.scene)) return;
    lit = t.at(r, px_face);
    CHECK(lit.r > 150 && lit.g < 12 && lit.b < 12);
    t.lights[0] = dir_light(Vec3(-1, 0, 0), Vec3(0, 0, 1));
    if (!render_ok(r, t.scene)) return;
    lit = t.at(r, px_face);
    CHECK(lit.b > 150 && lit.r < 12 && lit.g < 12);

    // Intensity scales it.
    t.lights[0] = dir_light(Vec3(-1, 0, 0), Vec3(1, 1, 1), 0.25f);
    if (!render_ok(r, t.scene)) return;
    CHECK(near_px(t.at(r, px_face), srgb8(lin(0.8f) * 0.25f), srgb8(lin(0.8f) * 0.25f), srgb8(lin(0.8f) * 0.25f), 12));

    // Albedo: sRGB to linear (a 0.5 gray face lit by a full light is linear 0.214).
    t.lights[0] = dir_light(Vec3(-1, 0, 0));
    t.items[0].color = Vec3(0.5f, 0.5f, 0.5f);
    if (!render_ok(r, t.scene)) return;
    CHECK(near_px(t.at(r, px_face), srgb8(lin(0.5f)), srgb8(lin(0.5f)), srgb8(lin(0.5f)), 12));
    t.items[0].color = Vec3(0.8f, 0.8f, 0.8f);

    // Hemisphere ambient: sky above (white), ground below (black), no lights.
    t.scene.light_count = 0;
    t.scene.sky_light.sky_color = Vec3(1, 1, 1);
    t.scene.sky_light.ground_color = Vec3(0, 0, 0);
    t.scene.sky_light.intensity = 1.0f;
    if (!render_ok(r, t.scene)) return;
    top = t.at(r, py_face);
    front = t.at(r, pz_face);
    CHECK(near_px(top, srgb8(lin(0.8f)), srgb8(lin(0.8f)), srgb8(lin(0.8f)), 5));
    CHECK(near_px(front, srgb8(lin(0.8f) * 0.5f), srgb8(lin(0.8f) * 0.5f), srgb8(lin(0.8f) * 0.5f), 5));
    t.scene.sky_light.intensity = 0.5f;
    if (!render_ok(r, t.scene)) return;
    CHECK(near_px(t.at(r, py_face), srgb8(lin(0.8f) * 0.5f), srgb8(lin(0.8f) * 0.5f), srgb8(lin(0.8f) * 0.5f), 5));
    t.scene.sky_light.intensity = 0;

    // A mirrored object (negative scale) is still drawn, lit on the same side.
    t.lights[0] = dir_light(Vec3(-1, 0, 0));
    t.scene.light_count = 1;
    t.items[0].model = Mat4::scale(Vec3(-1, 1, 1));
    if (!render_ok(r, t.scene)) return;
    CHECK(lum(t.at(r, Vec3(-0.5f, 0, 0))) < 6);
    t.camera(Vec3(-2.2f, 1.8f, 3.0f), Vec3(0, 0, 0));
    t.lights[0] = dir_light(Vec3(1, 0, 0));         // travels +x: lights the mirrored cube's face at -x
    if (!render_ok(r, t.scene)) return;
    CHECK(lum(t.at(r, Vec3(-0.5f, 0, 0))) > 150);
    t.camera(Vec3(2.2f, 1.8f, 3.0f), Vec3(0, 0, 0));
    t.items[0].model = Mat4::identity();

    // Roughness: a smooth surface has a highlight a rough one lacks.
    t.items[0].model = Mat4::identity();
    t.camera(Vec3(0, 0, 3), Vec3(0, 0, 0));
    t.lights[0] = dir_light(Vec3(0, 0, -1));
    t.scene.light_count = 1;
    t.items[0].roughness = 0.9f;
    if (!render_ok(r, t.scene)) return;
    Px rough = t.at(r, Vec3(0, 0, 0.5f));
    t.items[0].roughness = 0.08f;
    if (!render_ok(r, t.scene)) return;
    Px smooth = t.at(r, Vec3(0, 0, 0.5f));
    CHECK(smooth.r > rough.r);
    t.items[0].roughness = 0.6f;

    // Unlit draws its color (the light does not matter).
    t.camera(Vec3(2.2f, 1.8f, 3.0f), Vec3(0, 0, 0));
    t.items[0].unlit = true;
    t.items[0].color = Vec3(0.8f, 0.2f, 0.2f);
    t.scene.light_count = 0;
    if (!render_ok(r, t.scene)) return;
    Px u1 = t.at(r, py_face), u2 = t.at(r, px_face), u3 = t.at(r, pz_face);
    CHECK(near_px(u1, 204, 51, 51, 2));
    CHECK(near_px(u2, 204, 51, 51, 2));
    CHECK(near_px(u3, 204, 51, 51, 2));
    t.items[0].unlit = false;
    t.items[0].color = Vec3(0.8f, 0.8f, 0.8f);
}

void test_light_types() {
    Renderer r;
    CHECK(r.init(W, H));
    TestScene t;
    t.scene.items = t.items + 1;                // the ground only
    t.scene.item_count = 1;
    t.camera(Vec3(0, 6, 7), Vec3(0, 0, 0));
    float x, y;

    // A spot straight down: lit inside the cone, dark outside.
    RenderLight sp;
    sp.type = LightType::Spot;
    sp.position = Vec3(0, 3, 0);
    sp.direction = Vec3(0, -1, 0);
    sp.spot_angle = 30.0f;
    sp.spot_blend = 0.2f;
    sp.range = 12.0f;
    t.lights[0] = sp;
    t.scene.light_count = 1;
    CHECK(t.project(Vec3(4, 0, 0), &x, &y) && x > 0 && x < W && y > 0 && y < H);
    if (!render_ok(r, t.scene)) return;
    Px inside = t.at(r, Vec3(0, 0, 0)), outside = t.at(r, Vec3(4, 0, 0)), edge = t.at(r, Vec3(0.7f, 0, 0));
    CHECK(lum(inside) > 120);
    CHECK(lum(outside) < 6);
    CHECK(lum(edge) < lum(inside));                         // the soft edge fades
    // A narrower cone leaves the same point dark.
    t.lights[0].spot_angle = 8.0f;
    if (!render_ok(r, t.scene)) return;
    CHECK(lum(t.at(r, Vec3(0.7f, 0, 0))) < 6);
    CHECK(lum(t.at(r, Vec3(0, 0, 0))) > 100);

    // A point light: lit in range, nothing beyond it.
    RenderLight pt;
    pt.type = LightType::Point;
    pt.position = Vec3(0, 1, 0);
    pt.range = 2.0f;
    t.lights[0] = pt;
    if (!render_ok(r, t.scene)) return;
    CHECK(lum(t.at(r, Vec3(0, 0, 0))) > 100);
    CHECK(lum(t.at(r, Vec3(0.6f, 0, 0))) > 20);
    CHECK(lum(t.at(r, Vec3(3, 0, 0))) < 6);                 // distance 3.2 > range 2
    // Falloff with distance.
    CHECK(lum(t.at(r, Vec3(0, 0, 0))) > lum(t.at(r, Vec3(1.2f, 0, 0))));
    // Out of range altogether: the same as no light.
    t.lights[0].position = Vec3(0, 30, 0);
    if (!render_ok(r, t.scene)) return;
    CHECK(lum(t.at(r, Vec3(0, 0, 0))) < 6);
    CHECK(lum(t.at(r, Vec3(3, 0, 0))) < 6);

    // All three kinds at once, in any order.
    t.lights[0] = pt;
    t.lights[1] = sp;
    t.lights[1].spot_angle = 30.0f;
    t.lights[2] = dir_light(Vec3(1, -1, 0), Vec3(1, 0, 0), 0.5f);
    t.scene.light_count = 3;
    if (!render_ok(r, t.scene)) return;
    Px mixed = t.at(r, Vec3(0, 0, 0));
    CHECK(mixed.r > 100);
    // The order of the lights does not change the picture.
    RenderLight tmp = t.lights[0]; t.lights[0] = t.lights[2]; t.lights[2] = tmp;
    uint32_t first[W * H];
    // (compare with a re-render of the original order)
    if (!render_ok(r, t.scene)) return;
    memcpy(first, r.pixels(), sizeof first);
    tmp = t.lights[0]; t.lights[0] = t.lights[2]; t.lights[2] = tmp;
    if (!render_ok(r, t.scene)) return;
    int diff = 0;
    for (int i = 0; i < W * H; i++) {
        Px a = { (int)(first[i] & 0xFF), (int)((first[i] >> 8) & 0xFF), (int)((first[i] >> 16) & 0xFF) };
        uint32_t v = r.pixels()[i];
        Px b = { (int)(v & 0xFF), (int)((v >> 8) & 0xFF), (int)((v >> 16) & 0xFF) };
        if (abs(a.r - b.r) > 1 || abs(a.g - b.g) > 1 || abs(a.b - b.b) > 1) diff++;
    }
    CHECK_EQ(diff, 0);

    // More lights than the shader takes: the strongest are kept. Nine point
    // lights over the ground, eight bright and one weak: the weak one is dropped.
    t.camera(Vec3(0, 14, 0.01f), Vec3(0, 0, 0));
    for (int i = 0; i < 9; i++) {
        RenderLight l;
        l.type = LightType::Point;
        l.position = Vec3(-8.0f + 2.0f * (float)i, 0.5f, 0);
        l.range = 1.5f;
        l.intensity = i == 4 ? 0.5f : 40.0f;
        t.lights[i] = l;
    }
    t.scene.light_count = 9;
    if (!render_ok(r, t.scene)) return;
    int lit_spots = 0;
    for (int i = 0; i < 9; i++) {
        bool bright = lum(t.at(r, Vec3(-8.0f + 2.0f * (float)i, 0, 0))) > 60;
        if (bright) lit_spots++;
        if (i == 4) CHECK(!bright);
        else CHECK(bright);
    }
    CHECK_EQ(lit_spots, 8);
}

void test_post() {
    Renderer r;
    CHECK(r.init(W, H));
    TestScene t;
    const Vec3 face(0.5f, 0, 0);
    t.lights[0] = dir_light(Vec3(-1, 0, 0), Vec3(1, 1, 1), 3.0f);
    t.scene.light_count = 1;

    t.scene.post.tonemap = Tonemap::None;
    if (!render_ok(r, t.scene)) return;
    int none = lum(t.at(r, face));
    t.scene.post.tonemap = Tonemap::Reinhard;
    if (!render_ok(r, t.scene)) return;
    int rein = lum(t.at(r, face));
    t.scene.post.tonemap = Tonemap::ACES;
    if (!render_ok(r, t.scene)) return;
    int aces = lum(t.at(r, face));
    CHECK(none >= 250);                 // clipped
    CHECK(rein < none - 20);
    CHECK(aces < none - 3);
    CHECK(aces > rein + 10);
    CHECK(abs(rein - srgb8(lin(0.8f) * 3.0f / (1.0f + lin(0.8f) * 3.0f))) < 14);

    // Exposure in EV stops.
    t.lights[0].intensity = 0.5f;
    t.scene.post.tonemap = Tonemap::None;
    t.scene.post.exposure = -1.0f;
    if (!render_ok(r, t.scene)) return;
    int e_lo = lum(t.at(r, face));
    t.scene.post.exposure = 0.0f;
    if (!render_ok(r, t.scene)) return;
    int e_mid = lum(t.at(r, face));
    t.scene.post.exposure = 1.0f;
    if (!render_ok(r, t.scene)) return;
    int e_hi = lum(t.at(r, face));
    CHECK(e_lo + 20 < e_mid);
    CHECK(e_mid + 20 < e_hi);
    CHECK(abs(e_mid - srgb8(lin(0.8f) * 0.5f)) < 8);
    CHECK(abs(e_hi - srgb8(lin(0.8f) * 1.0f)) < 8);
    t.scene.post.exposure = 0.0f;

    // Saturation 0 is gray, contrast and tint change the color.
    t.scene.light_count = 0;
    t.scene.item_count = 0;                 // only the sky from here
    t.scene.sky.zenith = Vec3(0.2f, 0.4f, 0.6f);
    t.scene.post.saturation = 0.0f;
    if (!render_ok(r, t.scene)) return;
    Px g = px(r, 3, 3);
    CHECK(abs(g.r - g.g) <= 1 && abs(g.g - g.b) <= 1);
    CHECK(g.r > 100 && g.r < 200);
    t.scene.post.saturation = 1.0f;
    t.scene.post.tint = Vec3(1.0f, 0.5f, 0.25f);
    if (!render_ok(r, t.scene)) return;
    CHECK(near_px(px(r, 3, 3), srgb8(0.2f), srgb8(0.2f), srgb8(0.15f), 2));
    t.scene.post.tint = Vec3(1, 1, 1);
    t.scene.post.contrast = 2.0f;
    if (!render_ok(r, t.scene)) return;
    Px c = px(r, 3, 3);
    CHECK(near_px(c, srgb8(0.22f), srgb8(0.62f), 255, 2));   // stretched around linear 0.18
    t.scene.post.contrast = 1.0f;

    // Vignette: corners darker, the center untouched.
    t.scene.post.vignette = 0.8f;
    if (!render_ok(r, t.scene)) return;
    Px center = px(r, W / 2, H / 2), corner = px(r, 0, 0);
    CHECK(near_px(center, srgb8(0.2f), srgb8(0.4f), srgb8(0.6f), 2));
    CHECK(corner.b * 2 < center.b);
    t.scene.post.vignette = 0.0f;
}

void test_sky() {
    Renderer r;
    CHECK(r.init(W, H));
    TestScene t;
    t.scene.item_count = 0;
    t.camera(Vec3(0, 0, 0), Vec3(0, 0, -1));
    t.scene.sky.mode = SkyMode::Gradient;
    t.scene.sky.zenith = Vec3(0.0f, 0.2f, 0.9f);
    t.scene.sky.horizon = Vec3(0.9f, 0.9f, 0.6f);
    t.scene.sky.ground = Vec3(0.2f, 0.1f, 0.05f);
    t.scene.sky.sun_size = 0;
    if (!render_ok(r, t.scene)) return;
    Px top = px(r, W / 2, 0), mid = px(r, W / 2, H / 2), bottom = px(r, W / 2, H - 1);
    CHECK(top.b > top.r + 60);                                  // towards the zenith: blue
    CHECK(mid.r > mid.b + 20);                                  // horizon: yellowish
    CHECK(near_px(bottom, srgb8(0.2f), srgb8(0.1f), srgb8(0.05f), 3));     // below: the ground color
    CHECK(top.b > mid.b);

    // The sun disc towards the strongest directional light (and none with size 0).
    t.scene.sky.zenith = t.scene.sky.horizon = Vec3(0.02f, 0.03f, 0.06f);
    t.scene.sky.ground = Vec3(0.02f, 0.03f, 0.06f);
    t.lights[0] = dir_light(Vec3(0, -0.1f, -1.0f), Vec3(1, 1, 0.9f), 2.0f);
    t.lights[0].direction = normalize(Vec3(0.2f, -0.1f, 1.0f));           // the sun is at (-0.2, 0.1, -1): ahead, left
    t.lights[1] = dir_light(Vec3(0, -1, 0), Vec3(1, 1, 1), 0.2f);          // a weaker one: not the sun
    t.scene.light_count = 2;
    t.scene.sky.sun_size = 4.0f;
    Vec3 toward = normalize(Vec3(-0.2f, 0.1f, -1.0f));
    float sx, sy;
    CHECK(t.project(t.scene.camera.position + toward * 100.0f, &sx, &sy));
    if (!render_ok(r, t.scene)) return;
    Px sun = px(r, (int)sx, (int)sy), away = px(r, (int)sx + 30, (int)sy);
    CHECK(lum(sun) > 200);
    CHECK(lum(away) < 60);
    t.scene.sky.sun_size = 0;
    if (!render_ok(r, t.scene)) return;
    CHECK(lum(px(r, (int)sx, (int)sy)) < 60);
    // Flat color mode has no sun either, and an empty scene is just the sky.
    t.scene.sky.sun_size = 4.0f;
    t.scene.sky.mode = SkyMode::Color;
    if (!render_ok(r, t.scene)) return;
    CHECK(lum(px(r, (int)sx, (int)sy)) < 60);
    CHECK_EQ(r.stats().draws, 0);
}

void test_cache_and_resize() {
    Renderer r;
    CHECK(r.init(W, H));
    CHECK(r.ready());
    CHECK_EQ(r.width(), W);
    TestScene t;
    t.scene.item_count = 2;
    t.lights[0] = dir_light(Vec3(-1, -1, -1));
    t.scene.light_count = 1;
    fe_fgpu_host_stats_t s0, s1, s2;
    if (!render_ok(r, t.scene)) return;
    CHECK_EQ(r.stats().uploads, 2);
    CHECK_EQ(r.stats().draws, 2);
    CHECK_EQ(r.stats().triangles, 12 + 2);
    CHECK(r.stats().ms > 0);
    fe_fgpu_host_stats(&s0);
    CHECK_EQ(s0.buffers, 3u);                       // two meshes and the sky
    uint32_t a[W * H];
    memcpy(a, r.pixels(), sizeof a);
    if (!render_ok(r, t.scene)) return;
    CHECK_EQ(r.stats().uploads, 0);                 // the second frame uploads nothing
    fe_fgpu_host_stats(&s1);
    CHECK_EQ(s1.buffers, s0.buffers);
    CHECK(memcmp(a, r.pixels(), sizeof a) == 0);    // and draws the same picture

    // Key 0: uploaded every frame, freed after it.
    t.items[0].mesh.key = 0;
    if (!render_ok(r, t.scene)) return;
    CHECK_EQ(r.stats().uploads, 1);
    if (!render_ok(r, t.scene)) return;
    CHECK_EQ(r.stats().uploads, 1);
    fe_fgpu_host_stats(&s2);
    CHECK_EQ(s2.buffers, s0.buffers);               // the uncached mesh's buffer was freed
    CHECK(memcmp(a, r.pixels(), sizeof a) == 0);
    t.items[0].mesh.key = 11;                       // still cached from the first frame
    if (!render_ok(r, t.scene)) return;
    CHECK_EQ(r.stats().uploads, 0);
    // The same key with other vertices (a different count) uploads again.
    t.items[0].mesh.vertex_count = 6;
    if (!render_ok(r, t.scene)) return;
    CHECK_EQ(r.stats().uploads, 1);
    CHECK_EQ(r.stats().triangles, 2 + 2);
    t.items[0].mesh.vertex_count = kCubeVertices;
    if (!render_ok(r, t.scene)) return;

    // Missing normals are made from the faces: the same cube, the same light.
    RenderMesh nonorm = t.items[0].mesh;
    nonorm.normals = nullptr;
    nonorm.key = 13;
    t.items[0].mesh = nonorm;
    if (!render_ok(r, t.scene)) return;
    CHECK(memcmp(a, r.pixels(), sizeof a) == 0);
    t.items[0].mesh.normals = t.cube_n;
    t.items[0].mesh.key = 11;

    // Resize keeps the meshes.
    CHECK(r.resize(80, 60));
    CHECK_EQ(r.width(), 80);
    CHECK_EQ(r.height(), 60);
    t.camera(Vec3(2.2f, 1.8f, 3.0f), Vec3(0, 0, 0), 60.0f, 80.0f / 60.0f);
    if (!render_ok(r, t.scene)) return;
    CHECK_EQ(r.stats().uploads, 0);
    CHECK(lum(t.at(r, Vec3(0.5f, 0, 0))) > 10 || lum(t.at(r, Vec3(0, 0.5f, 0))) > 10);
    CHECK(near_px(px(r, 1, 1), srgb8(0.1f), srgb8(0.1f), srgb8(0.1f), 3));
    CHECK(r.resize(200, 150));
    t.camera(Vec3(2.2f, 1.8f, 3.0f), Vec3(0, 0, 0), 60.0f, 200.0f / 150.0f);
    if (!render_ok(r, t.scene)) return;
    CHECK_EQ(r.stats().uploads, 0);
    CHECK(r.resize(200, 150));                      // the same size: nothing to do
    CHECK(!r.resize(0, 10));
    CHECK(r.error()[0] != 0);
    CHECK(r.ready());

    // forget_meshes drops the cache.
    r.forget_meshes();
    if (!render_ok(r, t.scene)) return;
    CHECK_EQ(r.stats().uploads, 2);

    // Shutdown frees everything the renderer made.
    r.shutdown();
    CHECK(!r.ready());
}

void test_many_items_and_budget() {
    // Thousands of items: the command buffer stays under the 1 MiB limit (the
    // renderer submits in parts).
    constexpr int N = 3000;
    static DrawItem items[N];
    TestScene t;
    for (int i = 0; i < N; i++) {
        items[i] = t.items[0];
        items[i].mesh.key = 11;
        items[i].model = Mat4::translation(Vec3((float)(i % 55 - 27) * 0.5f, (float)(i / 55) * 0.4f, -20.0f)) *
                         Mat4::scale(Vec3(0.2f, 0.2f, 0.2f));
        items[i].unlit = i % 5 == 0;
    }
    t.scene.items = items;
    t.scene.item_count = N;
    t.lights[0] = dir_light(Vec3(-1, -1, -1));
    t.scene.light_count = 1;
    t.camera(Vec3(0, 6, 8), Vec3(0, 6, -20), 60.0f);
    Renderer r;
    CHECK(r.init(W, H));
    fe_fgpu_host_stats_t before, after;
    fe_fgpu_host_stats(&before);
    if (!render_ok(r, t.scene)) return;
    fe_fgpu_host_stats(&after);
    CHECK_EQ(r.stats().draws, N);
    CHECK(after.submits - before.submits >= 2);
    CHECK(after.max_command_bytes <= FGPU_MAX_COMMAND_BYTES);
    CHECK(lum(px(r, W / 2, H / 2)) > 0);
}

void test_big_mesh() {
    // More than one upload slice (8190 vertices) per buffer: 64 x 32 segments is 12288
    // vertices. With and without normals (made from the faces) the picture is the same
    // when the given normals are the face normals.
    constexpr int SEG = 64, RINGS = 32, N = SEG * RINGS * 6;
    static Vec3 pos[N], face_n[N];
    static Vec3 smooth_n[N];
    make_sphere(pos, smooth_n, 1.0f, SEG, RINGS);
    for (int i = 0; i < N; i += 3) {
        Vec3 n = normalize(cross(pos[i + 1] - pos[i], pos[i + 2] - pos[i]));
        if (length(n) == 0) n = smooth_n[i];                // pole triangles without area
        face_n[i] = face_n[i + 1] = face_n[i + 2] = n;
    }
    TestScene t;
    t.items[0].mesh = RenderMesh{ pos, face_n, N, 21 };
    t.lights[0] = dir_light(Vec3(-1, -1, -1));
    t.scene.light_count = 1;
    t.camera(Vec3(0, 0, 4), Vec3(0, 0, 0));
    Renderer r;
    CHECK(r.init(W, H));
    if (!render_ok(r, t.scene)) return;
    CHECK_EQ(r.stats().triangles, N / 3);
    static uint32_t a[W * H];
    memcpy(a, r.pixels(), sizeof a);
    CHECK(lum(t.at(r, Vec3(0, 0, 1))) > 10);
    t.items[0].mesh = RenderMesh{ pos, nullptr, N, 22 };
    if (!render_ok(r, t.scene)) return;
    int differing = 0;
    for (int i = 0; i < W * H; i++) if (a[i] != r.pixels()[i]) differing++;
    CHECK(differing <= 12);                                 // pole triangles aside
    // A vertex count that is not a multiple of 3 drops the rest.
    t.items[0].mesh = RenderMesh{ pos, face_n, N - 2, 23 };
    if (!render_ok(r, t.scene)) return;
    CHECK_EQ(r.stats().triangles, N / 3 - 1);
}

void test_jit_matches_interpreter() {
    // The shader JIT and the interpreter give the same picture bit for bit.
    TestScene t;
    t.scene.item_count = 2;
    t.lights[0] = dir_light(Vec3(-1, -1, -0.4f), Vec3(1, 0.9f, 0.8f), 2.0f);
    RenderLight sp;
    sp.type = LightType::Spot;
    sp.position = Vec3(1, 3, 1);
    sp.direction = normalize(Vec3(-0.3f, -1, -0.3f));
    sp.intensity = 5;
    t.lights[1] = sp;
    t.scene.light_count = 2;
    t.scene.post.tonemap = Tonemap::ACES;
    t.scene.post.vignette = 0.3f;
    t.scene.sky_light.intensity = 0.4f;
    t.scene.sky.mode = SkyMode::Gradient;
    static uint32_t jit[W * H];
    {
        fe_fgpu_host_set_jit(1);
        Renderer r;
        CHECK(r.init(W, H));
        if (!render_ok(r, t.scene)) return;
        memcpy(jit, r.pixels(), sizeof jit);
    }
    fe_fgpu_host_set_jit(0);
    {
        Renderer r;
        CHECK(r.init(W, H));
        if (render_ok(r, t.scene)) CHECK(memcmp(jit, r.pixels(), sizeof jit) == 0);
    }
    fe_fgpu_host_set_jit(1);
}

void test_errors() {
    Renderer r;
    TestScene t;
    CHECK(!r.ready());
    CHECK(!r.render(t.scene));
    CHECK(r.error()[0] != 0);
    CHECK(r.pixels() == nullptr);
    CHECK(!r.init(0, 100));
    CHECK(r.error()[0] != 0);
    CHECK(!r.init(100, 5000));
    CHECK(r.init(64, 48));
    CHECK(r.ready());
    // An item without vertices is skipped.
    t.items[0].mesh.positions = nullptr;
    CHECK(r.render(t.scene));
    CHECK_EQ(r.stats().draws, 0);
    t.items[0].mesh.positions = t.cube_p;
    t.items[0].mesh.vertex_count = 2;
    CHECK(r.render(t.scene));
    CHECK_EQ(r.stats().draws, 0);
}

void test_smoke_scene() {
    // The scene of the Minimal-OS program: print what the host draws so the two can be compared.
    SmokeScene s;
    s.build();
    Renderer r;
    uint32_t sum = 0;
    const char* why = smoke_check(r, s, &sum);       // the checks of the Minimal-OS program
    CHECK_MSG(why == nullptr, why ? why : "");
    if (why) fprintf(stderr, "smoke_check: %s (%s)\n", why, r.error());
    printf("gfx_smoke scene on the host: FE GFX PASS %08x\n", sum);
    CHECK(sum != 0);
}

}  // namespace

int main() {
    test_device();
    test_projection();
    test_lighting();
    test_light_types();
    test_post();
    test_sky();
    test_cache_and_resize();
    test_many_items_and_budget();
    test_big_mesh();
    test_jit_matches_interpreter();
    test_errors();
    test_smoke_scene();
    return finish_tests("gfx_tests");
}
