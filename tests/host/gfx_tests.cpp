// Tests of the FarcadeGL renderer (engine/gfx) on the host FGPU device:
// the device itself, the projection convention, lighting, sky, post
// processing, mesh caching, resizing and limits.
#include "gfx/renderer.h"
#include "gfx/meshgen.h"
#include "gfx/shader_file.h"
#include "gfx/host/farcadegl_host.h"
#include "../os/gfx_smoke/scene.h"
#include "mof_check.h"

#include <math.h>
#include <string.h>

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

void* read_test_asset(const char* path, size_t* size, void*) {
    static const uint8_t bmp[58] = {
        'B','M',58,0,0,0,0,0,0,0,54,0,0,0,
        40,0,0,0,1,0,0,0,1,0,0,0,1,0,24,0,
        0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,255,0
    };
    static const char shader[] =
        "// @vertex\n"
        "uniform mat4 fgl_ModelViewProjection;\n"
        "attribute vec3 position;\n"
        "void main() { gl_Position = fgl_ModelViewProjection * vec4(position, 1.0); }\n"
        "// @fragment\n"
        "void main() { gl_FragColor = vec4(0.0, 0.0, 1.0, 1.0); }\n";
    const void* src = nullptr;
    if (strcmp(path, "red.bmp") == 0) { src = bmp; *size = sizeof bmp; }
    else if (strcmp(path, "blue.fshader") == 0) { src = shader; *size = sizeof(shader) - 1; }
    else return nullptr;
    void* out = malloc(*size);
    if (out) memcpy(out, src, *size);
    return out;
}

void test_shader_file() {
    const char* text = "// @vertex\nattribute vec3 position;\nvoid main() {}\n"
                       "// @fragment\nvoid main() { gl_FragColor = vec4(1.0); }\n";
    char vertex[128], fragment[128], error[96];
    CHECK(parse_shader_file(text, strlen(text), vertex, sizeof(vertex), fragment, sizeof(fragment), error, sizeof(error)));
    CHECK(strstr(vertex, "attribute vec3 position") && !strstr(vertex, "gl_FragColor"));
    CHECK(strstr(fragment, "gl_FragColor") && !strstr(fragment, "attribute vec3"));
    const char* duplicate = "// @vertex\nvoid main() {}\n// @vertex\nvoid main() {}\n// @fragment\nvoid main() {}\n";
    CHECK(!parse_shader_file(duplicate, strlen(duplicate), vertex, sizeof(vertex), fragment, sizeof(fragment), error, sizeof(error)) &&
          strstr(error, "duplicate vertex"));
}

void test_asset_materials() {
    TestScene t;
    t.items[0].unlit = true;
    t.items[0].color = Vec3(1, 1, 1);
    t.items[0].texture_path = "red.bmp";
    t.scene.read_asset = read_test_asset;
    Renderer r;
    CHECK(r.init(W, H));
    if (!r.ready()) return;
    GraphicsSettings settings;
    settings.allow_gpu_effects_on_cpu = true;
    r.set_settings(settings);
    CHECK(render_ok(r, t.scene));
    CHECK(near_px(t.at(r, Vec3(0, 0, 0.49f)), 255, 0, 0, 4));
    t.items[0].texture_path = nullptr;
    t.items[0].shader_path = "blue.fshader";
    CHECK(render_ok(r, t.scene));
    CHECK(near_px(t.at(r, Vec3(0, 0, 0.49f)), 0, 0, 255, 4));
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

    // ABI 4 on the host device: scissored clears only touch the rectangle.
    CHECK(fglHasScissor());
    CHECK(fglHasMipmaps());
    CHECK(info.max_texture_units == 16u);
    fglEnable(gl, FGL_SCISSOR_TEST);
    fglScissor(gl, 4, 2, 8, 6);
    fglClearColor(gl, 1.0f, 0.0f, 0.0f);
    fglClear(gl, FGL_COLOR_BUFFER_BIT);
    fglDisable(gl, FGL_SCISSOR_TEST);
    CHECK(fglSubmit(gl) == 0);
    CHECK(fglReadPixels(gl, buf, 32 * 4) == 0);
    CHECK_EQ(buf[2 * 32 + 4] & 0xFFFFFF, 0xFF0000);         // first pixel inside
    CHECK_EQ(buf[7 * 32 + 11] & 0xFFFFFF, 0xFF0000);        // last pixel inside
    CHECK_EQ(buf[2 * 32 + 12] & 0xFFFFFF, 0x336699);        // right of it
    CHECK_EQ(buf[8 * 32 + 4] & 0xFFFFFF, 0x336699);         // below it
    CHECK_EQ(buf[0] & 0xFFFFFF, 0x336699);
    fglClearColor(gl, 0.2f, 0.4f, 0.6f);

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

    const char* buffer_fs =
        "layout(binding = 0) buffer vec4 colors[];\n"
        "uniform float u_index;\n"
        "void main() { gl_FragColor = colors[u_index]; }\n";
    FGLShader* buffer_sh = fglCreateShader(gl, vs, buffer_fs);
    CHECK(buffer_sh != nullptr);
    if (buffer_sh) {
        const float color_data[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
        FGLBuffer color_buffer = fglCreateBuffer(gl, sizeof color_data);
        CHECK(color_buffer != 0);
        CHECK(fglBufferData(gl, color_buffer, 0, color_data, sizeof color_data) == 0);
        fglBindStorageBuffer(gl, 0, color_buffer);
        fglUniform1f(buffer_sh, fglUniformLocation(buffer_sh, "u_index"), 0.0f);
        fglUseShader(gl, buffer_sh);
        fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
        fglDraw(gl, vb, 0, 3);
        CHECK(fglSubmit(gl) == 0);
        CHECK(fglReadPixels(gl, buf, 32 * 4) == 0);
        CHECK_EQ(buf[12 * 32 + 16] & 0xFFFFFF, 0x0000FF);

        fglUniform1f(buffer_sh, fglUniformLocation(buffer_sh, "u_index"), 1.0f);
        fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
        fglDraw(gl, vb, 0, 3);
        CHECK(fglSubmit(gl) == 0);
        CHECK(fglReadPixels(gl, buf, 32 * 4) == 0);
        CHECK_EQ(buf[12 * 32 + 16] & 0xFFFFFF, 0x000000);
        fglDestroyShader(gl, buffer_sh);
        fglDestroyBuffer(gl, color_buffer);
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
    // The same with the GPU tier forced (what tests/os/gfx_smoke prints as the second checksum).
    GraphicsSettings g;
    g.apply_preset(GraphicsPreset::High);
    g.shadow_resolution = 512;
    g.allow_gpu_effects_on_cpu = true;
    s.scene.post.bloom = 0.4f;
    s.scene.post.bloom_threshold = 0.8f;
    s.scene.sky.fog = 0.2f;
    Renderer h;
    h.set_settings(g);
    CHECK(h.init(kSmokeWidth, kSmokeHeight));
    if (render_ok(h, s.scene)) {
        CHECK(h.gpu_tier_stats().passes >= 5);
        uint32_t hq = checksum(h.pixels(), kSmokeWidth * kSmokeHeight);
        SmokeLocalLights ll;
        ll.apply(s);
        if (render_ok(h, s.scene)) {
            CHECK_EQ(h.gpu_tier_stats().local_shadows, 2);
            printf("gfx_smoke gpu tier on the host: FE GFX PASS %08x %08x %08x\n", sum, hq, checksum(h.pixels(), kSmokeWidth * kSmokeHeight));
        }
    }
}

// ---------------------------------------------------------------- the GPU tier

// Everything off, the tier forced on: tests switch on what they look at.
GraphicsSettings tier_off() {
    GraphicsSettings g;
    g.preset = GraphicsPreset::Custom;
    g.shadows = g.ambient_occlusion = g.bloom = g.antialiasing = g.pbr = false;
    g.local_shadows = false;
    g.shadow_resolution = 512;
    g.allow_gpu_effects_on_cpu = true;
    return g;
}

int lum_at(const TestScene& t, const Renderer& r, Vec3 p) { return lum(t.at(r, p)); }

void test_features() {
    Renderer r;
    TestScene t;
    GraphicsSettings g;                         // defaults: everything on, not forced
    r.set_settings(g);
    CHECK(r.init(W, H));
    CHECK(strstr(r.backend(), "cpu") != nullptr);
    GraphicsFeatures f = r.features();
    CHECK(!f.gpu);
    CHECK(!f.gpu_tier);
    CHECK(!f.shadows && !f.ambient_occlusion && !f.bloom && !f.antialiasing && !f.pbr);
    CHECK(render_ok(r, t.scene));
    CHECK_EQ(r.gpu_tier_stats().passes, 0);     // the base tier ran

    g.allow_gpu_effects_on_cpu = true;
    r.set_settings(g);
    f = r.features();
    CHECK(!f.gpu);
    CHECK(f.gpu_tier && f.shadows && f.ambient_occlusion && f.bloom && f.antialiasing && f.pbr);
    g.shadows = false; r.set_settings(g);
    f = r.features(); CHECK(!f.shadows && f.ambient_occlusion && f.bloom && f.antialiasing && f.pbr);
    g.ambient_occlusion = false; r.set_settings(g);
    f = r.features(); CHECK(!f.ambient_occlusion && f.bloom);
    g.bloom = false; r.set_settings(g);
    f = r.features(); CHECK(!f.bloom && f.antialiasing);
    g.antialiasing = false; r.set_settings(g);
    f = r.features(); CHECK(!f.antialiasing && f.pbr);
    g.pbr = false; r.set_settings(g);
    f = r.features(); CHECK(f.gpu_tier && !f.pbr);
    CHECK(r.settings().allow_gpu_effects_on_cpu);

    // Forced on but every effect off: the base tier's picture, bit for bit.
    uint32_t base_sum = 0, forced_sum = 0;
    {
        SmokeScene s;
        s.build();
        Renderer a, b;
        CHECK(a.init(kSmokeWidth, kSmokeHeight));
        CHECK(b.init(kSmokeWidth, kSmokeHeight));
        b.set_settings(tier_off());
        CHECK(a.render(s.scene));
        CHECK(b.render(s.scene));
        base_sum = checksum(a.pixels(), kSmokeWidth * kSmokeHeight);
        forced_sum = checksum(b.pixels(), kSmokeWidth * kSmokeHeight);
        CHECK_EQ(b.gpu_tier_stats().passes, 0);
    }
    CHECK_EQ(base_sum, forced_sum);
    // Settings set before init survive it.
    Renderer c;
    c.set_settings(tier_off());
    CHECK(c.init(64, 48));
    CHECK(c.features().gpu_tier);
}

// A cube on the floor under a sun looking down onto it from above.
struct ShadowRig {
    TestScene t;
    ShadowRig() {
        t.items[0].model = Mat4::translation(Vec3(0, 0.5f, 0));
        t.scene.item_count = 2;
        t.lights[0] = dir_light(Vec3(1, -1, 0));
        t.scene.light_count = 1;
        t.scene.sky_light.intensity = 0.3f;
        t.scene.sky_light.sky_color = Vec3(1, 1, 1);
        t.scene.sky_light.ground_color = Vec3(1, 1, 1);
        t.camera(Vec3(1.0f, 7.0f, 4.0f), Vec3(1.0f, 0, 0), 50.0f);
    }
};

void test_shadows() {
    ShadowRig rig;
    TestScene& t = rig.t;
    const Vec3 shadowed(1.0f, 0, 0), lit(1.0f, 0, 1.6f);
    Renderer r;
    CHECK(r.init(W, H));
    GraphicsSettings g = tier_off();
    r.set_settings(g);
    if (render_ok(r, t.scene)) CHECK(abs(lum_at(t, r, shadowed) - lum_at(t, r, lit)) < 6);
    g.shadows = true;
    r.set_settings(g);
    if (render_ok(r, t.scene)) {
        int a = lum_at(t, r, shadowed), b = lum_at(t, r, lit);
        CHECK_MSG(a < b * 0.7f, "ground in the cube's shadow is darker");
        CHECK(r.gpu_tier_stats().shadow_ms > 0 || r.gpu_tier_stats().shadow_reused);
        // the top of the cube is lit, no acne on the lit ground
        CHECK(lum_at(t, r, Vec3(0, 0.5f, 0)) > 100);
        CHECK(abs(lum_at(t, r, Vec3(-2, 0, 1)) - lum_at(t, r, Vec3(3, 0, 3))) < 12);
        // a second identical frame reuses the map
        CHECK(render_ok(r, t.scene));
        CHECK(r.gpu_tier_stats().shadow_reused);
    }
    // hard shadows too
    g.soft_shadows = false;
    r.set_settings(g);
    if (render_ok(r, t.scene)) CHECK(lum_at(t, r, shadowed) < lum_at(t, r, lit) * 0.7f);
    // the cube does not cast: no shadow
    g.soft_shadows = true;
    r.set_settings(g);
    t.items[0].cast_shadows = false;
    if (render_ok(r, t.scene)) CHECK(abs(lum_at(t, r, shadowed) - lum_at(t, r, lit)) < 8);
    t.items[0].cast_shadows = true;
    // the light does not cast shadows
    t.lights[0].shadows = false;
    if (render_ok(r, t.scene)) CHECK(abs(lum_at(t, r, shadowed) - lum_at(t, r, lit)) < 8);
}

// Point and spot light shadows from the atlas.
Mat4 wall_model(Vec3 at, Vec3 facing_away, Vec3 side) {
    // The plane's normal (+y) turned to -facing_away, x along `side`.
    Vec3 y = facing_away * -1.0f, x = side, z = cross(x, y);
    Mat4 m = Mat4::identity();
    m.m[0] = x.x; m.m[1] = x.y; m.m[2] = x.z;
    m.m[4] = y.x; m.m[5] = y.y; m.m[6] = y.z;
    m.m[8] = z.x; m.m[9] = z.y; m.m[10] = z.z;
    m.m[12] = at.x; m.m[13] = at.y; m.m[14] = at.z;
    return m;
}

void test_local_shadows() {
    GraphicsSettings g = tier_off();
    g.local_shadows = true;
    g.local_shadow_lights = 4;
    g.local_shadow_resolution = 1024;
    Renderer r;
    r.set_settings(g);
    CHECK(r.init(W, H));
    CHECK(r.features().local_shadows);

    // A point light, a cube 2 m away and a wall 4 m away, on each of the six
    // sides: the wall behind the cube is in shadow (every cube face of the
    // light's shadow), the wall beside it is not.
    const Vec3 axes[6] = { Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(0, -1, 0), Vec3(0, 0, 1), Vec3(0, 0, -1) };
    const Vec3 L(0.3f, 5.0f, -0.2f);
    int faces_ok = 0, sides_ok = 0;
    for (int f = 0; f < 6; f++) {
        Vec3 a = axes[f];
        Vec3 side = fabsf(a.y) > 0.5f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
        TestScene t;
        t.items[0].model = Mat4::translation(L + a * 2.0f);
        t.items[1].model = wall_model(L + a * 4.0f, a, side);
        t.scene.item_count = 2;
        RenderLight pl;
        pl.type = LightType::Point;
        pl.position = L;
        pl.range = 12.0f;
        pl.intensity = 3.0f;
        t.lights[0] = pl;
        t.scene.light_count = 1;
        Vec3 behind = L + a * 4.0f, beside = L + a * 4.0f + side * 2.2f;
        Vec3 perp = cross(a, side);
        t.camera(L + a * 2.0f + perp * 7.0f, behind, 60.0f);
        g.local_shadows = false; r.set_settings(g);
        int b0 = 0, s0 = 0, b1 = 0, s1 = 0;
        if (render_ok(r, t.scene)) { b0 = lum_at(t, r, behind); s0 = lum_at(t, r, beside); }
        g.local_shadows = true; r.set_settings(g);
        if (render_ok(r, t.scene)) {
            b1 = lum_at(t, r, behind); s1 = lum_at(t, r, beside);
            CHECK_EQ(r.gpu_tier_stats().local_shadows, 1);
        }
        bool face = b0 > 30 && b1 < b0 / 3;
        bool sidelit = abs(s1 - s0) <= 4;
        faces_ok += face;
        sides_ok += sidelit;
        if (!face || !sidelit) fprintf(stderr, "point shadow face %d: behind %d -> %d, beside %d -> %d\n", f, b0, b1, s0, s1);
    }
    CHECK_MSG(faces_ok == 6, "a point light shadows all six directions");
    CHECK_MSG(sides_ok == 6, "the wall next to the shadow stays lit (no acne, no other tile)");

    // A spot light pointing down onto a cube on the floor.
    {
        TestScene t;
        t.items[0].model = Mat4::translation(Vec3(0, 1.5f, 0));
        t.scene.item_count = 2;
        RenderLight sl;
        sl.type = LightType::Spot;
        sl.position = Vec3(0, 4.0f, 0);
        sl.direction = Vec3(0, -1, 0);
        sl.spot_angle = 80.0f;
        sl.range = 10.0f;
        sl.intensity = 3.0f;
        t.lights[0] = sl;
        t.scene.light_count = 1;
        t.camera(Vec3(0.5f, 6.0f, 5.0f), Vec3(0, 0, 0), 55.0f);
        const Vec3 under(0, 0, 0), out(1.6f, 0, 0);
        g.local_shadows = false; r.set_settings(g);
        int u0 = 0, o0 = 0;
        if (render_ok(r, t.scene)) { u0 = lum_at(t, r, under); o0 = lum_at(t, r, out); }
        g.local_shadows = true; r.set_settings(g);
        if (render_ok(r, t.scene)) {
            int u1 = lum_at(t, r, under), o1 = lum_at(t, r, out);
            CHECK_MSG(u0 > 30 && u1 < u0 / 3, "the floor under the cube is in the spot light's shadow");
            CHECK(abs(o1 - o0) <= 4);
            if (!(u1 < u0 / 3) || abs(o1 - o0) > 4) fprintf(stderr, "spot: under %d -> %d, out %d -> %d\n", u0, u1, o0, o1);
            CHECK(render_ok(r, t.scene));
            CHECK(r.gpu_tier_stats().atlas_reused);        // nothing moved: the atlas is kept
        }
        // the cube does not cast / the light has no shadows / the option is off
        t.items[0].cast_shadows = false;
        if (render_ok(r, t.scene)) CHECK(abs(lum_at(t, r, under) - u0) <= 4);
        t.items[0].cast_shadows = true;
        t.lights[0].shadows = false;
        if (render_ok(r, t.scene)) { CHECK(abs(lum_at(t, r, under) - u0) <= 4); CHECK_EQ(r.gpu_tier_stats().local_shadows, 0); }
        t.lights[0].shadows = true;
        // soft and hard filtering both work
        g.soft_shadows = false; r.set_settings(g);
        if (render_ok(r, t.scene)) CHECK(lum_at(t, r, under) < u0 / 3);
        g.soft_shadows = true; r.set_settings(g);
    }

    // The atlas budget: three point lights need 18 tiles, the atlas has 16;
    // with a spot light that is 2 points and the spot. The light count setting caps it too.
    {
        TestScene t;
        t.scene.item_count = 2;
        t.items[0].model = Mat4::translation(Vec3(0, 0.5f, 0));
        for (int i = 0; i < 3; i++) {
            RenderLight pl;
            pl.type = LightType::Point;
            pl.position = Vec3((float)i * 2.0f - 2.0f, 2.0f, 1.0f);
            pl.range = 8.0f;
            t.lights[i] = pl;
        }
        RenderLight sl;
        sl.type = LightType::Spot;
        sl.position = Vec3(0, 3, 0);
        sl.direction = Vec3(0, -1, 0);
        t.lights[3] = sl;
        t.scene.light_count = 4;
        if (render_ok(r, t.scene)) CHECK_EQ(r.gpu_tier_stats().local_shadows, 3);
        g.local_shadow_lights = 1; r.set_settings(g);
        if (render_ok(r, t.scene)) CHECK_EQ(r.gpu_tier_stats().local_shadows, 1);
        g.local_shadow_lights = 4; r.set_settings(g);
    }
}

// The biggest lit shader: 8 lights (a sun, points, spots), soft sun shadow,
// AO, PBR, fog, HDR (bloom) and four point / spot shadows must compile (32
// registers, 2048 instructions).
void test_worst_case_shader() {
    GraphicsSettings g;
    g.apply_preset(GraphicsPreset::Ultra);
    g.allow_gpu_effects_on_cpu = true;
    g.shadow_resolution = 512;
    g.local_shadow_resolution = 512;
    for (int combo = 0; combo < 3; combo++) {
        Renderer r;
        r.set_settings(g);
        CHECK(r.init(160, 120));
        ShadowRig rig;
        TestScene& t = rig.t;
        t.scene.post.bloom = 0.5f;
        t.scene.post.bloom_threshold = 0.8f;
        t.scene.sky.fog = 0.3f;
        int n = 1;      // the sun is lights[0]
        int points = combo == 0 ? 4 : (combo == 1 ? 1 : 2);
        for (int i = 0; i < 7; i++) {
            RenderLight l;
            l.type = i < points ? LightType::Point : LightType::Spot;
            l.position = Vec3((float)(i % 4) - 1.5f, 2.0f + (float)(i / 4), 1.0f);
            l.direction = Vec3(0, -1, 0);
            l.range = 6.0f;
            t.lights[n++] = l;
        }
        t.scene.light_count = n;
        bool ok = r.render(t.scene);
        CHECK_MSG(ok, "the worst-case lit shader compiles");
        if (!ok) fprintf(stderr, "worst case %d points: %s\n", points, r.error());
        else CHECK(r.gpu_tier_stats().local_shadows >= 2);
    }
}

void test_pbr() {
    Vec3 sph_p[sphere_vertices(24, 12)], sph_n[sphere_vertices(24, 12)];
    make_sphere(sph_p, sph_n, 1.0f, 24, 12);
    TestScene t;
    t.items[0].mesh = RenderMesh{ sph_p, sph_n, (uint32_t)sphere_vertices(24, 12), 31 };
    t.items[0].color = Vec3(0.9f, 0.2f, 0.1f);
    t.lights[0] = dir_light(Vec3(0, -0.5f, -1));
    t.scene.light_count = 1;
    t.scene.sky_light.intensity = 0.3f;
    t.camera(Vec3(0, 0.5f, 4), Vec3(0, 0, 0), 40.0f);
    Renderer r;
    CHECK(r.init(W, H));
    GraphicsSettings g = tier_off();
    g.pbr = true;
    r.set_settings(g);
    Px dielectric{}, metal{};
    t.items[0].roughness = 0.4f;
    t.items[0].metallic = 0;
    if (render_ok(r, t.scene)) dielectric = px(r, W / 2, H / 2 + 4);
    t.items[0].metallic = 1;
    if (render_ok(r, t.scene)) metal = px(r, W / 2, H / 2 + 4);
    CHECK_MSG(abs(dielectric.r - metal.r) + abs(dielectric.g - metal.g) + abs(dielectric.b - metal.b) > 20, "metallic changes the look");
    // the highlight: the area above half of its peak
    auto area = [&](float rough) {
        t.items[0].metallic = 0;
        t.items[0].roughness = rough;
        t.items[0].color = Vec3(0.05f, 0.05f, 0.05f);
        if (!render_ok(r, t.scene)) return 0;
        int peak = 0;
        for (int i = 0; i < W * H; i++) peak = peak > lum(px(r, i % W, i / W)) ? peak : lum(px(r, i % W, i / W));
        int n = 0;
        for (int i = 0; i < W * H; i++) if (lum(px(r, i % W, i / W)) * 2 > peak && lum(px(r, i % W, i / W)) > 30) n++;
        return n;
    };
    int smooth_area = area(0.15f), rough_area = area(0.7f);
    CHECK_MSG(smooth_area < rough_area, "a rough surface has a larger highlight");
    CHECK(smooth_area > 0);
    // plain Blinn-Phong with the tier on still draws
    g.pbr = false;
    r.set_settings(g);
    CHECK(render_ok(r, t.scene));
}

void test_fog() {
    for (int forced = 0; forced < 2; forced++) {
        TestScene t;
        t.items[0].color = Vec3(0.5f, 0.5f, 0.5f);
        t.scene.item_count = 2;
        t.items[1].color = Vec3(0.2f, 0.5f, 0.2f);
        t.lights[0] = dir_light(Vec3(0, -1, -0.2f));
        t.scene.light_count = 1;
        t.scene.sky_light.intensity = 0.5f;
        t.scene.sky.mode = SkyMode::Gradient;
        t.scene.sky.horizon = Vec3(0.9f, 0.9f, 0.9f);
        t.scene.sky.zenith = Vec3(0.2f, 0.3f, 0.8f);
        t.camera(Vec3(0, 1.0f, 8), Vec3(0, 0.9f, 0), 50.0f);
        Renderer r;
        GraphicsSettings g = forced ? tier_off() : GraphicsSettings();
        if (!forced) g.allow_gpu_effects_on_cpu = false;
        r.set_settings(g);
        CHECK(r.init(W, H));
        Vec3 far_ground(0, 0, -17);
        CHECK(render_ok(r, t.scene));
        Px clear = t.at(r, far_ground);
        t.scene.sky.fog = 1.0f;
        CHECK(render_ok(r, t.scene));
        Px fogged = t.at(r, far_ground);
        Px hor = { srgb8(0.9f), srgb8(0.9f), srgb8(0.9f) };
        auto dist = [&](Px a) { return abs(a.r - hor.r) + abs(a.g - hor.g) + abs(a.b - hor.b); };
        CHECK_MSG(dist(fogged) < dist(clear) - 40, "far ground blends towards the horizon color");
        // near ground barely changes
        t.scene.sky.fog = 0;
        CHECK(render_ok(r, t.scene));
        Px near_clear = t.at(r, Vec3(0, 0, 6));
        t.scene.sky.fog = 1.0f;
        CHECK(render_ok(r, t.scene));
        Px near_fog = t.at(r, Vec3(0, 0, 6));
        CHECK(abs(near_fog.g - near_clear.g) < 40);
        CHECK_EQ(r.gpu_tier_stats().passes, 1);  // fog alone: the lit pass only
    }
}

void test_ssao() {
    TestScene t;
    t.items[0].model = Mat4::translation(Vec3(0, 0.5f, 0));
    t.scene.item_count = 2;
    t.scene.sky_light.intensity = 1.0f;
    t.scene.sky_light.sky_color = Vec3(1, 1, 1);
    t.scene.sky_light.ground_color = Vec3(1, 1, 1);
    t.camera(Vec3(2.5f, 2.5f, 3.5f), Vec3(0, 0.3f, 0), 50.0f);
    Renderer r;
    CHECK(r.init(W * 2, H * 2));
    GraphicsSettings g = tier_off();
    r.set_settings(g);
    const Vec3 corner(0.56f, 0, 0.56f), open(-3.0f, 0, 3.0f);
    auto at = [&](Vec3 p) { return lum(t.at(r, p)); };
    int c0 = 0, o0 = 0, c1 = 0, o1 = 0;
    if (render_ok(r, t.scene)) { c0 = at(corner); o0 = at(open); }
    g.ambient_occlusion = true;
    r.set_settings(g);
    if (render_ok(r, t.scene)) { c1 = at(corner); o1 = at(open); }
    CHECK_MSG(c1 < c0 - 8, "the crease between cube and floor is darker with AO");
    CHECK_MSG(abs(o1 - o0) <= 6, "open floor is unchanged");
    CHECK(r.gpu_tier_stats().ssao_ms > 0);
}

void test_bloom() {
    TestScene t;
    t.items[0].unlit = true;
    t.items[0].color = Vec3(3, 3, 3);
    t.scene.sky.mode = SkyMode::Color;
    t.scene.sky.zenith = Vec3(0.01f, 0.01f, 0.01f);
    t.scene.post.bloom = 1.0f;
    t.scene.post.bloom_threshold = 0.5f;
    t.camera(Vec3(0, 0, 4), Vec3(0, 0, 0), 40.0f);
    Renderer r;
    CHECK(r.init(W, H));
    GraphicsSettings g = tier_off();
    r.set_settings(g);
    CHECK(render_ok(r, t.scene));
    int x = W / 2;
    while (x < W - 1 && lum(px(r, x, H / 2)) > 40) x++;      // the first dark pixel to the right
    x += 2;
    int before = lum(px(r, x, H / 2)), far_before = lum(px(r, W - 2, 3));
    g.bloom = true;
    r.set_settings(g);
    CHECK(render_ok(r, t.scene));
    int after = lum(px(r, x, H / 2));
    CHECK_MSG(after > before + 10, "bloom spreads into the dark neighbourhood");
    CHECK(lum(px(r, W - 2, 3)) <= far_before + 4);
    CHECK(r.gpu_tier_stats().bloom_ms > 0);
    // strength 0 turns it off
    t.scene.post.bloom = 0;
    CHECK(render_ok(r, t.scene));
    CHECK(abs(lum(px(r, x, H / 2)) - before) <= 3);
}

void test_fxaa() {
    TestScene t;
    t.items[0].unlit = true;
    t.items[0].color = Vec3(0.9f, 0.9f, 0.9f);
    t.items[0].model = Mat4::rotation(Quat::axis_angle(Vec3(0, 0, 1), 0.5f));
    t.scene.sky.mode = SkyMode::Color;
    t.scene.sky.zenith = Vec3(0.0f, 0.0f, 0.0f);
    t.camera(Vec3(0, 0, 4), Vec3(0, 0, 0), 40.0f);
    Renderer r;
    CHECK(r.init(W, H));
    GraphicsSettings g = tier_off();
    r.set_settings(g);
    auto mids = [&]() {
        int n = 0;
        for (int i = 0; i < W * H; i++) { int l = lum(px(r, i % W, i / W)); if (l > 25 && l < 190) n++; }
        return n;
    };
    CHECK(render_ok(r, t.scene));
    int hard = mids();
    g.antialiasing = true;
    r.set_settings(g);
    CHECK(render_ok(r, t.scene));
    int soft = mids();
    CHECK_MSG(soft > hard + 20, "anti-aliasing adds intermediate values along edges");
    CHECK(r.gpu_tier_stats().fxaa_ms > 0);
    // the inside of a flat area is untouched
    CHECK(abs(lum(px(r, W / 2, H / 2)) - 242) < 12 || lum(px(r, W / 2, H / 2)) > 200);
}

void test_gpu_tier_everything() {
    fe_fgpu_host_stats_t base;
    fe_fgpu_host_stats(&base);
    Renderer r;
    GraphicsSettings g;
    g.apply_preset(GraphicsPreset::Ultra);
    g.shadow_resolution = 1024;
    g.allow_gpu_effects_on_cpu = true;
    r.set_settings(g);
    CHECK(r.init(W, H));
    ShadowRig rig;
    TestScene& t = rig.t;
    t.scene.post.bloom = 0.5f;
    t.scene.post.bloom_threshold = 0.8f;
    t.scene.sky.fog = 0.3f;
    t.items[0].metallic = 0.5f;
    fe_fgpu_host_stats_t s0, s1, s2;
    int surf_steady = -1, prog_steady = -1;
    for (int i = 0; i < 50; i++) {
        t.items[0].model = Mat4::translation(Vec3(0, 0.5f, 0)) * Mat4::rotation(Quat::axis_angle(Vec3(0, 1, 0), 0.05f * (float)i));
        if (!render_ok(r, t.scene)) break;
        if (i == 2) { fe_fgpu_host_stats(&s1); surf_steady = r.gpu_tier_surfaces(); prog_steady = r.gpu_tier_programs(); }
    }
    fe_fgpu_host_stats(&s2);
    CHECK(r.error()[0] == 0);
    CHECK_EQ(s1.surfaces, s2.surfaces);
    CHECK_EQ(s1.programs, s2.programs);
    CHECK_EQ(s1.buffers, s2.buffers);
    CHECK_EQ(r.gpu_tier_surfaces(), surf_steady);
    CHECK_EQ(r.gpu_tier_programs(), prog_steady);
    CHECK(r.gpu_tier_surfaces() > 6);
    CHECK_EQ(r.stats().draws, 2);               // the main pass only
    const GpuTierStats& st = r.gpu_tier_stats();
    CHECK(st.lit_ms > 0 && st.ssao_ms > 0 && st.bloom_ms > 0 && st.composite_ms > 0 && st.fxaa_ms > 0);

    // resize with everything on, several times, also to odd sizes
    const int sizes[][2] = { { 97, 61 }, { 33, 17 }, { 200, 150 }, { 1, 1 }, { W, H } };
    for (auto& sz : sizes) {
        CHECK(r.resize(sz[0], sz[1]));
        t.camera(Vec3(1.0f, 7.0f, 4.0f), Vec3(1.0f, 0, 0), 50.0f, (float)sz[0] / (float)sz[1]);
        CHECK(render_ok(r, t.scene));
        CHECK_EQ(r.width(), sz[0]);
    }
    int surfaces_before = r.gpu_tier_surfaces();
    CHECK(surfaces_before > 0);
    r.shutdown();
    fe_fgpu_host_stats(&s0);
    CHECK_EQ(s0.surfaces, base.surfaces);      // nothing leaks
    CHECK_EQ(s0.programs, base.programs);
    CHECK_EQ(s0.buffers, base.buffers);
}

// render_rows draws the same picture as render(): bands, with a vignette that
// depends on the pixel position, and draft shading.
void test_bands() {
    Renderer r;
    CHECK(r.init(W, H));
    TestScene t;
    t.camera(Vec3(0, 0, 4), Vec3(0, 0, 0));
    t.scene.post.vignette = 0.6f;
    for (int draft = 0; draft < 2; draft++) {
        t.scene.draft = draft != 0;
        if (!render_ok(r, t.scene)) return;
        uint32_t* full = (uint32_t*)malloc((size_t)W * H * 4);
        uint32_t* out = (uint32_t*)calloc((size_t)W * H, 4);
        uint32_t* tmp = (uint32_t*)malloc((size_t)W * 7 * 4);
        for (int i = 0; i < W * H; i++) {
            uint32_t p = r.pixels()[i];
            full[i] = (p & 0xFF00) | ((p & 0xFF) << 16) | ((p >> 16) & 0xFF);
        }
        const int rows = 7;
        for (int y0 = 0; y0 < H; y0 += rows) {
            int y = y0 + rows > H ? H - rows : y0;
            if (!r.render_rows(t.scene, y, rows, tmp)) { fprintf(stderr, "render_rows: %s\n", r.error()); CHECK(false); break; }
            memcpy(out + (size_t)y * W, tmp, (size_t)W * rows * 4);
        }
        int bad = 0;
        for (int i = 0; i < W * H; i++) if ((out[i] & 0xFFFFFF) != (full[i] & 0xFFFFFF)) bad++;
        CHECK_MSG(bad == 0, "bands differ from the full render in some pixels");
        free(full); free(out); free(tmp);
    }
    t.scene.draft = false;
    CHECK(render_ok(r, t.scene));       // and a full render still works after bands
}


// Particles over the image (particles.h): drawn after the readback, hidden by
// meshes in front of them, the same in bands.
void test_particles() {
    Renderer r;
    CHECK(r.init(W, H));
    TestScene t;
    t.camera(Vec3(0, 0, 6), Vec3(0, 0, 0));
    Particle p;
    p.pos = Vec3(0, 0, -2);         // behind the unit cube at the origin
    p.size = 1.0f; p.r = 1; p.g = 0; p.b = 0; p.a = 1;
    ParticleBatch b;
    b.items = &p; b.count = 1; b.additive = false;
    if (!render_ok(r, t.scene)) return;
    Px before = t.at(r, Vec3(0.9f, 0.3f, 0.5f));
    p.pos = Vec3(0.9f, 0.3f, 0.5f);   // beside the cube, in front of the background
    t.scene.particles = &b;
    t.scene.particle_batch_count = 1;
    if (!render_ok(r, t.scene)) return;
    Px red = t.at(r, p.pos);
    CHECK_MSG(red.r > 180 && red.g < 60, "a red particle shows over the sky");
    CHECK_MSG(before.r < 120, "(sanity: the pixel was not red without it)");
    // Behind the cube: hidden by it.
    p.pos = Vec3(0, 0, -2);
    p.size = 0.4f;
    if (!render_ok(r, t.scene)) return;
    Px hidden = t.at(r, Vec3(0, 0, 0.5f));
    CHECK_MSG(!(hidden.r > 150 && hidden.g < 60), "a particle behind the cube is hidden by it");
    // In front of the cube: visible.
    p.pos = Vec3(0, 0, 2.5f);
    if (!render_ok(r, t.scene)) return;
    Px shown = t.at(r, p.pos);
    CHECK_MSG(shown.r > 150 && shown.g < 80, "a particle in front of the cube shows");
    // Bands draw the same.
    uint32_t* tmp = (uint32_t*)malloc((size_t)W * 6 * 4);
    float fx, fy;
    t.project(p.pos, &fx, &fy);
    int y0 = (int)fy - 3;
    if (r.render_rows(t.scene, y0, 6, tmp)) {
        uint32_t v = tmp[3 * W + (int)fx];
        CHECK_MSG(((v >> 16) & 0xFF) > 150 && (v & 0xFF) < 80, "bands draw particles too");
    } else {
        CHECK_MSG(false, "render_rows with particles");
    }
    free(tmp);
    // A text label, drawn with the built-in font.
    TextLabel lab;
    lab.pos = Vec3(0, 1.5f, 0); lab.height = 0.8f; lab.r = 1; lab.g = 1; lab.b = 0;
    snprintf(lab.text, sizeof lab.text, "OK");
    t.scene.particle_batch_count = 0;
    t.scene.labels = &lab;
    t.scene.label_count = 1;
    if (!render_ok(r, t.scene)) return;
    int yellow = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { Px q = px(r, x, y); if (q.r > 200 && q.g > 200 && q.b < 90) yellow++; }
    CHECK_MSG(yellow > 20, "a text label shows in the image");
}

}  // namespace

int main() {
    test_shader_file();
    test_asset_materials();
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
    test_features();
    test_shadows();
    test_local_shadows();
    test_worst_case_shader();
    test_pbr();
    test_fog();
    test_ssao();
    test_bloom();
    test_fxaa();
    test_gpu_tier_everything();
    test_bands();
    test_particles();
    return finish_tests("gfx_tests");
}
