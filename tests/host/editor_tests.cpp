// Editor interaction tests on the headless platform: mouse and keyboard
// events go in, the editor's state is checked after a few frames.
#include "editor.h"
#include "swraster.h"
#include "ecs/ecs.h"
#include "script/runtime.h"
#include "audio/audio.h"
#include "scene_io.h"
#include "fx_bridge.h"
#include "fx/fx.h"
#include "terrain_bridge.h"
#include "render_bridge.h"
#include "platform/headless.h"
#include "platform/frame_pacer.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imguizmo/ImGuizmo.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

using namespace fe;

static int g_checks, g_failed;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static float absf_(float x) { return x < 0 ? -x : x; }
static Editor* E;
static double g_time;

static void frames(int n) {
    for (int i = 0; i < n; i++) {
        g_time += 1.0 / 60.0;
        platform::headless_set_time(g_time);
        E->frame();
    }
}

static void ev_move(float x, float y) {
    platform::Event e;
    e.type = platform::EventType::MouseMove;
    e.x = x; e.y = y;
    platform::headless_push(e);
}

static void ev_button(float x, float y, int b, bool down) {
    platform::Event e;
    e.type = platform::EventType::MouseButton;
    e.x = x; e.y = y; e.button = b; e.down = down;
    platform::headless_push(e);
}

static void ev_key(uint16_t key, bool down, uint8_t mods = 0) {
    platform::Event e;
    e.type = platform::EventType::Key;
    e.key = key; e.down = down; e.mods = mods;
    platform::headless_push(e);
}

static void click(float x, float y, int b = 0) {
    ev_move(x, y); frames(1);
    ev_button(x, y, b, true); frames(1);
    ev_button(x, y, b, false); frames(2);
}

static void shift_click(float x, float y) {
    ev_key(platform::KEY_LSHIFT, true, platform::MOD_SHIFT); frames(1);
    ev_move(x, y); frames(1);
    ev_button(x, y, 0, true); frames(1);
    ev_button(x, y, 0, false); frames(1);
    ev_key(platform::KEY_LSHIFT, false, 0); frames(2);
}

static void double_click(float x, float y) {
    ev_move(x, y); frames(1);
    ev_button(x, y, 0, true); frames(1);
    ev_button(x, y, 0, false); frames(1);
    ev_button(x, y, 0, true); frames(1);
    ev_button(x, y, 0, false); frames(2);
}

static void drag(float x0, float y0, float x1, float y1, int b = 0, int steps = 12) {
    // Rest on the start point first, as a hand does (ImGuizmo only takes a
    // press on a gizmo it has seen hovered).
    ev_move(x0, y0); frames(3);
    ev_button(x0, y0, b, true); frames(1);
    for (int i = 1; i <= steps; i++) {
        ev_move(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
        frames(1);
    }
    ev_button(x1, y1, b, false); frames(2);
}

// World point to screen through the editor's camera and viewport.
static bool to_screen(const Vec3& p, float* sx, float* sy) {
    float* r = E->view_rect;
    Mat4 vp = E->camera.projection(r[2] / r[3]) * E->camera.view();
    Vec4 c = vp * Vec4{ p.x, p.y, p.z, 1 };
    if (c.w <= 0) return false;
    *sx = r[0] + (c.x / c.w * 0.5f + 0.5f) * r[2];
    *sy = r[1] + (0.5f - c.y / c.w * 0.5f) * r[3];
    return true;
}

static void object_screen(ObjectId id, float* sx, float* sy) {
    Object* o = E->scene.get(id);
    to_screen(o->position, sx, sy);
}

static const Editor::UiRect* ui(const char* key) { return E->find_rect(key); }

static bool click_ui(const char* key, int b = 0) {
    const Editor::UiRect* r = ui(key);
    if (!r) return false;
    // Below the panels (a long Inspector; the status bar takes the bottom): scroll the panel up to it first.
    const float win_h = 800.0f;
    for (int tries = 0; r && r->y + r->h > win_h - 36 && tries < 40; tries++) {
        ev_move(r->x + r->w * 0.5f, win_h - 60);
        platform::Event wheel;
        wheel.type = platform::EventType::MouseWheel;
        wheel.wheel_y = E->settings.invert_scrolling ? 1.0f : -1.0f;
        platform::headless_push(wheel);
        frames(1);
        r = ui(key);
    }
    if (!r) return false;
    click(r->x + r->w * 0.5f, r->y + r->h * 0.5f, b);
    return true;
}

static void type_text(const char* text) {
    for (const char* c = text; *c; c++) {
        platform::Event e;
        e.type = platform::EventType::Text;
        e.codepoint = (uint32_t)*c;
        platform::headless_push(e);
    }
}

static const Editor::TileRect* tile(const char* name) {
    for (int i = 0; i < E->file_tile_count; i++)
        if (!strcmp(path_filename(E->file_tiles[i].path), name)) return &E->file_tiles[i];
    return nullptr;
}

static void test_frame_pacer() {
    using platform::FramePacer;
    using platform::Vsync;
    const double p = 1.0 / 60.0;
    auto near = [](double a, double b) { return a - b < 1e-6 && b - a < 1e-6; };
    FramePacer f;
    f.set(Vsync::Off, 60);
    CHECK(near(f.present_time(1.234), 1.234), "frame pacer");
    // Double: frames on refreshes; a late one (25 ms) waits for the refresh after
    f.set(Vsync::Double, 60);
    double t0 = 10.0;
    CHECK(near(f.present_time(t0), t0), "frame pacer");
    CHECK(near(f.present_time(t0 + 0.005), t0 + p), "frame pacer");
    CHECK(near(f.present_time(t0 + p + 0.025), t0 + 3 * p), "frame pacer");
    CHECK(near(f.present_time(t0 + 3 * p + 0.001), t0 + 4 * p), "frame pacer");     // never twice on one refresh
    // Adaptive: on time like Double; a late frame at once
    f.set(Vsync::Adaptive, 60);
    CHECK(near(f.present_time(20.0), 20.0), "frame pacer");
    CHECK(near(f.present_time(20.004), 20.0 + p), "frame pacer");
    CHECK(near(f.present_time(20.0 + p + 0.030), 20.0 + p + 0.030), "frame pacer");
    CHECK(near(f.present_time(20.0 + p + 0.032), 20.0 + 2 * p + 0.030), "frame pacer");
    // Triple: the next refresh at or after a time
    f.set(Vsync::Triple, 120);
    CHECK(near(f.next_refresh(5.0), 5.0), "frame pacer");
    CHECK(near(f.next_refresh(5.001), 5.0 + 1.0 / 120.0), "frame pacer");
    CHECK(near(f.next_refresh(5.0 + 2.0 / 120.0), 5.0 + 2.0 / 120.0), "frame pacer");
}

int main() {
    test_frame_pacer();
    const char* dir = "build/host/test-project";
    mkdir("build/host", 0755);
    mkdir(dir, 0755);
    platform::write_file("build/host/test-project/crate.png", "x", 1);
    platform::write_file("build/host/test-project/ship.mof", "x", 1);
    platform::write_file("build/host/test-project/player.lua", "x", 1);

    static Editor ed;
    E = &ed;
    CHECK(ed.init(1280, 800), "init");
    ed.verify_damage = true;        // every incremental redraw is compared with a full one
    snprintf(ed.project_dir, sizeof(ed.project_dir), "%s", dir);
    snprintf(ed.files_dir, sizeof(ed.files_dir), "%s", dir);
    ed.camera.target = Vec3(0, 0.5f, 0);
    ed.camera.yaw = 0;
    ed.camera.pitch = 20;
    ed.camera.distance = 8;
    ObjectId a = ed.create_object("A", MeshKind::Cube, Vec3(-1.5f, 0.5f, 0));
    ObjectId b = ed.create_object("B", MeshKind::Cube, Vec3(1.5f, 0.5f, 0));
    ed.scene.clear_selection();
    ed.tool = Tool::Select;
    frames(4);
    CHECK(ed.view_rect[2] > 100 && ed.view_rect[3] > 100, "viewport has a size (%f x %f)", ed.view_rect[2], ed.view_rect[3]);
    ed.show_settings = true;
    frames(2);
    CHECK(ui("settings:invert_scrolling"), "General settings exposes the invert scrolling toggle");
    CHECK(click_ui("settings:invert_scrolling") && ed.settings.invert_scrolling, "the General toggle enables inverted scrolling");
    CHECK(click_ui("settings:invert_scrolling") && !ed.settings.invert_scrolling, "the General toggle disables inverted scrolling");
    ed.show_settings = false;
    frames(1);
    {
        float x = ed.view_rect[0] + ed.view_rect[2] * 0.5f;
        float y = ed.view_rect[1] + ed.view_rect[3] * 0.5f;
        ev_move(x, y);
        frames(2);
        platform::Event wheel;
        wheel.type = platform::EventType::MouseWheel;
        wheel.wheel_y = 1;
        ed.settings.invert_scrolling = false;
        ed.camera.distance = 8;
        platform::headless_push(wheel);
        frames(1);
        CHECK(ed.camera.distance < 8, "normal scrolling zooms the viewport in");
        ed.settings.invert_scrolling = true;
        ed.camera.distance = 8;
        platform::headless_push(wheel);
        frames(1);
        CHECK(ed.camera.distance > 8, "inverted scrolling zooms the viewport out");
        ed.settings.invert_scrolling = false;
    }

    // ---- selection in the viewport
    float ax, ay, bx, by;
    object_screen(a, &ax, &ay);
    object_screen(b, &bx, &by);
    click(ax, ay);
    CHECK(ed.scene.selected_count() == 1 && ed.scene.is_selected(a), "click selects A (count %d)", ed.scene.selected_count());
    click(bx, by);
    CHECK(ed.scene.selected_count() == 1 && ed.scene.is_selected(b), "click selects only B");
    shift_click(ax, ay);
    CHECK(ed.scene.selected_count() == 2 && ed.scene.is_selected(a) && ed.scene.is_selected(b), "shift-click adds A");
    CHECK(ed.scene.active() == a, "the last shift-clicked object is active");
    shift_click(bx, by);
    CHECK(ed.scene.selected_count() == 1 && ed.scene.is_selected(a), "shift-click again removes B");
    click(ed.view_rect[0] + 20, ed.view_rect[1] + ed.view_rect[3] - 20);
    CHECK(ed.scene.selected_count() == 0, "click on empty space clears (count %d)", ed.scene.selected_count());

    // ---- move gizmo, with and without snap
    ed.scene.select_only(a);
    ed.tool = Tool::Move;
    ed.settings.snap = false;
    frames(2);
    Object* oa = ed.scene.get(a);
    Vec3 p0 = oa->position;
    float cx, cy, xx, xy;
    to_screen(p0, &cx, &cy);
    // A point on the X arrow, a bit out from the center.
    Vec3 along = p0 + Vec3(0.6f, 0, 0);
    to_screen(along, &xx, &xy);
    float dx = xx - cx, dy = xy - cy;
    float grab_x = cx + dx * 0.9f, grab_y = cy + dy * 0.9f;
    drag(grab_x, grab_y, grab_x + 80, grab_y);
    CHECK(oa->position.x > p0.x + 0.2f, "dragging the X arrow moves A along x (%.3f -> %.3f)", p0.x, oa->position.x);
    CHECK(oa->position.y == p0.y && oa->position.z == p0.z, "only x changes (%.3f %.3f)", oa->position.y, oa->position.z);

    ed.settings.snap = true;
    ed.settings.snap_move = 0.5f;
    oa->position = Vec3(-1.5f, 0.5f, 0);
    frames(2);
    to_screen(oa->position, &cx, &cy);
    to_screen(oa->position + Vec3(0.6f, 0, 0), &xx, &xy);
    grab_x = cx + (xx - cx) * 0.9f; grab_y = cy + (xy - cy) * 0.9f;
    drag(grab_x, grab_y, grab_x + 63, grab_y);
    float k = oa->position.x / 0.5f;
    CHECK(k == (float)(int)k && oa->position.x != -1.5f, "with snap, x lands on a 0.5 step (%.4f)", oa->position.x);

    // Two selected objects move together.
    ed.scene.select_only(b);
    ed.scene.toggle(a);
    Object* ob = ed.scene.get(b);
    Vec3 pb = ob->position, pa = oa->position;
    frames(2);
    to_screen(oa->position, &cx, &cy);
    to_screen(oa->position + Vec3(0.6f, 0, 0), &xx, &xy);
    grab_x = cx + (xx - cx) * 0.9f; grab_y = cy + (xy - cy) * 0.9f;
    drag(grab_x, grab_y, grab_x + 70, grab_y);
    float moved_a = oa->position.x - pa.x, moved_b = ob->position.x - pb.x;
    CHECK(moved_a > 0.2f && moved_b > moved_a - 1e-4f && moved_b < moved_a + 1e-4f, "the whole selection moves (%.3f, %.3f)", moved_a, moved_b);

    // Rotate snap: the gizmo's angle steps by snap_rotate.
    ed.settings.snap = true;
    ed.settings.snap_rotate = 15;
    ed.scene.select_only(a);
    ed.tool = Tool::Rotate;
    oa->rotation = Quat();
    frames(2);
    // Keyboard shortcuts switch tools.
    ev_key(platform::KEY_A + ('w' - 'a'), true); frames(1); ev_key(platform::KEY_A + ('w' - 'a'), false); frames(1);
    CHECK(ed.tool == Tool::Move, "W selects the move tool");
    ev_key(platform::KEY_A + ('r' - 'a'), true); frames(1); ev_key(platform::KEY_A + ('r' - 'a'), false); frames(1);
    CHECK(ed.tool == Tool::Scale, "R selects the scale tool");

    // ---- hierarchy: click and shift-click rows
    ImGuiWindow* hw = ImGui::FindWindowByName("###Hierarchy");
    CHECK(hw != nullptr, "hierarchy window exists");
    {
        ed.scene.clear_selection();
        frames(1);
        char ka[32], kb[32];
        snprintf(ka, sizeof(ka), "hier:%u", a);
        snprintf(kb, sizeof(kb), "hier:%u", b);
        const Editor::UiRect* ra = ui(ka);
        const Editor::UiRect* rb = ui(kb);
        CHECK(ra && rb, "hierarchy rows for A and B are drawn");
        if (ra && rb) {
            float x = ra->x + 40, ya = ra->y + ra->h * 0.5f, yb = rb->y + rb->h * 0.5f;
            click(x, ya);
            CHECK(ed.scene.selected_count() == 1 && ed.scene.is_selected(a), "clicking the first row selects A");
            shift_click(x, yb);
            CHECK(ed.scene.selected_count() == 2, "shift-clicking the second row adds B (count %d)", ed.scene.selected_count());
        }
        // The eye at the end of a row hides the object without selecting it.
        ed.scene.clear_selection();
        frames(1);
        char ke[32];
        snprintf(ke, sizeof(ke), "eye:%u", b);
        const Editor::UiRect* re = ui(ke);
        CHECK(re != nullptr, "B's row has a visibility button");
        if (re) {
            float ex = re->x + re->w * 0.5f, ey = re->y + re->h * 0.5f;
            ev_move(ex, ey); frames(2);
            click(ex, ey);
            CHECK(!ed.scene.get(b)->visible, "the eye hides B");
            CHECK(ed.scene.selected_count() == 0, "and does not select it (count %d)", ed.scene.selected_count());
            click(ex, ey);
            CHECK(ed.scene.get(b)->visible, "a second click shows B again");
        }
    }

    // ---- files: double-click opens in the configured editor
    frames(2);
    const Editor::TileRect* lua = tile("player.lua");
    CHECK(lua != nullptr, "player.lua is listed (%d tiles)", ed.file_tile_count);
    if (lua) {
        double_click(lua->x + lua->w * 0.5f, lua->y + lua->h * 0.5f);
        const char* opened = platform::headless_last_open();
        CHECK(strstr(opened, "edit.run") && strstr(opened, "player.lua"), "double-click opens player.lua in edit.run (\"%s\")", opened);
    }

    // Right-click > Open in file manager.
    const Editor::TileRect* png = tile("crate.png");
    CHECK(png != nullptr, "crate.png is listed");
    if (png) {
        click(png->x + png->w * 0.5f, png->y + png->h * 0.5f, 1);
        ImGuiContext& g = *ImGui::GetCurrentContext();
        CHECK(g.OpenPopupStack.Size == 1, "right-click opens a menu (%d)", g.OpenPopupStack.Size);
        const Editor::UiRect* item = ui("menu:Open in file manager");
        CHECK(item != nullptr, "the menu has \"Open in file manager\"");
        if (item) {
            click(item->x + 30, item->y + item->h * 0.5f);
            CHECK(strstr(platform::headless_last_reveal(), "crate.png") != nullptr, "Open in file manager asks for crate.png (\"%s\")",
                  platform::headless_last_reveal());
        }
    }

    // ---- drag a file from Files onto an object
    ed.tool = Tool::Select;
    ed.scene.clear_selection();
    frames(2);
    png = tile("crate.png");
    object_screen(b, &bx, &by);
    if (png) {
        drag(png->x + png->w * 0.5f, png->y + png->h * 0.5f, bx, by, 0, 20);
        CHECK(strstr(ed.scene.get(b)->texture_path, "crate.png") != nullptr, "crate.png dropped on B becomes its texture (\"%s\")",
              ed.scene.get(b)->texture_path);
    }
    const Editor::TileRect* mof = tile("ship.mof");
    if (mof) {
        int before = ed.scene.count();
        float gx = ed.view_rect[0] + ed.view_rect[2] * 0.5f, gy = ed.view_rect[1] + ed.view_rect[3] * 0.85f;
        drag(mof->x + mof->w * 0.5f, mof->y + mof->h * 0.5f, gx, gy, 0, 20);
        CHECK(ed.scene.count() == before + 1, "a .mof dropped on the ground makes a new object (%d -> %d)", before, ed.scene.count());
        Object* last = &ed.scene.at(ed.scene.count() - 1);
        CHECK(last->mesh == MeshKind::Model && strstr(last->model_path, "ship.mof"), "the new object uses ship.mof");
    }

    // ---- the floating tool pill switches tools
    ed.tool = Tool::Select;
    frames(2);
    CHECK(click_ui("tool:rotate"), "the tool pill has a rotate button");
    CHECK(ed.tool == Tool::Rotate, "clicking it selects the rotate tool");
    CHECK(click_ui("tool:move") && ed.tool == Tool::Move, "and the move button the move tool");
    {
        bool grid = ed.settings.show_grid;
        click_ui("tool:grid");
        CHECK(ed.settings.show_grid != grid, "the grid button toggles the grid");
        click_ui("tool:grid");
    }
    // World | Local; a click on the pill does not reach the view below.
    ed.scene.select_only(a);
    ed.gizmo_local = false;
    frames(1);
    CHECK(click_ui("tool:local") && ed.gizmo_local, "the Local button switches the gizmo to local axes");
    CHECK(ed.scene.selected_count() == 1, "the pill eats its clicks (selection kept)");
    CHECK(click_ui("tool:world") && !ed.gizmo_local, "the World button switches back");
    ev_key(platform::KEY_A + ('x' - 'a'), true); frames(1); ev_key(platform::KEY_A + ('x' - 'a'), false); frames(1);
    CHECK(ed.gizmo_local, "X toggles local axes");
    ev_key(platform::KEY_A + ('x' - 'a'), true); frames(1); ev_key(platform::KEY_A + ('x' - 'a'), false); frames(1);
    CHECK(!ed.gizmo_local, "X again goes back to world axes");

    // ---- local gizmo: an object turned 45 degrees about y moves along its own x
    {
        ed.tool = Tool::Move;
        ed.settings.snap = false;
        Object* oa2 = ed.scene.get(a);
        oa2->position = Vec3(-1.5f, 0.5f, 0);
        // Turned so that its x points towards the camera (ImGuizmo flips an
        // axis that points away to its other side).
        oa2->rotation = Quat::euler_deg(Vec3(0, -45, 0));
        ed.scene.select_only(a);
        ed.gizmo_local = true;
        frames(2);
        Vec3 local_x = rotate(oa2->rotation, Vec3(1, 0, 0));     // diagonal in x and z
        Vec3 p0 = oa2->position;
        float cx2, cy2, ax2, ay2;
        to_screen(p0, &cx2, &cy2);
        to_screen(p0 + local_x * 0.6f, &ax2, &ay2);
        float gx = cx2 + (ax2 - cx2) * 0.9f, gy = cy2 + (ay2 - cy2) * 0.9f;
        float dxs = ax2 - cx2, dys = ay2 - cy2;
        float len = sqrtf(dxs * dxs + dys * dys);
        drag(gx, gy, gx + dxs / len * 60, gy + dys / len * 60);
        Vec3 moved = oa2->position - p0;
        float ml = length(moved);
        float along = ml > 0 ? dot(moved, local_x) / ml : 0;
        CHECK(ml > 0.15f && along > 0.999f && fabsf(moved.y) < 1e-3f,
              "with local axes the X arrow moves A along its own x (moved %.3f, %.3f, %.3f; along %.4f)", moved.x, moved.y, moved.z, along);
        ed.gizmo_local = false;
        oa2->rotation = Quat();
        oa2->position = Vec3(-1.5f, 0.5f, 0);
        frames(2);
    }

    // ---- parenting: the tree, drag to parent, world transforms
    {
        ed.tool = Tool::Select;
        ed.scene.clear_selection();
        Object* oa2 = ed.scene.get(a);
        Object* ob2 = ed.scene.get(b);
        oa2->position = Vec3(-1.5f, 0.5f, 0);
        ob2->position = Vec3(1.5f, 0.5f, 0);
        frames(2);
        char ka[32], kb[32];
        snprintf(ka, sizeof(ka), "hier:%u", a);
        snprintf(kb, sizeof(kb), "hier:%u", b);
        const Editor::UiRect* rb = ui(kb);
        const Editor::UiRect* ra = ui(ka);
        CHECK(ra && rb, "rows for A and B");
        if (ra && rb) {
            // Drag B's row onto A's row: B becomes A's child, staying where it is.
            drag(rb->x + 60, rb->y + rb->h * 0.5f, ra->x + 60, ra->y + ra->h * 0.5f, 0, 10);
            CHECK(ob2->parent == a, "dragging B's row onto A makes B a child of A (parent %u)", ob2->parent);
            Mat4 wb = ed.scene.world_matrix(*ob2);
            CHECK(fabsf(wb.m[12] - 1.5f) < 1e-3f && fabsf(wb.m[14]) < 1e-3f, "B stays where it was in the world (%.3f, %.3f)", wb.m[12], wb.m[14]);
            CHECK(fabsf(ob2->position.x - 3.0f) < 1e-3f, "and its position is now relative to A (%.3f)", ob2->position.x);
            frames(2);
            ra = ui(ka);
            rb = ui(kb);
            CHECK(ra && rb && rb->y > ra->y, "B's row is under A's");
            char kf[32];
            snprintf(kf, sizeof(kf), "fold:%u", a);
            const Editor::UiRect* rf = ui(kf);
            CHECK(rf != nullptr, "A has a fold arrow");
            // Moving the parent moves the child.
            oa2->position.y += 1.0f;
            frames(1);
            wb = ed.scene.world_matrix(*ob2);
            CHECK(fabsf(wb.m[13] - 1.5f) < 1e-3f, "moving A moves B (world y %.3f)", wb.m[13]);
            oa2->position.y -= 1.0f;
            // Clicking where B is in the viewport picks B through its parent.
            frames(2);
            float sx2, sy2;
            to_screen(Vec3(1.5f, 0.5f, 0), &sx2, &sy2);
            click(sx2, sy2);
            CHECK(ed.scene.is_selected(b) && ed.scene.selected_count() == 1, "B can be picked in the viewport");
            // Folding hides the child row.
            if (rf) {
                click(rf->x + rf->w * 0.5f, rf->y + rf->h * 0.5f);
                CHECK(!oa2->expanded && ui(kb) == nullptr, "the fold arrow hides A's children");
                click(rf->x + rf->w * 0.5f, rf->y + rf->h * 0.5f);
                CHECK(oa2->expanded && ui(kb) != nullptr, "and shows them again");
            }
            // A cannot go under its own child.
            CHECK(!ed.scene.set_parent(a, b), "a parent cannot become its child's child");
            // Hiding A hides B in the viewport (it is not picked).
            oa2->visible = false;
            frames(2);
            ed.scene.clear_selection();
            click(sx2, sy2);
            CHECK(ed.scene.selected_count() == 0, "hiding A hides its child B");
            oa2->visible = true;
            // Dropping B's row on "Scene" takes it out again.
            frames(2);
            rb = ui(kb);
            const Editor::UiRect* rr = ui("hier:root");
            if (rb && rr) drag(rb->x + 80, rb->y + rb->h * 0.5f, rr->x + 60, rr->y + rr->h * 0.5f, 0, 10);
            CHECK(ob2->parent == kNoObject, "dropping B on Scene moves it to the top (parent %u)", ob2->parent);
            CHECK(fabsf(ob2->position.x - 1.5f) < 1e-3f, "keeping its world position (%.3f)", ob2->position.x);
            // Deleting a parent deletes its children; duplicating copies them.
            ed.scene.set_parent(b, a);
            ed.scene.select_only(a);
            int before = ed.scene.count();
            ed.duplicate_selection();
            CHECK(ed.scene.count() == before + 2, "duplicating A copies its child too (%d -> %d)", before, ed.scene.count());
            ObjectId a_copy = ed.scene.active();
            int kids = 0;
            for (int i = 0; i < ed.scene.count(); i++) if (ed.scene.at(i).parent == a_copy) kids++;
            CHECK(kids == 1, "the copy has one child (%d)", kids);
            ed.delete_selection();
            CHECK(ed.scene.count() == before, "deleting the copy deletes its child (%d)", ed.scene.count());
            ed.scene.set_parent(b, kNoObject);
            ed.scene.clear_selection();
            frames(2);
        }
    }

    // ---- orientation gizmo: clicking an axis looks along it
    frames(2);
    CHECK(ui("axis:+y") && ui("axis:+x") && ui("axis:-z"), "the orientation gizmo shows its axes");
    {
        const Editor::UiRect* r = ui("axis:+x");
        if (r) {
            float gx = r->x + r->w * 0.5f, gy = r->y + r->h * 0.5f;
            ev_move(gx, gy); frames(2);
            click(gx, gy);
            CHECK(ed.camera.yaw > 89.0f && ed.camera.yaw < 91.0f && ed.camera.pitch > -1 && ed.camera.pitch < 1,
                  "clicking +X views from the right (yaw %.1f pitch %.1f)", ed.camera.yaw, ed.camera.pitch);
        }
        frames(1);
        r = ui("axis:+y");
        if (r) {
            float gx = r->x + r->w * 0.5f, gy = r->y + r->h * 0.5f;
            ev_move(gx, gy); frames(2);
            click(gx, gy);
            CHECK(ed.camera.pitch > 89.0f, "clicking +Y views from the top (pitch %.1f)", ed.camera.pitch);
        }
        ed.camera.yaw = 0;
        ed.camera.pitch = 20;
        frames(2);
    }

    // ---- view menu
    if (click_ui("view:menu")) {
        frames(1);
        CHECK(click_ui("menu:Right"), "the view menu lists Right");
        CHECK(ed.camera.yaw > 89.0f && ed.camera.yaw < 91.0f, "View > Right looks from +X (yaw %.1f)", ed.camera.yaw);
        ed.camera.yaw = 0;
        ed.camera.pitch = 20;
        frames(2);
    } else {
        CHECK(false, "the viewport has a view menu");
    }

    // ---- renderer chip: display modes
    if (click_ui("run:renderer")) {
        frames(1);
        CHECK(click_ui("menu:Wireframe") && ed.view_mode == ViewMode::Wireframe, "the renderer menu switches to wireframe");
        frames(2);
        CHECK(ed.view_triangles > 0, "wireframe still draws the scene (%d triangles)", ed.view_triangles);
        click(ed.view_rect[0] + 300, ed.view_rect[1] + 10);    // closes the popup (lands on the view: clears selection)
        ed.view_mode = ViewMode::Lit;
        frames(2);
    } else {
        CHECK(false, "the top bar has a renderer chip");
    }

    // ---- play mode: Play saves the scene, Stop restores it exactly
    {
        ed.scene.select_only(a);
        ed.scene.toggle(b);
        int count0 = ed.scene.count();
        Vec3 pos_a = ed.scene.get(a)->position;
        ObjectId active0 = ed.scene.active();
        frames(1);
        CHECK(click_ui("run:play") && ed.playing(), "Play starts play mode");
        frames(30);
        CHECK(ed.play_time > 0.3, "play time runs (%.2f s)", ed.play_time);
        // Change things while playing.
        ed.scene.get(a)->position = Vec3(5, 5, 5);
        ed.delete_selection();
        ObjectId c = ed.create_object("C", MeshKind::Sphere, Vec3(0, 1, 0));
        frames(2);
        CHECK(ed.scene.count() == count0 - 1, "objects can change while playing (%d)", ed.scene.count());
        CHECK(click_ui("run:stop") && !ed.playing(), "Stop ends play mode");
        CHECK(ed.scene.count() == count0, "Stop brings back the deleted objects (%d)", ed.scene.count());
        CHECK(ed.scene.get(a) && ed.scene.get(a)->position.x == pos_a.x && ed.scene.get(a)->position.y == pos_a.y,
              "and A's position");
        CHECK(ed.scene.get(c) == nullptr, "and removes what was made while playing");
        CHECK(ed.scene.selected_count() == 2 && ed.scene.active() == active0, "and the selection (count %d, active %u)",
              ed.scene.selected_count(), ed.scene.active());
        ObjectId d = ed.create_object("D", MeshKind::Cube, Vec3(0, 0.5f, 3));
        CHECK(d != c && d > c, "ids are not reused after Stop (%u after %u)", d, c);
        ed.scene.remove(d);
        // F5 toggles too.
        ev_key(platform::KEY_F5, true); frames(1); ev_key(platform::KEY_F5, false); frames(1);
        CHECK(ed.playing(), "F5 starts play mode");
        ev_key(platform::KEY_F5, true); frames(1); ev_key(platform::KEY_F5, false); frames(1);
        CHECK(!ed.playing(), "F5 again stops it");
    }

    // ---- console chips filter by level
    {
        ImGuiWindow* cw = ImGui::FindWindowByName("###Console");
        if (cw && cw->DockNode && cw->DockNode->TabBar) {
            ImGuiTabItem* t = ImGui::TabBarFindTabByID(cw->DockNode->TabBar, cw->TabId);
            if (t) click(cw->DockNode->TabBar->BarRect.Min.x + t->Offset + t->Width * 0.5f, cw->DockNode->TabBar->BarRect.GetCenter().y);
        }
        frames(2);
        CHECK(ui("console:warnings") != nullptr, "the console shows level chips");
        CHECK(click_ui("console:clear") && ed.log_count == 0, "the trash button clears the console");
    }

    // ---- scripts: declared inputs (parsed without running Lua)
    {
        const char* src =
            "-- A door.\n"
            "-- @prop speed  number = 5  range 0 20   -- how fast\n"
            "--- @prop label text = \"Front door\"\n"
            "-- @prop note   text = plain words here\n"
            "-- @prop open   toggle = true\n"
            "-- @prop mode   list(closed, open, locked) = locked\n"
            "-- @prop target object\n"
            "-- @prop skin file = \"assets/door.png\"\n"
            "-- @prop big    number = 50 range 0 10\n"
            "-- @prop bad    vector = 1\n"
            "-- @prop worse  list() \n"
            "-- @prop speed  number = 1\n"
            "local x = 1 -- @prop not_a_decl number\n";
        ScriptProp props[kMaxProps];
        int bad = 0;
        int n = parse_script_props(src, (uint32_t)strlen(src), props, kMaxProps, &bad);
        CHECK(n == 8, "8 inputs declared (got %d)", n);
        CHECK(bad == 3, "3 bad lines: unknown type, empty list, a repeated name (got %d)", bad);
        if (n == 8) {
            CHECK(!strcmp(props[0].name, "speed") && props[0].type == PropType::Number && props[0].number == 5 && props[0].min == 0 && props[0].max == 20,
                  "number with a range (%s %g [%g %g])", props[0].name, props[0].number, props[0].min, props[0].max);
            CHECK(props[1].type == PropType::Text && !strcmp(props[1].text, "Front door"), "quoted text (\"%s\")", props[1].text);
            CHECK(!strcmp(props[2].text, "plain words here"), "bare text is the rest of the line (\"%s\")", props[2].text);
            CHECK(props[3].type == PropType::Toggle && props[3].toggle, "toggle = true");
            CHECK(props[4].type == PropType::List && props[4].choice_count == 3 && props[4].choice == 2 && !strcmp(props[4].choices[2], "locked"),
                  "list with a default choice (%d choices, choice %d)", props[4].choice_count, props[4].choice);
            CHECK(props[5].type == PropType::Object && props[5].object == 0, "object link, empty");
            CHECK(!strcmp(props[6].file, "assets/door.png"), "file input default (%s)", props[6].file);
            CHECK(props[7].number == 10, "a default outside the range is clamped (%g)", props[7].number);
        }

        // Merging after the file changed: kept values, new defaults, dropped inputs.
        ScriptSlot slot;
        merge_script_props(&slot, props, n);
        slot.prop("speed")->number = 12;
        slot.prop("mode")->choice = 1;                 // "open"
        snprintf(slot.prop("label")->text, sizeof(slot.prop("label")->text), "%s", "Back door");
        snprintf(slot.prop("skin")->file, sizeof(slot.prop("skin")->file), "%s", "assets/blue.png");
        const char* src2 =
            "-- @prop mode  list(locked, open) = locked\n"
            "-- @prop speed number = 1 range 0 10\n"
            "-- @prop label toggle = false\n"
            "-- @prop extra number = 3\n"
            "-- @prop skin file\n";
        ScriptProp p2[kMaxProps];
        int n2 = parse_script_props(src2, (uint32_t)strlen(src2), p2, kMaxProps);
        merge_script_props(&slot, p2, n2);
        CHECK(slot.prop_count == 5 && !slot.prop("note") && !slot.prop("target"), "inputs no longer declared are dropped (%d)", slot.prop_count);
        CHECK(slot.prop("speed")->number == 10, "a kept number is clamped to the new range (%g)", slot.prop("speed")->number);
        CHECK(slot.prop("mode")->choice == 1, "a list choice is found again by its text (%d)", slot.prop("mode")->choice);
        CHECK(slot.prop("label")->type == PropType::Toggle && !slot.prop("label")->toggle, "an input whose type changed starts from its default");
        CHECK(slot.prop("extra")->number == 3, "a new input gets its default");
        CHECK(!strcmp(slot.prop("skin")->file, "assets/blue.png"), "a file input keeps its path on reload");
        CHECK(!strcmp(slot.props[0].name, "mode"), "inputs follow the file's order");
    }

    // ---- scripts on objects: several per object, linked to each other
    {
        const char* door1 = "-- @prop speed number = 2 range 0 5\n-- @prop locked toggle = true\n";
        platform::write_file("build/host/test-project/door.lua", door1, strlen(door1));
        const char* sw = "-- @prop door object\n-- @prop mode list(press, hold) = press\n-- @prop label text = \"Switch\"\n";
        platform::write_file("build/host/test-project/switch.lua", sw, strlen(sw));
        ObjectId door = ed.create_object("Door", MeshKind::Cube, Vec3(0, 0.5f, -3));
        ObjectId lever = ed.create_object("Lever", MeshKind::Cylinder, Vec3(1, 0.5f, -3));
        ObjectId knob = ed.create_child("Knob", MeshKind::Sphere, lever);
        ed.apply_file_to_object("build/host/test-project/door.lua", door);
        ed.apply_file_to_object("build/host/test-project/player.lua", door);
        Object* od = ed.scene.get(door);
        CHECK(od->script_count == 2, "dropping a second script adds it (count %d)", od->script_count);
        CHECK(od->scripts[0].prop_count == 2 && od->scripts[0].prop("locked")->toggle, "door.lua's inputs are read when it is added");
        CHECK(od->scripts[1].prop_count == 0 && !od->scripts[1].missing, "a script without @prop lines has no inputs");
        CHECK(ed.scene.find_script(door, "door") == &od->scripts[0] && ed.scene.find_script(door, "door.lua") == &od->scripts[0],
              "find_script by name, with or without the extension");
        CHECK(ed.scene.find_script(door, "do") == nullptr && ed.scene.find_script(door, "switch") == nullptr, "find_script: no partial or absent names");

        ed.add_script(lever, "build/host/test-project/switch.lua");
        ed.add_script(knob, "build/host/test-project/switch.lua");
        ScriptProp* link = ed.scene.script_prop(lever, "switch", "door");
        CHECK(link && link->type == PropType::Object, "the lever's switch has a door link");
        if (link) link->object = door;
        // The knob's switch links to the lever, inside the tree that is duplicated below.
        if (ScriptProp* k = ed.scene.script_prop(knob, "switch", "door")) k->object = lever;
        // What a switch script does at run time: follow the link to the door's script and use its inputs.
        ScriptProp* through = link ? ed.scene.script_prop(link->object, "door", "locked") : nullptr;
        CHECK(through && through->toggle, "a script reaches another object's script through a link");
        CHECK(ed.scene.links_to(door) == 1 && ed.scene.links_to(lever) == 1, "links_to counts links (%d, %d)", ed.scene.links_to(door), ed.scene.links_to(lever));

        // Duplicating the lever (with its knob): the copy's links inside the
        // copied tree go to the copies, links outside stay.
        ed.scene.select_only(lever);
        int before = ed.scene.count();
        ed.duplicate_selection();
        CHECK(ed.scene.count() == before + 2, "lever and knob duplicated");
        ObjectId lever2 = ed.scene.active();
        ObjectId knob2 = kNoObject;
        for (int i = 0; i < ed.scene.count(); i++) if (ed.scene.at(i).parent == lever2) knob2 = ed.scene.at(i).id;
        ScriptProp* l2 = ed.scene.script_prop(lever2, "switch", "door");
        ScriptProp* k2 = ed.scene.script_prop(knob2, "switch", "door");
        CHECK(l2 && l2->object == door, "a link out of the copied tree stays (%u)", l2 ? l2->object : 0);
        CHECK(k2 && k2->object == lever2, "a link inside the copied tree goes to the copy (%u, copy %u)", k2 ? k2->object : 0, lever2);

        // Play keeps editor values and puts back what play changed.
        ed.scene.script_prop(door, "door", "speed")->number = 4;
        ed.play();
        ed.scene.script_prop(door, "door", "speed")->number = 1;
        ed.stop();
        CHECK(ed.scene.script_prop(door, "door", "speed")->number == 4, "Stop puts script inputs back (%g)", ed.scene.script_prop(door, "door", "speed")->number);

        // The file changes: Reload keeps the value and adds the new input.
        const char* door2 = "-- @prop speed number = 2 range 0 5\n-- @prop locked toggle = true\n-- @prop sound text = creak\n";
        platform::write_file("build/host/test-project/door.lua", door2, strlen(door2));
        CHECK(ed.reload_script(door, 0), "reload reads the file again");
        od = ed.scene.get(door);
        CHECK(od->scripts[0].prop_count == 3 && od->scripts[0].prop("speed")->number == 4 && !strcmp(od->scripts[0].prop("sound")->text, "creak"),
              "reload keeps values and adds new inputs");

        // Deleting the door clears the links to it.
        ed.scene.select_only(door);
        ed.delete_selection();
        CHECK(ed.scene.script_prop(lever, "switch", "door")->object == kNoObject && ed.scene.links_to(door) == 0, "links to a deleted object are cleared");

        // ---- the Inspector: one section per script, inputs, add and remove
        ed.scene.select_only(lever);
        // Scroll the Inspector down so the scripts are in view.
        frames(2);
        ImGuiWindow* iw = ImGui::FindWindowByName("###Inspector");
        if (iw) { ImGui::SetScrollY(iw, iw->ScrollMax.y); }
        frames(2);
        if (iw) { ImGui::SetScrollY(iw, iw->ScrollMax.y); }
        frames(2);
        CHECK(ui("script:0") && ui("script:0:mode") && ui("script:0:label") && ui("script:0:door"), "the Inspector shows the switch and its inputs");
        CHECK(ui("inspector:add_script") != nullptr, "an Add script button");
        if (ui("inspector:add_script")) {
            click_ui("inspector:add_script");
            frames(2);
            CHECK(ui("addscript:door.lua") != nullptr, "Add script lists the project's scripts");
            if (ui("addscript:door.lua")) click_ui("addscript:door.lua");
            Object* ol = ed.scene.get(lever);
            CHECK(ol->script_count == 2 && ol->scripts[1].name_is("door") && ol->scripts[1].prop_count == 3, "picking one adds it with its inputs (count %d)",
                  ol->script_count);
            if (iw) { frames(1); ImGui::SetScrollY(iw, iw->ScrollMax.y); frames(2); }
            CHECK(click_ui("script:1:up") && ed.scene.get(lever)->scripts[0].name_is("door"), "the up button moves a script earlier");
            if (iw) { frames(1); ImGui::SetScrollY(iw, iw->ScrollMax.y); frames(2); }
            CHECK(click_ui("script:0:remove") && ed.scene.get(lever)->script_count == 1 && ed.scene.get(lever)->scripts[0].name_is("switch"),
                  "the trash button removes a script");
            // Add script > New file: the search text names a new Lua script.
            if (iw) { frames(1); ImGui::SetScrollY(iw, iw->ScrollMax.y); frames(2); }
            click_ui("inspector:add_script");
            frames(2);
            type_text("spinner");
            frames(2);
            CHECK(ui("addscript:new") != nullptr, "Add script ends with New file");
            click_ui("addscript:new");
            frames(2);
            CHECK(platform::file_exists("build/host/test-project/scripts/spinner.lua"), "New file writes scripts/spinner.lua");
            Object* ol2 = ed.scene.get(lever);
            CHECK(ol2->script_count == 2 && ol2->scripts[1].name_is("spinner"), "and adds it to the object (count %d)", ol2->script_count);
        }
        // Filling the slots: at most kMaxScripts.
        for (int i = 0; i < kMaxScripts + 2; i++) ed.scene.add_script(knob, "build/host/test-project/player.lua");
        CHECK(ed.scene.get(knob)->script_count == kMaxScripts, "an object holds at most %d scripts", kMaxScripts);
        ed.scene.select_only(kNoObject);
        frames(2);
    }

    // ---- modules and special objects, the one-current rule
    {
        CHECK(module_by_name("camera") == ModuleKind::Camera && module_by_name("rigidbody3d") == ModuleKind::RigidBody3D &&
              module_by_name("nope") == ModuleKind::Count, "modules are found by their Lua names");
        int with_current = 0;
        for (int k = 0; k < kModuleKindCount; k++) {
            const ModuleInfo& mi = module_info((ModuleKind)k);
            with_current += mi.has_current;
            const FieldInfo* cf = find_field(mi, "current");
            CHECK(mi.has_current == (cf != nullptr), "%s: has_current matches a 'current' field", mi.name);
            if (cf) CHECK(cf->offset == mi.current_offset && cf->type == FieldType::Bool, "%s: current offset", mi.name);
        }
        CHECK(with_current == 6, "camera, sky, sky light, post processing, clouds and listener have Current (%d)", with_current);

        int objects0 = ed.scene.count();
        ObjectId c1 = ed.create_kind("Cam A", ObjectKind::Camera, Vec3(0, 2, 5));
        CHECK(ed.scene.get(c1)->kind == ObjectKind::Camera && ed.scene.get(c1)->modules.has(ModuleKind::Camera), "a camera object has camera settings");
        CHECK(ed.scene.current(ModuleKind::Camera) == c1, "the first camera becomes current");
        ObjectId c2 = ed.create_kind("Cam B", ObjectKind::Camera, Vec3(3, 2, 5));
        CHECK(ed.scene.current(ModuleKind::Camera) == c1 && !ed.scene.is_current(c2, ModuleKind::Camera), "a second camera is not current");
        ed.scene.set_current(c2, ModuleKind::Camera);
        CHECK(ed.scene.is_current(c2, ModuleKind::Camera) && !ed.scene.get(c1)->modules.camera.current, "setting B current clears A");
        ed.scene.clear_current(c2, ModuleKind::Camera);
        CHECK(ed.scene.current(ModuleKind::Camera) == kNoObject, "clearing leaves none current");
        ed.scene.set_current(c1, ModuleKind::Camera);
        // Turned on by hand in the data (as a script or a loaded file may do), fix_current keeps one.
        ed.scene.get(c2)->modules.camera.current = true;
        ed.scene.fix_current();
        int cur = ed.scene.get(c1)->modules.camera.current + ed.scene.get(c2)->modules.camera.current;
        CHECK(cur == 1, "fix_current leaves exactly one current (%d)", cur);
        ed.scene.set_current(c1, ModuleKind::Camera);
        // Duplicate: the copy is not current.
        ed.scene.select_only(c1);
        ed.duplicate_selection();
        ObjectId c3 = ed.scene.active();
        CHECK(c3 != c1 && ed.scene.get(c3)->kind == ObjectKind::Camera && !ed.scene.get(c3)->modules.camera.current &&
              ed.scene.current(ModuleKind::Camera) == c1, "a duplicated camera is not current");
        // Play/Stop: the current camera comes back.
        ed.play();
        ed.scene.set_current(c3, ModuleKind::Camera);
        ed.stop();
        CHECK(ed.scene.current(ModuleKind::Camera) == c1, "Stop puts the current camera back");
        // Kinds without the flag ignore it; set_current needs the module.
        ObjectId e = ed.create_kind("Holder", ObjectKind::Empty, Vec3(0, 1, 0));
        ed.scene.set_current(e, ModuleKind::Camera);
        CHECK(ed.scene.current(ModuleKind::Camera) == c1, "set_current on an object without a camera does nothing");
        CHECK(ed.scene.add_module(e, ModuleKind::RigidBody3D) && !ed.scene.add_module(e, ModuleKind::RigidBody3D), "one module of a kind per object");
        CHECK(!ed.scene.add_module(e, ModuleKind::Camera), "camera settings are not a module");
        ed.scene.set_current(e, ModuleKind::RigidBody3D);
        CHECK(ed.scene.current(ModuleKind::RigidBody3D) == kNoObject, "a rigidbody has no Current");
        // A listener is a module with Current: the first one added is current.
        ed.scene.add_module(c1, ModuleKind::AudioListener);
        ed.scene.add_module(e, ModuleKind::AudioListener);
        CHECK(ed.scene.current(ModuleKind::AudioListener) == c1, "the first listener is current");
        // Deleting the current camera leaves none (the others stay as they were).
        ed.scene.select_only(c1);
        ed.delete_selection();
        CHECK(ed.scene.current(ModuleKind::Camera) == kNoObject && ed.scene.current(ModuleKind::AudioListener) == kNoObject,
              "deleting the current camera leaves no current camera or listener");
        // Lights are empties with a Light module.
        ObjectId l = ed.create_light(LightType::Spot, Vec3(0, 3, 0));
        CHECK(ed.scene.get(l)->kind == ObjectKind::Empty && ed.scene.get(l)->modules.has(ModuleKind::Light) &&
              ed.scene.get(l)->modules.light.type == LightType::Spot, "a spot light");
        // Special objects are picked around their origin.
        ed.scene.clear_selection();
        frames(2);
        ed.camera.target = Vec3(0, 0.5f, 0); ed.camera.yaw = 0; ed.camera.pitch = 20; ed.camera.distance = 14;
        ed.tool = Tool::Select;
        frames(2);
        char vk[32];
        snprintf(vk, sizeof(vk), "vicon:%u", c2);
        CHECK(ui(vk) != nullptr, "the camera has an icon in the viewport");
        CHECK(click_ui(vk) && ed.scene.is_selected(c2), "clicking a camera's icon selects it (selected %u)", ed.scene.active());

        // ---- the Inspector: Current switch, Add module, remove
        ed.scene.select_only(c2);
        frames(3);
        CHECK(ui("module:camera") && ui("module:camera:current") && ui("module:camera:fov"), "the Inspector shows the camera's settings");
        CHECK(click_ui("module:camera:current") && ed.scene.current(ModuleKind::Camera) == c2, "the Current switch makes it current");
        CHECK(click_ui("module:camera:current") && ed.scene.current(ModuleKind::Camera) == kNoObject, "and off again");
        click_ui("module:camera:current");
        ed.scene.select_only(c3);
        frames(3);
        CHECK(click_ui("module:camera:current") && ed.scene.current(ModuleKind::Camera) == c3 && !ed.scene.is_current(c2, ModuleKind::Camera),
              "switching another camera on turns the first off");
        CHECK(!ui("module:camera:ortho_size"), "perspective cameras don't show the ortho size");
        CHECK(click_ui("inspector:add_module"), "an Add module button");
        frames(2);
        CHECK(click_ui("addmodule:light") && ed.scene.get(c3)->modules.has(ModuleKind::Light), "Add module > Light adds a light");
        frames(3);
        ImGuiWindow* iw2 = ImGui::FindWindowByName("###Inspector");
        if (iw2) { ImGui::SetScrollY(iw2, iw2->ScrollMax.y); frames(2); }
        CHECK(ui("module:light:intensity") && !ui("module:light:spot_angle"), "a point light shows intensity, not the spot angle");
        CHECK(click_ui("module:light:remove") && !ed.scene.get(c3)->modules.has(ModuleKind::Light), "the trash button removes the module");
        CHECK(click_ui("inspector:add_module"), "Add module opens for a character controller");
        frames(2);
        CHECK(click_ui("addmodule:character_controller3d") &&
              ed.scene.get(c3)->modules.has(ModuleKind::CharacterController3D), "Add module adds Character Controller 3D");
        frames(2);
        if (iw2) { ImGui::SetScrollY(iw2, iw2->ScrollMax.y); frames(2); }
        CHECK(ui("module:character_controller3d:radius") && ui("module:character_controller3d:grounded"),
              "the Inspector exposes the controller settings and grounded state");
        CHECK(click_ui("module:character_controller3d:remove") &&
              !ed.scene.get(c3)->modules.has(ModuleKind::CharacterController3D), "the controller can be removed");

        // Clean up so the rest of the tests see the old scene.
        while (ed.scene.count() > objects0) ed.scene.remove(ed.scene.at(ed.scene.count() - 1).id);
        ed.scene.clear_selection();
        frames(2);
    }

    // ---- Play runs the scene as an ECS world
    {
        int objects0 = ed.scene.count();
        ObjectId cam = ed.create_kind("Play Cam", ObjectKind::Camera, Vec3(1, 2, 3));
        ObjectId cam_b = ed.create_kind("Play Cam B", ObjectKind::Camera, Vec3(2, 2, 3));
        ed.scene.set_current(cam_b, ModuleKind::Camera);
        ObjectId root = ed.create_object("Rig", MeshKind::Cube, Vec3(1, 0, 0));
        snprintf(ed.scene.get(root)->texture_path, sizeof(ed.scene.get(root)->texture_path), "%s", "assets/start.png");
        snprintf(ed.scene.get(root)->shader_path, sizeof(ed.scene.get(root)->shader_path), "%s", "shaders/start.fshader");
        ObjectId kid = ed.create_child("Rig Part", MeshKind::Sphere, root);
        ed.scene.get(root)->rotation = Quat::euler_deg(Vec3(0, 90, 0));
        ed.scene.add_module(kid, ModuleKind::Light);
        ed.scene.get(kid)->modules.light.intensity = 3.5f;
        CHECK(ed.play_world() == nullptr, "no world while editing");
        ed.play();
        World* w = ed.play_world();
        CHECK(w != nullptr, "Play builds a world");
        if (w) {
            uint64_t ek = ed.entity_of(kid), er = ed.entity_of(root);
            CHECK(ek && er && w->parent(ek) == er, "the parent/child tree is in the world");
            const WorldTransform* wt = w->world_transform(ek);
            Mat4 sm = ed.scene.world_matrix(*ed.scene.get(kid));
            bool same = wt != nullptr;
            for (int i = 0; same && i < 16; i++) same = fabsf(wt->matrix.m[i] - sm.m[i]) < 1e-4f;
            CHECK(same, "world transforms match the scene's");
            const Light* l = w->has(ek, ModuleKind::Light) ? (const Light*)w->get(ek, ModuleKind::Light) : nullptr;
            CHECK(l && l->intensity == 3.5f, "modules are components");
            CHECK(w->current(ModuleKind::Camera) == ed.entity_of(cam_b), "the current camera is current in the world");
            CHECK(w->renderable(er) && !w->renderable(ed.entity_of(cam)), "mesh objects are renderable, cameras not");
            Renderable* renderable = w->renderable(er);
            CHECK(renderable && !strcmp(renderable->texture_path, "assets/start.png") &&
                  !strcmp(renderable->shader_path, "shaders/start.fshader"), "asset paths reach the play world");
            if (renderable) snprintf(renderable->texture_path, sizeof(renderable->texture_path), "%s", "assets/runtime.png");
            // A system moving an entity moves the object in the editor.
            Transform* t = w->transform(er);
            if (t) t->position = Vec3(5, 0, 0);
            frames(2);
            CHECK(ed.scene.get(root)->position.x == 5.0f &&
                  !strcmp(ed.scene.get(root)->texture_path, "assets/runtime.png"), "transforms and material paths come back to the scene each frame");
        }
        ed.stop();
        CHECK(ed.play_world() == nullptr && ed.scene.get(root)->position.x == 1.0f &&
              !strcmp(ed.scene.get(root)->texture_path, "assets/start.png") &&
              !strcmp(ed.scene.get(root)->shader_path, "shaders/start.fshader"), "Stop drops the world and restores the scene");
        while (ed.scene.count() > objects0) ed.scene.remove(ed.scene.at(ed.scene.count() - 1).id);
        frames(2);
    }

    // ---- the FarcadeGL renderer in the viewport, and Play through the current camera
    {
        int objects0 = ed.scene.count();
        for (int i = 0; i < ed.scene.count(); i++) {
            ed.scene.at(i).texture_path[0] = 0;
            ed.scene.at(i).shader_path[0] = 0;
        }
        ed.scene.clear_selection();
        frames(2);
        CHECK(click_ui("run:renderer"), "the renderer menu opens");
        frames(2);
        CHECK(click_ui("menu:FarcadeGL") && ed.view_renderer == Editor::ViewRenderer::FarcadeGL, "the menu switches to FarcadeGL");
        ev_key(platform::KEY_ESCAPE, true); frames(1); ev_key(platform::KEY_ESCAPE, false); frames(3);
        int errors = 0;
        for (int i = 0; i < ed.log_count; i++) errors += ed.log_lines[i].level == LogLevel::Error && strstr(ed.log_lines[i].text, "FarcadeGL");
        CHECK(ed.view_renderer == Editor::ViewRenderer::FarcadeGL && errors == 0, "FarcadeGL renders without errors (%d)", errors);
        CHECK(ed.view_triangles > 0, "FarcadeGL drew triangles (%d)", ed.view_triangles);
        // Settings > Graphics: the GPU tier is off on the CPU backend unless forced.
        CHECK(ed.fgl_backend[0] && !ed.fgl_features.gpu && !ed.fgl_features.gpu_tier, "on the CPU backend the GPU tier is off (%s)", ed.fgl_backend);
        ed.settings.graphics.allow_gpu_effects_on_cpu = true;
        frames(2);
        CHECK(ed.fgl_features.gpu_tier && ed.fgl_features.shadows && ed.fgl_features.pbr, "forcing it turns the GPU effects on");
        ed.settings.graphics.shadows = false;
        frames(2);
        CHECK(!ed.fgl_features.shadows, "a GPU effect's switch turns it off");
        // New shaders are compiled over several frames with a bar: a tiny
        // budget makes every frame compile one.
        {
            ed.fgl_compile_budget = 1e-6;
            ed.settings.graphics.shadows = true;            // needs shaders again? not if kept: force new ones
            ed.settings.graphics.ssr = true;
            ed.settings.graphics.ao_mode = gfx::AOMode::GTAO;
            frames(1);
            bool started = ed.fgl_compiling;
            int seen = 0, guard = 0;
            while (ed.fgl_compiling && guard++ < 100) { seen = ed.fgl_compiled; frames(1); }
            CHECK(started && seen >= 1 && !ed.fgl_compiling, "new shaders compile over frames (%d, %d)", started, seen);
            int errs = 0;
            for (int i = 0; i < ed.log_count; i++) errs += ed.log_lines[i].level == LogLevel::Error && strstr(ed.log_lines[i].text, "FarcadeGL");
            CHECK(errs == 0 && ed.view_renderer == Editor::ViewRenderer::FarcadeGL, "compiling over frames is no error");
            CHECK(ed.fgl_shader_expect[1] >= seen, "the bar remembers how many it took");
            ed.fgl_compile_budget = 25.0;
            ed.settings.graphics.ssr = false;
            ed.settings.graphics.ao_mode = gfx::AOMode::SSAO;
            ed.settings.graphics.shadows = false;
            frames(2);
        }
        // Materials: with PBR on, making every mesh metal changes the picture.
        {
            auto px = [&](float fx, float fy) {
                Framebuffer& fb = platform::framebuffer();
                int x = (int)(ed.view_rect[0] + ed.view_rect[2] * fx), y = (int)(ed.view_rect[1] + ed.view_rect[3] * fy);
                return fb.pixels[y * fb.pitch + x];
            };
            const float spots[3][2] = { { 0.5f, 0.5f }, { 0.4f, 0.6f }, { 0.6f, 0.7f } };
            ObjectId ball = ed.create_object("Metal Ball", MeshKind::Sphere, ed.camera.target);
            ed.scene.get(ball)->scale = Vec3(3, 3, 3);
            ed.scene.get(ball)->color = Vec3(0.9f, 0.6f, 0.2f);
            ObjectId sun = ed.create_light(LightType::Directional, Vec3(0, 4, 0));
            ed.scene.get(sun)->rotation = Quat::euler_deg(Vec3(-50, 30, 0));
            ed.scene.clear_selection();
            frames(8);
            uint32_t was[3];
            for (int i = 0; i < 3; i++) was[i] = px(spots[i][0], spots[i][1]);
            uint64_t h0 = ed.view_hash(100, 100);
            for (int i = 0; i < ed.scene.count(); i++) { ed.scene.at(i).metallic = 1.0f; ed.scene.at(i).roughness = 0.15f; }
            CHECK(ed.view_hash(100, 100) != h0, "the material is part of the viewport hash");
            frames(8);
            int changed = 0;
            for (int i = 0; i < 3; i++) changed += px(spots[i][0], spots[i][1]) != was[i];
            CHECK(changed > 0, "metal and roughness change the GPU tier picture (%d of 3 spots)", changed);
            for (int i = 0; i < ed.scene.count(); i++) { ed.scene.at(i).metallic = 0.0f; ed.scene.at(i).roughness = 0.6f; }
            ed.scene.remove(ball);
            ed.scene.remove(sun);
            frames(2);
        }
        ed.settings.graphics.apply_preset(gfx::GraphicsPreset::High);
        ed.settings.graphics.allow_gpu_effects_on_cpu = false;
        frames(2);
        // A sky changes the picture: read a viewport pixel before and after.
        auto pixel = [&](float fx, float fy) {
            Framebuffer& fb = platform::framebuffer();
            int x = (int)(ed.view_rect[0] + ed.view_rect[2] * fx), y = (int)(ed.view_rect[1] + ed.view_rect[3] * fy);
            return fb.pixels[y * fb.pitch + x];
        };
        uint32_t before = pixel(0.5f, 0.12f);
        ObjectId sky = ed.create_kind("Test Sky", ObjectKind::Sky, Vec3(0, 3, 0));
        ed.scene.set_current(sky, ModuleKind::Sky);
        ed.scene.get(sky)->modules.sky.mode = SkyMode::Color;
        ed.scene.get(sky)->modules.sky.zenith = Vec3(1.0f, 0.0f, 0.0f);
        ed.scene.clear_selection();
        frames(3);
        uint32_t after = pixel(0.5f, 0.12f);
        CHECK(before != after && ((after >> 16) & 0xFF) > ((after >> 8) & 0xFF) + 40, "a red sky turns the background red (%08X -> %08X)", before, after);
        // Play through the current camera.
        ObjectId cam = ed.create_kind("Look Cam", ObjectKind::Camera, Vec3(0, 1.5f, 6));
        ed.scene.set_current(cam, ModuleKind::Camera);
        ed.scene.clear_selection();
        ed.play();
        frames(3);
        CHECK(ed.looking_through_camera, "Play looks through the current camera");
        ed.settings.play_through_camera = false;
        frames(2);
        CHECK(!ed.looking_through_camera, "unless that is switched off");
        ed.settings.play_through_camera = true;
        ed.stop();
        frames(2);
        CHECK(!ed.looking_through_camera, "editing uses the editor camera");
        ed.view_renderer = Editor::ViewRenderer::CpuPreview;
        while (ed.scene.count() > objects0) ed.scene.remove(ed.scene.at(ed.scene.count() - 1).id);
        frames(2);
    }

    // ---- physics in Play: a ball falls onto the ground, through a trigger
    {
        int objects0 = ed.scene.count();
        ObjectId floor_ = ed.create_object("Phys Floor", MeshKind::Plane, Vec3(20, 0, 0));
        ed.scene.get(floor_)->scale = Vec3(4, 1, 4);
        ed.scene.add_module(floor_, ModuleKind::Collider3D);
        CHECK(ed.scene.get(floor_)->modules.collider3d.size.y < 0.2f, "a plane's collider starts thin");
        ObjectId ball = ed.create_object("Phys Ball", MeshKind::Sphere, Vec3(20, 3, 0));
        ed.scene.add_module(ball, ModuleKind::RigidBody3D);
        ed.scene.add_module(ball, ModuleKind::Collider3D);
        CHECK(ed.scene.get(ball)->modules.collider3d.shape == ColliderShape::Sphere, "a sphere's collider starts as a sphere");
        ObjectId zone = ed.create_kind("Phys Zone", ObjectKind::Empty, Vec3(20, 1.5f, 0));
        ed.scene.add_module(zone, ModuleKind::Collider3D);
        ed.scene.get(zone)->modules.collider3d.mode = ColliderMode::Trigger;
        int events0 = ed.physics_event_count;
        ed.play();
        CHECK(ed.play_physics() != nullptr, "Play starts physics");
        frames(150);                                 // 2.5 s at 60 fps
        float y = ed.scene.get(ball)->position.y;
        CHECK(y > 0.4f && y < 0.6f, "the ball fell and rests on the floor (y %.3f)", y);
        CHECK(ed.physics_event_count >= events0 + 3, "trigger enter, exit and the hit were reported (%d)", ed.physics_event_count - events0);
        bool logged = false;
        for (int i = 0; i < ed.log_count; i++) logged |= strstr(ed.log_lines[i].text, "Phys Ball entered trigger Phys Zone") != nullptr;
        CHECK(logged, "the console says the ball entered the trigger");
        ed.stop();
        CHECK(ed.scene.get(ball)->position.y == 3.0f && !ed.play_physics(), "Stop puts the ball back");
        while (ed.scene.count() > objects0) ed.scene.remove(ed.scene.at(ed.scene.count() - 1).id);
        frames(2);
    }

    // ---- scripts run in Play: inputs, moving, triggers switching the camera, errors
    {
        int objects0 = ed.scene.count();
        const char* mover =
            "-- @prop speed number = 1\n"
            "-- @prop label text = \"x\"\n"
            "function on_start(self) self.started = true; print(self.object.name .. ' starts, ' .. self.label) end\n"
            "function on_update(self, dt) self.object:move(self.speed * dt, 0, 0) end\n";
        platform::write_file("build/host/test-project/mover.lua", mover, strlen(mover));
        const char* switcher =
            "-- @prop camera object\n"
            "function on_trigger_enter(self, other)\n"
            "  print(other.name .. ' entered ' .. self.object.name)\n"
            "  if self.camera then self.camera.camera.current = true end\n"
            "end\n";
        platform::write_file("build/host/test-project/switcher.lua", switcher, strlen(switcher));
        const char* broken = "function on_update(self, dt) local t = nil; t.x = 1 end\n";
        platform::write_file("build/host/test-project/broken.lua", broken, strlen(broken));

        ObjectId runner = ed.create_object("Runner", MeshKind::Cube, Vec3(30, 0.5f, 0));
        ed.add_script(runner, "build/host/test-project/mover.lua");
        ed.scene.script_prop(runner, "mover", "speed")->number = 2;
        snprintf(ed.scene.script_prop(runner, "mover", "label")->text, 64, "%s", "go");
        ed.add_script(runner, "build/host/test-project/broken.lua");
        ObjectId cam_a = ed.create_kind("Cam Main", ObjectKind::Camera, Vec3(30, 2, 8));
        ObjectId cam_b = ed.create_kind("Cam Trap", ObjectKind::Camera, Vec3(36, 2, 8));
        ed.scene.set_current(cam_a, ModuleKind::Camera);
        ObjectId floor2 = ed.create_object("Trap Floor", MeshKind::Plane, Vec3(36, 0, 0));
        ed.scene.get(floor2)->scale = Vec3(4, 1, 4);
        ed.scene.add_module(floor2, ModuleKind::Collider3D);
        ObjectId rock = ed.create_object("Rock", MeshKind::Sphere, Vec3(36, 3, 0));
        ed.scene.add_module(rock, ModuleKind::RigidBody3D);
        ed.scene.add_module(rock, ModuleKind::Collider3D);
        ObjectId trap = ed.create_kind("Trap", ObjectKind::Empty, Vec3(36, 1.5f, 0));
        ed.scene.add_module(trap, ModuleKind::Collider3D);
        ed.scene.get(trap)->modules.collider3d.mode = ColliderMode::Trigger;
        ed.add_script(trap, "build/host/test-project/switcher.lua");
        ed.scene.script_prop(trap, "switcher", "camera")->object = cam_b;

        int log0 = ed.log_count;
        ed.play();
        ScriptRuntime* rt = ed.play_scripts();
        CHECK(rt && rt->stats().instances >= 3, "Play starts the scripts (%d)", rt ? rt->stats().instances : -1);
        frames(60);                                      // 1 s
        float x = ed.scene.get(runner)->position.x;
        CHECK(x > 31.6f && x < 32.4f, "the mover moved its object by speed * time (x %.2f)", x);
        auto logged = [&](const char* what) {
            for (int i = log0; i < ed.log_count; i++) if (strstr(ed.log_lines[i].text, what)) return true;
            return false;
        };
        CHECK(logged("Runner starts, go"), "print() reaches the console with the inputs");
        CHECK(logged("Rock entered Trap"), "a trigger calls on_trigger_enter");
        CHECK(ed.scene.current(ModuleKind::Camera) == cam_b, "the script made the trap camera current");
        CHECK(ed.looking_through_camera, "and Play now looks through it");
        CHECK(logged("broken.lua:1:") && rt->stats().failed == 1, "a runtime error is logged with file:line and stops that script only");
        ed.stop();
        CHECK(ed.scene.current(ModuleKind::Camera) == cam_a && ed.scene.get(runner)->position.x == 30.0f && !ed.play_scripts(),
              "Stop restores the camera and positions");
        while (ed.scene.count() > objects0) ed.scene.remove(ed.scene.at(ed.scene.count() - 1).id);
        frames(2);
    }

    // ---- collider shapes in the viewport: drag handles change size, radius, height
    {
        int objects0 = ed.scene.count();
        ed.camera.target = Vec3(0, 0.5f, 0); ed.camera.yaw = 30; ed.camera.pitch = 20; ed.camera.distance = 7;
        ed.settings.snap = false;
        ed.tool = Tool::Select;
        ObjectId box = ed.create_object("Shape Box", MeshKind::Cube, Vec3(0, 0.5f, 0));
        ed.scene.add_module(box, ModuleKind::Collider3D);
        ed.scene.select_only(box);
        frames(3);
        CHECK(!ui("chandle:0"), "no handles until Edit shape is on");
        CHECK(click_ui("module:collider3d:edit") && ed.edit_collider, "the Edit shape switch turns handles on");
        frames(2);
        CHECK(ui("chandle:0") && ui("chandle:5") && !ui("chandle:6"), "a box has 6 handles");
        // Handle 1 is the +X face: drag it outwards along its direction on screen.
        auto drag_handle = [&](int i, float dx, float dy) {
            char k[24];
            snprintf(k, sizeof(k), "chandle:%d", i);
            const Editor::UiRect* r = ui(k);
            if (!r) return false;
            float x = r->x + r->w * 0.5f, y = r->y + r->h * 0.5f;
            drag(x, y, x + dx, y + dy, 0, 10);
            return true;
        };
        float sx, sy, hx, hy;
        to_screen(Vec3(0, 0.5f, 0), &sx, &sy);
        to_screen(Vec3(0.5f, 0.5f, 0), &hx, &hy);
        float ux = hx - sx, uy = hy - sy;               // the +X direction on screen, half a unit long
        Vec3 pos0 = ed.scene.get(box)->position;
        CHECK(drag_handle(1, ux, uy), "drag the +X handle");
        float size_x = ed.scene.get(box)->modules.collider3d.size.x;
        CHECK(size_x > 1.6f && size_x < 2.4f, "dragging it half a unit out makes the box about 2 wide (%.2f)", size_x);
        CHECK(ed.scene.get(box)->position.x == pos0.x && ed.scene.selected_count() == 1, "the object did not move and stays selected");
        // A capsule: the top handle sets the height, a side handle the radius.
        ed.scene.get(box)->modules.collider3d.shape = ColliderShape::Capsule;
        ed.scene.get(box)->modules.collider3d.radius = 0.5f;
        ed.scene.get(box)->modules.collider3d.height = 2.0f;
        frames(2);
        CHECK(ui("chandle:5") && !ui("chandle:6"), "a capsule has 6 handles (2 for the height, 4 for the radius)");
        float tx, ty, tx2, ty2;
        to_screen(Vec3(0, 1.5f, 0), &tx, &ty);
        to_screen(Vec3(0, 2.0f, 0), &tx2, &ty2);
        CHECK(drag_handle(1, tx2 - tx, ty2 - ty), "drag the top handle");
        float hgt = ed.scene.get(box)->modules.collider3d.height;
        CHECK(hgt > 2.6f && hgt < 3.4f, "half a unit up makes it about 3 tall (%.2f)", hgt);
        float rx, ry, rx2, ry2;
        to_screen(Vec3(0.5f, 0.5f, 0), &rx, &ry);
        to_screen(Vec3(0.25f, 0.5f, 0), &rx2, &ry2);
        CHECK(drag_handle(3, rx2 - rx, ry2 - ry), "drag a side handle in");
        float rad = ed.scene.get(box)->modules.collider3d.radius;
        CHECK(rad > 0.15f && rad < 0.35f, "the radius shrinks to about 0.25 (%.2f)", rad);
        // The new cylinder shape is offered and fits cylinders.
        ObjectId cyl = ed.create_object("Shape Cyl", MeshKind::Cylinder, Vec3(3, 0.5f, 0));
        ed.scene.add_module(cyl, ModuleKind::Collider3D);
        CHECK(ed.scene.get(cyl)->modules.collider3d.shape == ColliderShape::Cylinder, "a cylinder gets a cylinder collider");
        ed.edit_collider = false;
        ed.tool = Tool::Move;
        while (ed.scene.count() > objects0) ed.scene.remove(ed.scene.at(ed.scene.count() - 1).id);
        ed.scene.clear_selection();
        frames(2);
    }

    // ---- audio in Play: a script plays a sampler, a stream starts on its own
    {
        int objects0 = ed.scene.count();
        // A 0.5 s 440 Hz mono WAV.
        {
            const int rate = 22050, n = rate / 2;
            static uint8_t wav[44 + 22050];
            uint32_t data_bytes = n * 2;
            auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; i++) wav[at + i] = (uint8_t)(v >> (8 * i)); };
            auto put16 = [&](int at, uint32_t v) { wav[at] = (uint8_t)v; wav[at + 1] = (uint8_t)(v >> 8); };
            memcpy(wav, "RIFF", 4); put32(4, 36 + data_bytes); memcpy(wav + 8, "WAVEfmt ", 8);
            put32(16, 16); put16(20, 1); put16(22, 1); put32(24, rate); put32(28, rate * 2); put16(32, 2); put16(34, 16);
            memcpy(wav + 36, "data", 4); put32(40, data_bytes);
            for (int i = 0; i < n; i++) {
                float ph = (float)(i % 50) / 50.0f;          // ~441 Hz triangle
                float v = ph < 0.5f ? ph * 4 - 1 : 3 - ph * 4;
                put16(44 + i * 2, (uint32_t)(int16_t)(v * 12000));
            }
            platform::write_file("build/host/test-project/beep.wav", wav, 44 + data_bytes);
        }
        const char* player = "function on_start(self) self.played = self.object.audio_sampler:play() end\n";
        platform::write_file("build/host/test-project/beeper.lua", player, strlen(player));
        ObjectId src = ed.create_kind("Beeper", ObjectKind::Empty, Vec3(0, 1, 0));
        ed.scene.add_module(src, ModuleKind::AudioSampler);
        snprintf(ed.scene.get(src)->modules.audio_sampler.path, kPathMax, "%s", "build/host/test-project/beep.wav");
        ed.scene.get(src)->modules.audio_sampler.spatial = false;
        ed.add_script(src, "build/host/test-project/beeper.lua");
        ObjectId music = ed.create_kind("Music", ObjectKind::Empty, Vec3(0, 1, 0));
        ed.scene.add_module(music, ModuleKind::AudioStream);
        snprintf(ed.scene.get(music)->modules.audio_stream.path, kPathMax, "%s", "build/host/test-project/beep.wav");
        ed.play();
        frames(3);
        AudioSystem* au = ed.play_audio();
        CHECK(au != nullptr, "Play starts audio");
        if (au) {
            CHECK(au->is_playing(ed.entity_of(music)), "an Audio Stream with play-on-start plays");
            CHECK(au->stats().voices_active >= 2, "the script's sampler and the stream are playing (%d voices)", au->stats().voices_active);
            CHECK(au->stats().frames_pushed > 0, "mixed audio reaches the device (%llu frames)", (unsigned long long)au->stats().frames_pushed);
        }
        bool err = false;
        for (int i = 0; i < ed.log_count; i++) err |= strstr(ed.log_lines[i].text, "beeper.lua") && ed.log_lines[i].level == LogLevel::Error;
        CHECK(!err, "obj.audio_sampler:play() runs without errors");
        ed.stop();
        CHECK(!ed.play_audio(), "Stop ends audio");
        while (ed.scene.count() > objects0) ed.scene.remove(ed.scene.at(ed.scene.count() - 1).id);
        frames(2);
    }

    // ---- scene files: save, load back, every field comes back
    {
        ed.new_scene();
        mkdir("build/host/test-project/scripts", 0755);
        const char* door =
            "-- @prop speed  number = 5  range 0 20\n"
            "-- @prop label  text = \"Front door\"\n"
            "-- @prop open   toggle = true\n"
            "-- @prop mode   list(closed, open, locked) = locked\n"
            "-- @prop target object\n"
            "-- @prop skin file\n";
        platform::write_file("build/host/test-project/scripts/gate.lua", door, strlen(door));
        ObjectId base = ed.create_object("Base \"quoted\"", MeshKind::Cylinder, Vec3(1, 2, 3));
        Object* ob = ed.scene.get(base);
        ob->rotation = Quat::euler_deg(Vec3(10, 20, 30));
        ob->scale = Vec3(0.5f, 2, 0.75f);
        ob->color = Vec3(0.1f, 0.2f, 0.3f);
        ob->roughness = 0.33f; ob->metallic = 0.8f; ob->unlit = true; ob->cast_shadows = false;
        ob->visible = false;
        snprintf(ob->texture_path, sizeof ob->texture_path, "%s/crate.png", ed.project_dir);
        snprintf(ob->shader_path, sizeof ob->shader_path, "%s/material.fshader", ed.project_dir);
        ed.scene.add_module(base, ModuleKind::Collider3D);
        ob->modules.collider3d.shape = ColliderShape::Capsule;
        ob->modules.collider3d.radius = 0.7f;
        ob->modules.collider3d.layer = 0x8005;
        ed.scene.add_module(base, ModuleKind::AudioSampler);
        snprintf(ob->modules.audio_sampler.path, kPathMax, "%s/sounds/hit.wav", ed.project_dir);
        ob->modules.audio_sampler.voices = 7;
        ObjectId kid = ed.create_child("Kid", MeshKind::Sphere, base);
        ObjectId lamp = ed.create_light(LightType::Spot, Vec3(0, 3, 0), kid);
        ed.scene.get(lamp)->modules.light.spot_angle = 33.5f;
        ObjectId cam1 = ed.create_kind("Cam 1", ObjectKind::Camera, Vec3(0, 1, 5));
        ObjectId cam2 = ed.create_kind("Cam 2", ObjectKind::Camera, Vec3(0, 1, -5));
        ed.scene.set_current(cam2, ModuleKind::Camera);
        ed.scene.get(cam2)->modules.camera.fov = 72.0f;
        char door_full[300];
        snprintf(door_full, sizeof door_full, "%s/scripts/gate.lua", ed.project_dir);
        CHECK(ed.add_script(kid, door_full) == 0, "a script with inputs goes on the kid");
        if (ScriptProp* p = ed.scene.script_prop(kid, "gate", "speed")) p->number = 12.5f;
        if (ScriptProp* p = ed.scene.script_prop(kid, "gate", "label")) snprintf(p->text, sizeof p->text, "%s", "Back \"door\"");
        if (ScriptProp* p = ed.scene.script_prop(kid, "gate", "open")) p->toggle = false;
        if (ScriptProp* p = ed.scene.script_prop(kid, "gate", "mode")) p->choice = 1;
        if (ScriptProp* p = ed.scene.script_prop(kid, "gate", "target")) p->object = cam1;
        if (ScriptProp* p = ed.scene.script_prop(kid, "gate", "skin")) snprintf(p->file, sizeof p->file, "%s/crate.png", ed.project_dir);

        size_t size1 = 0;
        char* text1 = save_scene_text(ed.scene, ed.project_dir, &size1);
        CHECK(text1 && strstr(text1, "farcade-scene 1") && strstr(text1, "texture \"crate.png\"") &&
              strstr(text1, "shader \"material.fshader\"") && strstr(text1, "file \"crate.png\"") &&
              strstr(text1, "\"scripts/gate.lua\""),
              "the file starts with its version and stores project paths relative");
        Scene back;
        SceneLoadReport rep;
        CHECK(text1 && load_scene_text(back, text1, size1, &rep) && rep.skipped == 0, "it loads back (%s, %d skipped)", rep.error, rep.skipped);
        size_t size2 = 0;
        char* text2 = save_scene_text(back, ed.project_dir, &size2);
        CHECK(text1 && text2 && size1 == size2 && memcmp(text1, text2, size1) == 0, "saving the loaded scene gives the same file");
        free(text1);
        free(text2);
        Object* bb = back.get(base);
        CHECK(back.count() == ed.scene.count() && bb && back.get(kid) && back.get(kid)->parent == base && back.get(lamp)->parent == kid,
              "objects keep their ids and the tree");
        CHECK(bb && !strcmp(bb->name, "Base \"quoted\"") && bb->mesh == MeshKind::Cylinder && bb->metallic == 0.8f && bb->roughness == 0.33f &&
              bb->unlit && !bb->cast_shadows && !bb->visible && bb->scale.z == 0.75f && bb->rotation.w == ob->rotation.w,
              "names, meshes, materials and transforms come back exactly");
        CHECK(bb && !strcmp(bb->shader_path, "material.fshader") &&
              !strcmp(back.get(kid)->scripts[0].prop("skin")->file, "crate.png"),
              "shader and script asset paths are saved project-relative");
        CHECK(bb && bb->modules.has(ModuleKind::Collider3D) && bb->modules.collider3d.shape == ColliderShape::Capsule &&
              bb->modules.collider3d.radius == 0.7f && bb->modules.collider3d.layer == 0x8005 && bb->modules.audio_sampler.voices == 7 &&
              !strcmp(bb->modules.audio_sampler.path, "sounds/hit.wav"), "modules come back with every field");
        CHECK(back.current(ModuleKind::Camera) == cam2 && back.get(cam2)->modules.camera.fov == 72.0f &&
              back.get(lamp)->modules.light.type == LightType::Spot && back.get(lamp)->modules.light.spot_angle == 33.5f,
              "special objects and the current flag come back");
        CHECK(back.next_id() >= ed.scene.next_id(), "new objects get fresh ids after loading");
        ScriptProp* sp = back.script_prop(kid, "gate", "label");
        ScriptProp* sm = back.script_prop(kid, "gate", "mode");
        ScriptProp* st = back.script_prop(kid, "gate", "target");
        CHECK(sp && !strcmp(sp->text, "Back \"door\"") && sm && sm->choice == 1 && !strcmp(sm->choices[sm->choice], "open") && st && st->object == cam1 &&
              back.script_prop(kid, "gate", "speed")->number == 12.5f && !back.script_prop(kid, "gate", "open")->toggle,
              "script inputs come back with their values and links");

        // Every field of every module, set away from its default, comes back.
        {
            Scene all;
            for (int k = 0; k < kModuleKindCount; k++) {
                Object* o = all.create_with_id((ObjectId)(k + 1));
                const ModuleInfo& mi = module_info((ModuleKind)k);
                o->modules.add((ModuleKind)k);
                uint8_t* d = (uint8_t*)o->modules.data((ModuleKind)k);
                for (int f = 0; f < mi.field_count; f++) {
                    const FieldInfo& fi = mi.fields[f];
                    uint8_t* p = d + fi.offset;
                    switch (fi.type) {
                    case FieldType::Float: { float v; memcpy(&v, p, 4); v = v * 1.5f + 0.3125f; memcpy(p, &v, 4); break; }
                    case FieldType::Int: { int32_t v; memcpy(&v, p, 4); v += 3; memcpy(p, &v, 4); break; }
                    case FieldType::Bits: { uint32_t v; memcpy(&v, p, 4); v ^= 0x80000010u; memcpy(p, &v, 4); break; }
                    case FieldType::Object: { uint32_t v = 7; memcpy(p, &v, 4); break; }
                    case FieldType::Bool: *(bool*)p = !*(bool*)p; break;
                    case FieldType::Enum: { int n = 0; while (fi.enum_names[n]) n++; *p = (uint8_t)((*p + 1) % n); break; }
                    case FieldType::Color:
                    case FieldType::Vec3: { Vec3 v; memcpy(&v, p, sizeof v); v = v + Vec3(0.125f, -0.25f, 1.5f); memcpy(p, &v, sizeof v); break; }
                    case FieldType::Text:
                    case FieldType::Path: snprintf((char*)p, kPathMax, "sounds/a \"b\" c.ogg"); break;
                    case FieldType::PathList: snprintf((char*)p, kPathListMax, "models/a.mof; models/b \"c\".mof;d.mof"); break;
                    }
                }
            }
            size_t n = 0;
            char* t = save_scene_text(all, "", &n);
            Scene got;
            SceneLoadReport r2;
            bool ok = t && load_scene_text(got, t, n, &r2) && r2.skipped == 0 && got.count() == kModuleKindCount;
            int wrong = 0;
            for (int k = 0; ok && k < kModuleKindCount; k++) {
                const ModuleInfo& mi = module_info((ModuleKind)k);
                Object* a1 = all.get((ObjectId)(k + 1));
                Object* b1 = got.get((ObjectId)(k + 1));
                if (!b1 || !b1->modules.has((ModuleKind)k)) { wrong++; continue; }
                const uint8_t* da = (const uint8_t*)a1->modules.data((ModuleKind)k);
                const uint8_t* db = (const uint8_t*)b1->modules.data((ModuleKind)k);
                for (int f = 0; f < mi.field_count; f++) {
                    const FieldInfo& fi = mi.fields[f];
                    size_t sz = field_size(fi.type);
                    if (memcmp(da + fi.offset, db + fi.offset, sz) != 0) { wrong++; printf("  field %s.%s differs\n", mi.name, fi.name); }
                }
            }
            free(t);
            CHECK(ok && wrong == 0, "every field of every module comes back (%d wrong, %d skipped)", wrong, r2.skipped);
        }

        // Unknown lines are skipped, broken files are refused.
        const char* future = "farcade-scene 1\nnext_id 5\nobject 2\n  name \"X\"\n  sparkle 3\n  module light\n    glow 1\n    intensity 2\n  end\n"
                             "  module warp_drive\n    speed 9\n  end\nend\n";
        Scene fs;
        CHECK(load_scene_text(fs, future, strlen(future), &rep) && rep.skipped == 3 && fs.get(2) && fs.get(2)->modules.light.intensity == 2.0f,
              "unknown fields and modules are skipped (%d)", rep.skipped);
        const char* broken[] = { "", "hello\n", "farcade-scene 99\n", "farcade-scene 1\nobject 3\n  name \"A\"\n",
                                 "farcade-scene 1\nobject 3\nend\nobject 3\nend\n" };
        int refused = 0;
        for (const char* t : broken) refused += !load_scene_text(fs, t, strlen(t), &rep) && fs.count() == 0;
        CHECK(refused == 5, "broken files are refused (%d of 5)", refused);
        const char* loop = "farcade-scene 1\nobject 1\n  parent 2\nend\nobject 2\n  parent 1\nend\nobject 3\n  parent 9\nend\n";
        CHECK(load_scene_text(fs, loop, strlen(loop), &rep) && fs.depth(*fs.get(1)) < 3 && fs.depth(*fs.get(2)) < 3 && fs.get(3)->parent == kNoObject,
              "parent loops and missing parents are cut");

        // Through the editor: Ctrl+S saves to scenes/main.fscene, opening it brings the scene back.
        frames(1);
        const uint16_t key_s = (uint16_t)(platform::KEY_A + ('s' - 'a'));
        ev_key(platform::KEY_LCTRL, true, platform::MOD_CTRL); ev_key(key_s, true, platform::MOD_CTRL); frames(1);
        ev_key(key_s, false, platform::MOD_CTRL); ev_key(platform::KEY_LCTRL, false); frames(1);
        CHECK(platform::file_exists("build/host/test-project/scenes/main.fscene") && strstr(ed.scene_path, "scenes/main.fscene"),
              "Ctrl+S saves the scene (%s)", ed.scene_path);
        int count = ed.scene.count();
        ed.play();
        ed.scene.get(base)->position = Vec3(9, 9, 9);
        CHECK(ed.save_scene("scenes/during-play.fscene"), "saving while playing works");
        ed.stop();
        // Double-click in Files hands over the full path (project folder included).
        ed.new_scene();
        char main_full[300];
        snprintf(main_full, sizeof main_full, "%s/scenes/main.fscene", ed.project_dir);
        ed.open_file(main_full);
        CHECK(ed.scene.count() == count && !strcmp(ed.scene_path, main_full), "opening a scene from Files works (%s)", ed.scene_path);
        ed.new_scene();
        CHECK(ed.scene.count() == 0 && !ed.scene_path[0], "New scene empties it");
        CHECK(ed.open_scene("scenes/during-play.fscene") && ed.scene.count() == count && ed.scene.get(base)->position.x == 1.0f,
              "the file saved during Play holds the scene from before Play");
        CHECK(ed.scene.script_prop(kid, "gate", "speed") && ed.scene.script_prop(kid, "gate", "speed")->number == 12.5f &&
              ed.scene.script_prop(kid, "gate", "speed")->max == 20.0f, "opening re-reads the script inputs and keeps the saved values");
        ObjectId fresh = ed.create_object("Fresh", MeshKind::Cube, Vec3(0, 0, 0));
        CHECK(fresh > lamp && fresh > cam2, "objects made after opening get new ids");
        int before = ed.scene.count();
        platform::write_file("build/host/test-project/scenes/bad.fscene", "nope", 4);
        CHECK(!ed.open_scene("scenes/bad.fscene") && ed.scene.count() == before, "a broken file leaves the scene alone");
        // File > Save scene as...
        CHECK(click_ui("menubar:File"), "File menu opens");
        frames(2);
        CHECK(click_ui("menu:Save scene as..."), "Save scene as... opens a prompt");
        frames(2);
        // Type the name: select all, then the letters.
        ev_key(platform::KEY_LCTRL, true, platform::MOD_CTRL); ev_key(platform::KEY_A, true, platform::MOD_CTRL); frames(1);
        ev_key(platform::KEY_A, false, platform::MOD_CTRL); ev_key(platform::KEY_LCTRL, false); frames(1);
        for (const char* c = "level2"; *c; c++) {
            platform::Event e;
            e.type = platform::EventType::Text;
            e.codepoint = (uint32_t)*c;
            platform::headless_push(e);
        }
        frames(2);
        CHECK(click_ui("save_as:save"), "the prompt's Save button");
        frames(2);
        CHECK(platform::file_exists("build/host/test-project/scenes/level2.fscene") && strstr(ed.scene_path, "level2.fscene"),
              "Save scene as writes scenes/level2.fscene (%s)", ed.scene_path);
        ed.new_scene();
        frames(2);
    }

    // ---- project settings and export
    {
        ed.project = ProjectSettings();
        CHECK(click_ui("menubar:File"), "File menu opens");
        frames(2);
        CHECK(click_ui("menu:Project settings..."), "Project settings... opens a dialog");
        frames(2);
        snprintf(ed.project.name, sizeof ed.project.name, "Test Game");
        snprintf(ed.project.entry_scene, sizeof ed.project.entry_scene, "scenes/level2.fscene");
        ed.project.window_width = 800;
        frames(1);
        CHECK(click_ui("project:save"), "the dialog's Save button");
        frames(2);
        CHECK(platform::file_exists("build/host/test-project/project.fe"), "project.fe is written");
        ProjectSettings back = ed.project;
        ed.project = ProjectSettings();
        CHECK(ed.load_project() && !strcmp(ed.project.name, "Test Game") && ed.project.window_width == 800 &&
                  !strcmp(ed.project.entry_scene, "scenes/level2.fscene") && !strcmp(back.name, "Test Game"),
              "project.fe reads back (%s)", ed.project.name);
        // File > Export game...
        CHECK(click_ui("menubar:File"), "File menu opens");
        frames(2);
        CHECK(click_ui("menu:Export game..."), "Export game... opens a dialog");
        frames(2);
        CHECK(!strcmp(ed.export_name, "Test Game") && !strcmp(ed.export_scene, "scenes/level2.fscene"), "the dialog starts from the project settings");
        snprintf(ed.export_out, sizeof ed.export_out, "build/host/test-export");
        frames(1);
        CHECK(click_ui("export:go"), "the dialog's Export button");
        frames(2);
        CHECK(!strncmp(ed.export_status, "Exported", 8), "export succeeded (%s)", ed.export_status);
        CHECK(platform::file_exists("build/host/test-export/Test_Game.fepak") && platform::file_exists("build/host/test-export/Test_Game"),
              "the package and the player are in the output folder");
        CHECK(click_ui("export:close"), "Close");
        frames(2);
    }

    // ---- importing a model
    {
        ed.new_scene();
        frames(2);
        const char* obj = "o quad\nv 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nvn 0 0 1\nf 1//1 2//1 3//1\nf 2//1 4//1 3//1\n";
        platform::make_dir("build/host/test-project/models");
        platform::write_file("build/host/test-project/models/quad.obj", obj, strlen(obj));
        int before = ed.scene.count();
        CHECK(click_ui("menubar:Create"), "Create menu opens");
        frames(2);
        CHECK(click_ui("menu:Import model..."), "Create > Import model... opens the file picker");
        frames(2);
        for (const char* c = "models/quad.obj"; *c; c++) {
            platform::Event e;
            e.type = platform::EventType::Text;
            e.codepoint = (uint32_t)*c;
            platform::headless_push(e);
        }
        frames(2);
        CHECK(click_ui("pick:ok"), "the picker's Open button");
        frames(2);
        CHECK(ed.scene.count() == before + 1, "the model is in the scene");
        Object* mo = ed.scene.get(ed.scene.active());
        CHECK(mo && mo->mesh == MeshKind::Model && strstr(mo->model_path, "quad.mof"), "the object uses the converted .mof (%s)", mo ? mo->model_path : "");
        CHECK(mo && Scene::mesh_for(*mo).vertex_count == 6 && Scene::model_for(*mo), "the real mesh is used, not the cube");
        CHECK(platform::file_exists("build/host/test-project/models/quad.mof"), "quad.mof was written next to the source");
        ed.new_scene();
        frames(2);
    }

    // ---- undo and redo
    {
        ed.new_scene();
        frames(2);
        CHECK(!ed.can_undo() && !ed.can_redo() && !ed.scene_modified(), "a new scene has no history and no changes");
        ObjectId u1 = ed.create_object("Undo A", MeshKind::Cube, Vec3(0, 0.5f, 0));
        ed.scene.add_module(u1, ModuleKind::Light);
        ed.scene.get(u1)->modules.light.intensity = 4.0f;
        ed.history_capture();
        ObjectId u2 = ed.create_object("Undo B", MeshKind::Sphere, Vec3(2, 0.5f, 0));
        ed.history_capture();
        CHECK(ed.can_undo() && ed.scene_modified(), "edits go into the history and mark the scene changed");
        ed.scene.select_only(u1);
        ed.undo();
        CHECK(!ed.scene.get(u2) && ed.scene.get(u1) && ed.can_redo(), "undo removes the last object");
        CHECK(ed.scene.is_selected(u1) && ed.scene.active() == u1, "undo keeps the selection");
        ed.redo();
        CHECK(ed.scene.get(u2) && !ed.can_redo(), "redo brings it back");
        // Delete through the editor, then undo: modules come back too.
        ed.scene.select_only(u1);
        ed.delete_selection();
        ed.history_capture();
        CHECK(!ed.scene.get(u1), "deleted");
        ed.undo();
        CHECK(ed.scene.get(u1) && ed.scene.get(u1)->modules.has(ModuleKind::Light) && ed.scene.get(u1)->modules.light.intensity == 4.0f,
              "undo of a delete brings the object back with its modules");
        // Ids are not handed out twice after an undo.
        ed.undo();      // back before B
        ObjectId u3 = ed.create_object("Undo C", MeshKind::Cube, Vec3(0, 0, 0));
        CHECK(u3 > u2, "a new object after undo gets a fresh id (%u > %u)", (unsigned)u3, (unsigned)u2);
        ed.history_capture();
        CHECK(!ed.can_redo(), "a new edit clears redo");
        ed.scene.clear_selection();
        frames(2);
        // A drag is one step: changes while a mouse button is down are not recorded on their own.
        int steps0 = 0;
        while (ed.can_undo()) { ed.undo(); steps0++; }
        while (ed.can_redo()) ed.redo();
        float qx = ed.view_rect[0] + 30, qy = ed.view_rect[1] + ed.view_rect[3] - 30;
        ev_move(qx, qy); frames(1);
        ev_button(qx, qy, 0, true); frames(1);
        for (int i = 1; i <= 5; i++) { ed.scene.get(u3)->position.x = (float)i; ev_move(qx + i, qy); frames(1); }
        ev_button(qx + 5, qy, 0, false); frames(2);
        int steps1 = 0;
        while (ed.can_undo()) { ed.undo(); steps1++; }
        CHECK(steps1 == steps0 + 1, "a drag is one undo step (%d -> %d)", steps0, steps1);
        while (ed.can_redo()) ed.redo();
        CHECK(ed.scene.get(u3) && ed.scene.get(u3)->position.x == 5.0f, "redo all the way gives the dragged position back");
        // Keys: Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z.
        const uint16_t kz = (uint16_t)(platform::KEY_A + ('z' - 'a')), ky = (uint16_t)(platform::KEY_A + ('y' - 'a'));
        auto chord = [&](uint16_t k, uint8_t mods) {
            ev_key(platform::KEY_LCTRL, true, mods); ev_key(k, true, mods); frames(1);
            ev_key(k, false, mods); ev_key(platform::KEY_LCTRL, false); frames(1);
        };
        chord(kz, platform::MOD_CTRL);
        CHECK(ed.scene.get(u3)->position.x == 0.0f, "Ctrl+Z undoes (x %.1f)", ed.scene.get(u3)->position.x);
        chord(ky, platform::MOD_CTRL);
        CHECK(ed.scene.get(u3)->position.x == 5.0f, "Ctrl+Y redoes");
        chord(kz, platform::MOD_CTRL);
        chord(kz, platform::MOD_CTRL | platform::MOD_SHIFT);
        CHECK(ed.scene.get(u3)->position.x == 5.0f, "Ctrl+Shift+Z redoes too");
        // Play does not add history; Stop leaves it as it was.
        int before_play = 0;
        while (ed.can_undo()) { ed.undo(); before_play++; }
        while (ed.can_redo()) ed.redo();
        ed.play();
        ed.scene.get(u3)->position.y = 42.0f;
        frames(3);
        ed.stop();
        frames(2);
        int after_play = 0;
        while (ed.can_undo()) { ed.undo(); after_play++; }
        while (ed.can_redo()) ed.redo();
        CHECK(after_play == before_play, "Play adds no undo steps (%d, %d)", before_play, after_play);

        // ---- unsaved changes: New scene and Open ask first
        CHECK(ed.scene_modified(), "the scene has unsaved changes");
        int n0 = ed.scene.count();
        CHECK(click_ui("menubar:File") && (frames(2), click_ui("menu:New scene")), "File > New scene");
        frames(2);
        CHECK(ui("unsaved:cancel") != nullptr && ed.scene.count() == n0, "it asks first and keeps the scene");
        click_ui("unsaved:cancel");
        frames(2);
        CHECK(ed.scene.count() == n0 && !ui("unsaved:cancel"), "Cancel keeps the scene");
        click_ui("menubar:File"); frames(2); click_ui("menu:New scene"); frames(2);
        CHECK(click_ui("unsaved:discard") && (frames(2), ed.scene.count() == 0), "Don't save empties it");
        CHECK(!ed.scene_modified(), "the new scene has no unsaved changes");
        // Unchanged: opening does not ask.
        char lvl[300];
        snprintf(lvl, sizeof lvl, "%s/scenes/level2.fscene", ed.project_dir);
        ed.open_file(lvl);
        frames(2);
        CHECK(!ui("unsaved:save") && strstr(ed.scene_path, "level2.fscene") && ed.scene.count() > 0, "an unchanged scene opens without asking");
        // Changed, then open with Save: the change is written first.
        ed.create_object("Saved First", MeshKind::Cube, Vec3(0, 0, 0));
        int with_new = ed.scene.count();
        char mainp[300];
        snprintf(mainp, sizeof mainp, "%s/scenes/main.fscene", ed.project_dir);
        ed.open_file(mainp);
        frames(2);
        CHECK(click_ui("unsaved:save"), "the prompt's Save");
        frames(2);
        CHECK(strstr(ed.scene_path, "main.fscene") != nullptr, "then the other scene opens (%s)", ed.scene_path);
        ed.open_scene("scenes/level2.fscene");
        CHECK(ed.scene.count() == with_new, "the change was saved to level2 before leaving it");
        ed.new_scene();
        frames(2);
    }

    // ---- effects modules in the editor: preview, Play, Lua
    {
        ed.new_scene();
        frames(2);
        ObjectId em = ed.create_object("Fire", MeshKind::Cube, Vec3(0, 1, 0));
        CHECK(ed.scene.add_module(em, ModuleKind::ParticleEmitter), "Particle Emitter can be added");
        ed.scene.get(em)->modules.particles.emission_rate = 60;
        ed.scene.get(em)->modules.particles.lifetime_min = ed.scene.get(em)->modules.particles.lifetime_max = 5;
        ed.scene.select_only(em);
        frames(40);
        CHECK(ed.fx()->preview_emitters() == 1 && ed.fx()->last_particles > 5, "a selected emitter previews in the viewport (%d drawn)", ed.fx()->last_particles);
        ed.scene.clear_selection();
        frames(3);
        CHECK(ed.fx()->preview_emitters() == 0, "deselected: the preview stops");
        ed.scene.select_only(em);
        ObjectId hang = ed.create_object("Hang", MeshKind::Cube, Vec3(5, 6, 0));
        ed.scene.add_module(hang, ModuleKind::RigidBody3D);
        ed.scene.add_module(hang, ModuleKind::Joint3D);       // fixed to the world
        ed.scene.get(hang)->modules.rigidbody3d.mass = 2;
        ed.play();
        frames(40);
        CHECK(ed.fx()->world() != nullptr && ed.scene.get(em)->modules.particles.particle_count > 20 && ed.scene.get(em)->modules.particles.playing,
              "Play runs the emitter (%d alive)", ed.scene.get(em)->modules.particles.particle_count);
        CHECK(ed.fx()->last_particles > 5, "and the viewport draws its particles (%d)", ed.fx()->last_particles);
        ed.play_scripts()->run("local f = fe.find('Fire'); f.particles:stop(); f.particles:clear(); f.particles:emit(12)");
        frames(1);
        CHECK(ed.scene.get(em)->modules.particles.particle_count == 12 && !ed.scene.get(em)->modules.particles.playing, "Lua: stop(), clear(), emit(12) (%d alive)", ed.scene.get(em)->modules.particles.particle_count);
        ed.play_scripts()->run("fe.find('Fire').particles:play()");
        frames(10);
        CHECK(ed.scene.get(em)->modules.particles.playing && ed.scene.get(em)->modules.particles.particle_count > 12, "Lua: play()");
        CHECK(absf_(ed.scene.get(hang)->position.y - 6.0f) < 0.05f, "a body fixed to the world with Joint 3D hangs in the air (%.2f)", ed.scene.get(hang)->position.y);
        ed.play_scripts()->run("fe.find('Hang').joint3d:snap()");
        frames(60);
        CHECK(ed.scene.get(hang)->modules.joint3d.broken && ed.scene.get(hang)->position.y < 4.0f, "Lua: joint3d:snap() lets it fall (%.2f)", ed.scene.get(hang)->position.y);
        // Spawner: copies of a ball with a body, a lifetime, Lua spawn() and clear().
        ed.stop();
        frames(2);
        ObjectId ball = ed.create_object("Ball", MeshKind::Sphere, Vec3(0, 20, 0));
        ed.scene.add_module(ball, ModuleKind::RigidBody3D);
        ObjectId sp = ed.create_object("Fountain", MeshKind::Cube, Vec3(0, 3, 0));
        ed.scene.add_module(sp, ModuleKind::Spawner);
        {
            Spawner& m = ed.scene.get(sp)->modules.spawner;
            m.source = ball; m.interval = 0.25f; m.max_alive = 5; m.lifetime = 0; m.velocity = Vec3(0, 4, 0);
        }
        int objects_before = ed.scene.count();
        ed.play();
        frames(150);
        CHECK(ed.scene.count() > objects_before && ed.scene.count() <= objects_before + 5, "a spawner adds copies to the play scene, at most max_alive (%d -> %d)", objects_before, ed.scene.count());
        CHECK(ed.scene.get(sp)->modules.spawner.spawned >= 8 && ed.scene.get(sp)->modules.spawner.alive == 5, "spawned %d, alive %d", ed.scene.get(sp)->modules.spawner.spawned, ed.scene.get(sp)->modules.spawner.alive);
        ed.play_scripts()->run("local s = fe.find('Fountain'); s.spawner.auto_spawn = false; s.spawner.interval = 1; s.spawner:clear()");
        frames(2);
        CHECK(ed.scene.count() == objects_before, "clear() removes the copies from the scene too (%d)", ed.scene.count());
        ed.play_scripts()->run("local s = fe.find('Fountain'); s.spawner:spawn(); s.spawner:spawn()");
        frames(2);
        CHECK(ed.scene.get(sp)->modules.spawner.alive == 2 && ed.scene.count() == objects_before + 2, "Lua spawn() x2 (%d)", ed.scene.count());
        ed.stop();
        frames(2);
        CHECK(ed.scene.count() == objects_before, "Stop puts the scene back");
        ed.scene.remove(ball);
        ed.scene.remove(sp);
        // Animator: a clip file in the project moves the object in Play; Lua picks clips.
        const char* anim = "fanim 1\nclip slide 2\n track . position\n  key 0 0 1 0\n  key 2 6 1 0\nclip rise 1\n track . position\n  key 0 0 1 0\n  key 1 0 5 0\n";
        platform::write_file("build/host/test-project/box.fanim", anim, strlen(anim));
        ObjectId mv = ed.create_object("Mover", MeshKind::Cube, Vec3(0, 1, 0));
        ed.scene.add_module(mv, ModuleKind::Animator);
        snprintf(ed.scene.get(mv)->modules.animator.source, kPathMax, "box.fanim");
        snprintf(ed.scene.get(mv)->modules.animator.clip, kPathMax, "slide");
        ed.play();
        frames(60);
        CHECK(ed.scene.get(mv)->position.x > 2.0f && ed.scene.get(mv)->modules.animator.playing, "an Animator plays its clip in Play (x %.2f)", ed.scene.get(mv)->position.x);
        ed.play_scripts()->run("local m = fe.find('Mover'); m.animator:play('rise', 0); m.animator.speed = 1");
        frames(30);
        CHECK(absf_(ed.scene.get(mv)->position.y - 3.0f) < 0.5f && !strcmp(ed.scene.get(mv)->modules.animator.clip, "rise"), "Lua: animator:play('rise') (y %.2f)", ed.scene.get(mv)->position.y);
        ed.stop();
        frames(2);
        CHECK(ed.scene.get(mv)->position.x == 0.0f, "Stop puts it back");
        ed.scene.remove(mv);
        // Billboard and Text 3D show in the viewport; the quality switch hides them.
        ObjectId bb = ed.create_object("Icon", MeshKind::Cube, Vec3(0, 1, 0));
        ed.scene.add_module(bb, ModuleKind::Billboard);
        ObjectId tx = ed.create_object("Sign", MeshKind::Cube, Vec3(0, 2, 0));
        ed.scene.add_module(tx, ModuleKind::Text3D);
        snprintf(ed.scene.get(tx)->modules.text3d.text, kPathMax, "Hello");
        ed.scene.get(tx)->modules.text3d.size = 0.8f;
        frames(3);
        CHECK(ed.fx()->last_sprites == 2, "a billboard and a label are drawn (%d)", ed.fx()->last_sprites);
        ed.settings.graphics.fx_sprites = false;
        frames(2);
        CHECK(ed.fx()->last_sprites == 0, "Effects > sprites off draws none");
        ed.settings.graphics.fx_sprites = true;
        ed.play();
        frames(2);
        ed.play_scripts()->run("local s = fe.find('Sign'); s.text3d.text = 'Score 10'; s.text3d.size = 1.0; fe.find('Icon').billboard.alpha = 0.5");
        frames(2);
        CHECK(!strcmp(ed.scene.get(tx)->modules.text3d.text, "Score 10") && ed.scene.get(bb)->modules.billboard.alpha == 0.5f, "Lua reaches the text and the billboard");
        ed.stop();
        frames(2);
        ed.scene.remove(bb);
        ed.scene.remove(tx);
        ed.play();
        frames(2);
        ed.stop();
        frames(2);
        CHECK(ed.fx()->world() == nullptr && ed.scene.get(em)->modules.particles.particle_count == 0, "Stop puts the scene back");
        ed.new_scene();
        frames(2);
    }

    // ---- panels can be dragged out of the dock
    ImGuiWindow* iw = ImGui::FindWindowByName("###Inspector");
    CHECK(iw && iw->DockIsActive, "Inspector starts docked");
    if (iw && iw->DockNode && iw->DockNode->TabBar) {
        ImGuiTabItem* t = ImGui::TabBarFindTabByID(iw->DockNode->TabBar, iw->TabId);
        CHECK(t != nullptr, "Inspector has a tab");
        if (t) {
            float tx = iw->DockNode->TabBar->BarRect.Min.x + t->Offset + t->Width * 0.5f;
            float ty = iw->DockNode->TabBar->BarRect.GetCenter().y;
            drag(tx, ty, ed.view_rect[0] + 200, ed.view_rect[1] + 150, 0, 25);
            CHECK(!iw->DockIsActive, "dragging its tab into the middle floats the Inspector");
        }
    }

    CHECK(ed.damage_mismatches == 0, "incremental UI redraws differ from full redraws in %d frames", ed.damage_mismatches);
    printf("editor_tests: frames %d drawn %d skipped %d\n", ed.perf.frames, ed.perf.drawn, ed.perf.skipped);
    // ---- Files: right-click New script, Rename, Remove
    {
        platform::make_dir("build/host/test-project/sandbox");
        snprintf(ed.files_dir, sizeof(ed.files_dir), "%s/sandbox", dir);
        frames(3);
        ImGui::SetWindowFocus("###Files");      // it shares a tab bar with the Console
        frames(3);
        CHECK(click_ui("files:empty", 1), "right-click the empty part of Files");
        frames(2);
        CHECK(click_ui("menu:New Lua script..."), "Files offers New Lua script");
        frames(2);
        type_text("walker");
        frames(1);
        CHECK(click_ui("files:action_ok"), "the name dialog's Create");
        frames(3);
        CHECK(platform::file_exists("build/host/test-project/sandbox/walker.lua"), "walker.lua was created");
        frames(2);
        const Editor::TileRect* t = tile("walker.lua");
        CHECK(t != nullptr, "it shows in Files");
        if (t) {
            click(t->x + t->w * 0.5f, t->y + t->h * 0.5f, 1);
            frames(2);
            CHECK(click_ui("menu:Rename..."), "a file offers Rename");
            frames(2);
            type_text("runner.lua");       // the old name is selected, so typing replaces it
            frames(1);
            click_ui("files:action_ok");
            frames(3);
            CHECK(platform::file_exists("build/host/test-project/sandbox/runner.lua") &&
                  !platform::file_exists("build/host/test-project/sandbox/walker.lua"), "Rename renames it");
        }
        t = tile("runner.lua");
        if (t) {
            click(t->x + t->w * 0.5f, t->y + t->h * 0.5f, 1);
            frames(2);
            CHECK(click_ui("menu:Remove file..."), "a file offers Remove");
            frames(2);
            click_ui("files:action_ok");
            frames(3);
            CHECK(!platform::file_exists("build/host/test-project/sandbox/runner.lua"), "Remove deletes it");
        }
    }

    // ---- File > Open project...: the folder picker
    {
        platform::make_dir("build/host/other-project");
        platform::write_file("build/host/other-project/project.fe", "name = Other\n", 13);
        CHECK(click_ui("menubar:File") && (frames(2), click_ui("menu:Open project...")), "File > Open project... opens the picker");
        frames(2);
        CHECK(click_ui("pick:place1"), "the Projects place");
        frames(2);
        const Editor::UiRect* r = ui("pick:other-project");
        CHECK(r != nullptr, "the picker lists other-project");
        if (r) {
            click(r->x + r->w * 0.5f, r->y + r->h * 0.5f);
            click(r->x + r->w * 0.5f, r->y + r->h * 0.5f);
            frames(2);
        }
        CHECK(click_ui("pick:ok"), "Select folder");
        frames(3);
        if (ui("unsaved:discard")) { click_ui("unsaved:discard"); frames(3); }
        CHECK(strstr(ed.project_dir, "other-project") != nullptr, "the project is open (%s)", ed.project_dir);
    }

    // ---- Shift + double-click selects similar objects; edits go to every selected object
    {
        ed.stop();
        ed.new_scene();
        ObjectId c1 = ed.create_object("C1", MeshKind::Cube, Vec3(-2, 0.5f, 0));
        ObjectId c2 = ed.create_object("C2", MeshKind::Cube, Vec3(2, 0.5f, 0));
        ObjectId c3 = ed.create_object("C3", MeshKind::Cube, Vec3(0, 0.5f, -3));
        ObjectId sp = ed.create_object("S", MeshKind::Sphere, Vec3(0, 0.5f, 2));
        ObjectId l1 = ed.create_light(LightType::Point, Vec3(0, 3, 0));
        ObjectId l2 = ed.create_light(LightType::Point, Vec3(3, 3, 0));
        ObjectId l3 = ed.create_light(LightType::Spot, Vec3(-3, 3, 0));
        ed.scene.get(c3)->visible = false;
        ed.camera.target = Vec3(0, 0.5f, 0);
        ed.camera.yaw = 0;
        ed.camera.pitch = 25;
        ed.camera.distance = 10;
        ed.tool = Tool::Select;
        ed.terrain_tool = Editor::TerrainTool::None;
        ed.scene.clear_selection();
        frames(3);
        float x1, y1;
        object_screen(c1, &x1, &y1);
        ev_key(platform::KEY_LSHIFT, true, platform::MOD_SHIFT); frames(1);
        ev_move(x1, y1); frames(1);
        ev_button(x1, y1, 0, true); frames(1);
        ev_button(x1, y1, 0, false); frames(1);
        ev_button(x1, y1, 0, true); frames(1);
        ev_button(x1, y1, 0, false); frames(1);
        ev_key(platform::KEY_LSHIFT, false, 0); frames(2);
        CHECK(ed.scene.is_selected(c1) && ed.scene.is_selected(c2) && !ed.scene.is_selected(c3) && !ed.scene.is_selected(sp) &&
              ed.scene.active() == c1, "Shift + double-click selects the visible cubes, not the sphere (%d selected)", ed.scene.selected_count());
        ed.scene.clear_selection();
        CHECK(ed.select_similar(l1) == 2 && ed.scene.is_selected(l2) && !ed.scene.is_selected(l3), "point lights, not the spot light");
        // Edits: the active object's changes go to the others that have the same thing.
        ed.scene.clear_selection();
        ed.scene.select_add(c2);
        ed.scene.select_add(sp);
        ed.scene.select_add(c1);
        Object before = *ed.scene.get(c1);
        Object* a = ed.scene.get(c1);
        a->color = Vec3(0.1f, 0.9f, 0.2f);
        a->roughness = 0.13f;
        a->scale.y = 3.0f;
        ed.scene.add_module(c1, ModuleKind::RigidBody3D);
        a->modules.rigidbody3d.mass = 7.0f;
        ed.apply_edit_to_selection(before, *a);
        Object* b = ed.scene.get(c2);
        Object* s2 = ed.scene.get(sp);
        CHECK(b->color.y > 0.89f && s2->color.y > 0.89f && b->roughness == 0.13f && s2->roughness == 0.13f, "look goes to all");
        CHECK(b->scale.y == 3.0f && b->scale.x == 1.0f && b->position.x == 2.0f, "scale per axis, positions stay");
        CHECK(b->modules.has(ModuleKind::RigidBody3D) && b->modules.rigidbody3d.mass == 7.0f && s2->modules.has(ModuleKind::RigidBody3D),
              "an added module (with its values) goes to all");
        CHECK(!strcmp(b->name, "C2"), "names stay");
        before = *a;
        a->modules.rigidbody3d.mass = 2.0f;
        a->modules.rigidbody3d.velocity.x = 5.0f;
        b->modules.rigidbody3d.velocity.z = -4.0f;
        ed.apply_edit_to_selection(before, *a);
        CHECK(b->modules.rigidbody3d.mass == 2.0f && b->modules.rigidbody3d.velocity.x == 5.0f && b->modules.rigidbody3d.velocity.z == -4.0f,
              "a module field, a vector per component");
        // Through the Inspector: the Unlit switch with three selected.
        frames(2);
        bool was = ed.scene.get(c2)->unlit;
        CHECK(click_ui("inspector:unlit"), "the Unlit switch");
        frames(2);
        CHECK(ed.scene.get(c1)->unlit != was && ed.scene.get(c2)->unlit != was && ed.scene.get(sp)->unlit != was, "the Inspector edits all three");
        ed.new_scene();
        frames(2);
    }

    // ---- groups: the name list, the scene file, multi-edit, select by group, the Inspector, scripts in Play
    {
        ed.stop();
        ed.new_scene();
        char gl[256] = {};
        CHECK(name_list_add(gl, sizeof gl, "grass") && name_list_add(gl, sizeof gl, "Sway") && name_list_add(gl, sizeof gl, "SWAY") &&
              !strcmp(gl, "grass;Sway"), "add skips a name already there whatever its case (%s)", gl);
        CHECK(!name_list_add(gl, sizeof gl, "a;b") && !name_list_add(gl, sizeof gl, "") && !name_list_add(gl, sizeof gl, " x"), "bad names");
        CHECK(name_list_has(gl, "GRASS") && name_list_remove(gl, sizeof gl, "grass") && !strcmp(gl, "Sway") && !name_list_remove(gl, sizeof gl, "x"),
              "has / remove");
        char tiny[8] = {};
        CHECK(name_list_add(tiny, sizeof tiny, "abc") && !name_list_add(tiny, sizeof tiny, "defg") && !strcmp(tiny, "abc"), "a full list");
        ObjectId g1 = ed.create_object("G1", MeshKind::Cube, Vec3(0, 0.5f, 0));
        ObjectId g2 = ed.create_object("G2", MeshKind::Cube, Vec3(2, 0.5f, 0));
        ObjectId g3 = ed.create_object("G3", MeshKind::Sphere, Vec3(-2, 0.5f, 0));
        snprintf(ed.scene.get(g1)->groups, 256, "%s", "grass;sway");
        snprintf(ed.scene.get(g2)->groups, 256, "%s", "grass");
        snprintf(ed.scene.get(g3)->groups, 256, "%s", "rock");
        size_t n = 0;
        char* text = save_scene_text(ed.scene, ed.project_dir, &n);
        CHECK(text && strstr(text, "  groups \"grass;sway\"\n"), "the scene file writes groups");
        Scene back;
        SceneLoadReport rep;
        CHECK(text && load_scene_text(back, text, n, &rep) && rep.skipped == 0 && back.get(g1) && !strcmp(back.get(g1)->groups, "grass;sway") &&
              !strcmp(back.get(g3)->groups, "rock"), "and reads them back");
        free(text);
        CHECK(ed.select_group("GRASS") == 2 && ed.scene.is_selected(g1) && ed.scene.is_selected(g2) && !ed.scene.is_selected(g3), "select a group");
        CHECK(ed.select_group("rock", true) == 1 && ed.scene.selected_count() == 3, "Shift adds a group to the selection");
        // Multi-edit: groups added and removed on the active one go to the others, their other groups stay.
        ed.scene.clear_selection();
        ed.scene.select_add(g3);
        ed.scene.select_add(g2);
        ed.scene.select_add(g1);
        Object before = *ed.scene.get(g1);
        Object* a = ed.scene.get(g1);
        name_list_add(a->groups, sizeof a->groups, "tall");
        name_list_remove(a->groups, sizeof a->groups, "grass");
        ed.apply_edit_to_selection(before, *a);
        CHECK(!strcmp(ed.scene.get(g2)->groups, "tall") && !strcmp(ed.scene.get(g3)->groups, "rock;tall"),
              "multi-edit adds and removes the changed groups only (%s / %s)", ed.scene.get(g2)->groups, ed.scene.get(g3)->groups);
        // The Inspector's Groups field adds a group to all three.
        frames(2);
        const Editor::UiRect* gr = ui("inspector:groups");
        CHECK(gr != nullptr, "the Inspector has a Groups field");
        if (gr) {
            click(gr->x + 10, gr->y + gr->h * 0.5f);
            frames(1);
            type_text("bush");
            frames(1);
            ev_key(platform::KEY_ENTER, true, 0); frames(1);
            ev_key(platform::KEY_ENTER, false, 0); frames(2);
            CHECK(name_list_has(ed.scene.get(g1)->groups, "bush") && name_list_has(ed.scene.get(g2)->groups, "bush") &&
                  name_list_has(ed.scene.get(g3)->groups, "bush"), "typed into the Inspector, it goes to every selected object (%s)",
                  ed.scene.get(g2)->groups);
        }
        // Scripts while playing.
        ed.scene.clear_selection();
        ed.play();
        ScriptRuntime* rt = ed.play_scripts();
        CHECK(rt && rt->run("_G.ng = #fe.find_group('tall'); fe.find('G3'):add_group('found'); _G.first = fe.find_group('tall')[1].name"),
              "fe.find_group runs in Play");
        if (rt) {
            CHECK(rt->run("assert(ng == 3 and first == 'G1')"), "it finds the three, in Hierarchy order");
            CHECK(name_list_has(ed.scene.get(g3)->groups, "found"), "obj:add_group changes the object");
        }
        ed.stop();
        CHECK(!name_list_has(ed.scene.get(g3)->groups, "found"), "Stop puts the groups back");
        ed.new_scene();
        frames(2);
    }

    // ---- terrain: create, brush in the viewport, undo / redo, foliage, streaming, save and open
    {
        ed.stop();
        ed.new_scene();
        ObjectId t = ed.create_kind("Terrain", ObjectKind::Terrain, Vec3(0, 0, 0));
        Object* to = ed.scene.get(t);
        TerrainData* td = to ? terrain_data(*to) : nullptr;
        CHECK(to && to->modules.terrain.data[0] && td, "a new terrain has data (%s)", to ? to->modules.terrain.data : "");
        CHECK(td && td->chunks_x == 8 && td->chunks_z == 8 && td->cells == 32, "256 m in chunks of 32 m");
        CHECK(to && to->position.x == -128.0f && to->position.z == -128.0f, "centred on where it was made");
        ed.history_reset();
        ed.scene.select_only(t);
        ed.tool = Tool::Select;
        ed.terrain_tool = Editor::TerrainTool::Raise;
        ed.terrain_radius = 12.0f;
        ed.terrain_strength = 1.0f;
        ed.camera.target = Vec3(0, 0, 0);
        ed.camera.yaw = 0;
        ed.camera.pitch = 50;
        ed.camera.distance = 60;
        frames(3);
        float cx = ed.view_rect[0] + ed.view_rect[2] * 0.5f, cy = ed.view_rect[1] + ed.view_rect[3] * 0.5f;
        drag(cx - 5, cy, cx + 5, cy, 0, 30);
        frames(2);
        float raised = td->height_at(128, 128);
        CHECK(raised > 0.5f, "dragging with Add raises the ground (%.3f)", raised);
        CHECK(ed.scene.selected_count() == 1 && ed.scene.active() == t, "the brush does not change the selection");
        int rev = to->modules.terrain.revision;
        CHECK(rev == td->revision && ed.can_undo(), "the stroke is a history step (revision %d)", rev);
        ed.undo();
        frames(2);
        to = ed.scene.get(t);
        td = to ? terrain_data(*to) : nullptr;
        CHECK(td && absf_(td->height_at(128, 128)) < 1e-4f, "undo flattens it again (%.3f)", td ? td->height_at(128, 128) : -1.0f);
        ed.redo();
        frames(2);
        to = ed.scene.get(t);
        td = to ? terrain_data(*to) : nullptr;
        CHECK(td && absf_(td->height_at(128, 128) - raised) < 1e-4f, "redo raises it again");
        // Shift with Add lowers.
        ev_key(platform::KEY_LSHIFT, true, platform::MOD_SHIFT); frames(1);
        drag(cx - 5, cy, cx + 5, cy, 0, 30);
        ev_key(platform::KEY_LSHIFT, false, 0);
        frames(2);
        CHECK(td->height_at(128, 128) < raised - 0.2f, "Shift turns Add into Remove (%.3f)", td->height_at(128, 128));
        // Level to 2 m, then a click selects the terrain with no tool.
        ed.terrain_tool = Editor::TerrainTool::Level;
        ed.terrain_level = 2.0f;
        drag(cx - 3, cy, cx + 3, cy, 0, 60);
        frames(2);
        CHECK(absf_(td->height_at(128, 128) - 2.0f) < 0.1f, "Level flattens to 2 m (%.3f)", td->height_at(128, 128));
        ed.terrain_tool = Editor::TerrainTool::None;
        ed.scene.clear_selection();
        frames(2);
        click(cx, cy);
        CHECK(ed.scene.active() == t, "a click on the ground selects the terrain");
        // Foliage: a layer with a model, painted, drawn by the render bridge in batches.
        {
            size_t n = 0;
            void* m = platform::read_file("assets/samples/cube_textured.mof", &n);
            char mp[512];
            snprintf(mp, sizeof mp, "%s/grass.mof", ed.project_dir);
            if (m) platform::write_file(mp, m, n);
            free(m);
            snprintf(to->modules.terrain.foliage[0].model, kPathMax, "grass.mof");
            to->modules.terrain.foliage[0].density = 0.5f;
            to->modules.terrain.foliage[0].scale_min = 0.1f;
            to->modules.terrain.foliage[0].scale_max = 0.2f;
            ed.scene.select_only(t);
            ed.terrain_tool = Editor::TerrainTool::Foliage;
            ed.terrain_radius = 20.0f;
            drag(cx - 3, cy, cx + 3, cy, 0, 40);
            frames(2);
            CHECK(td->has_foliage(4, 4, 0) || td->has_foliage(3, 3, 0), "the Foliage tool paints the layer");
            ed.terrain_tool = Editor::TerrainTool::None;
            RenderBridge rb;
            rb.terrain.build_budget_ms = 1e9;
            Mat4 view = Mat4::look_at(Vec3(0, 20, 40), Vec3(0, 0, 0), Vec3(0, 1, 0));
            Mat4 proj = Mat4::perspective(radians(60.0f), 1.5f, 0.1f, 500.0f);
            rb.build(ed.scene, view, proj, Vec3(0, 20, 40), 0.1f, 500.0f);
            const TerrainViewStats& st = rb.terrain.stats();
            CHECK(st.chunks_drawn == 64 && st.foliage_batches > 0 && st.foliage_instances > 50,
                  "64 chunks and foliage batches (%d chunks, %d batches, %ld instances)", st.chunks_drawn, st.foliage_batches, st.foliage_instances);
            int fol = 0;
            for (int i = 0; i < rb.scene.item_count; i++) fol += rb.scene.items[i].foliage;
            CHECK(fol == st.foliage_batches, "foliage items are marked");
            // Several models in one layer: a folder of them, each instance picks one.
            {
                long one_model = st.foliage_instances;
                char dirp[512];
                snprintf(dirp, sizeof dirp, "%s/plants", ed.project_dir);
                platform::make_dir(dirp);
                size_t n2 = 0;
                void* m2 = platform::read_file("assets/samples/cube_textured.mof", &n2);
                for (int k = 0; k < 3; k++) {
                    char fp[600];
                    snprintf(fp, sizeof fp, "%s/plant_%d.mof", dirp, k);
                    if (m2) platform::write_file(fp, m2, n2);
                }
                free(m2);
                to->modules.terrain.foliage[0].model[0] = 0;
                CHECK(ed.foliage_add_model(t, 0, "plants"), "a folder adds its models");
                CHECK(!ed.foliage_add_model(t, 0, "plants/plant_1.mof"), "one already there is not added twice");
                CHECK(ed.foliage_add_model(t, 0, "grass.mof") && !ed.foliage_add_model(t, 0, "notes.txt"), "a .mof is added, other files are not");
                CHECK(!strcmp(to->modules.terrain.foliage[0].model, "plants/plant_0.mof;plants/plant_1.mof;plants/plant_2.mof;grass.mof"),
                      "the list (%s)", to->modules.terrain.foliage[0].model);
                rb.build(ed.scene, view, proj, Vec3(0, 20, 40), 0.1f, 500.0f);
                int nb = 0;
                for (int i = 0; i < rb.scene.item_count; i++) nb += rb.scene.items[i].foliage;
                CHECK(rb.terrain.stats().foliage_batches > st.foliage_batches || nb >= 4, "the models get batches of their own (%d)", nb);
                long many = rb.terrain.stats().foliage_instances;
                CHECK(many >= one_model * 9 / 10 && many <= one_model * 11 / 10 + 8, "about as many instances, spread over the models (%ld vs %ld)", many, one_model);
                // Back to one model for the rest.
                snprintf(to->modules.terrain.foliage[0].model, kPathListMax, "grass.mof");
            }
            // Far away, the same batch draws fewer instances (a prefix), and none past the distance.
            to->modules.terrain.foliage[0].distance = 400.0f;
            rb.build(ed.scene, view, proj, Vec3(0, 20, 40), 0.1f, 500.0f);
            long near_n = rb.terrain.stats().foliage_instances;
            rb.build(ed.scene, view, proj, Vec3(0, 20, 300), 0.1f, 500.0f);
            long far_n = rb.terrain.stats().foliage_instances;
            CHECK(far_n < near_n && far_n > 0, "fewer instances far away (%ld < %ld)", far_n, near_n);
            rb.build(ed.scene, view, proj, Vec3(0, 20, 800), 0.1f, 1000.0f);
            CHECK(rb.terrain.stats().foliage_instances == 0, "none past the distance");
            // Model LOD: past lod_distance a block draws grass_lod1.mof (another model: other vertices per instance).
            {
                size_t n3 = 0;
                void* m3 = platform::read_file("assets/samples/skinned_bar.mof", &n3);
                char lp[600];
                snprintf(lp, sizeof lp, "%s/grass_lod1.mof", ed.project_dir);
                if (m3) platform::write_file(lp, m3, n3);
                free(m3);
                to->modules.terrain.foliage[0].lod_distance = 60.0f;
                auto per_instance = [&](Vec3 eye) {
                    rb.build(ed.scene, Mat4::look_at(eye, Vec3(0, 0, 0), Vec3(0, 1, 0)), proj, eye, 0.1f, 1000.0f);
                    uint32_t near_v = 0, far_v = 0;
                    for (int i = 0; i < rb.scene.item_count; i++)
                        if (rb.scene.items[i].foliage) { (near_v ? far_v : near_v) = rb.scene.items[i].mesh.instance_vertices; }
                    return near_v;
                };
                uint32_t v_near = per_instance(Vec3(0, 20, 40)), v_far = per_instance(Vec3(0, 20, 300));
                CHECK(v_near && v_far && v_near != v_far, "a far block draws the LOD model (%u vs %u vertices per instance)", v_far, v_near);
                to->modules.terrain.foliage[0].lod_distance = 0.0f;
                CHECK(per_instance(Vec3(0, 20, 300)) == v_near, "LOD distance 0: the full model everywhere");
            }
            // The eighth layer is saved and read back with the scene; a heightmap (8 bit grey PGM) imports.
            {
                snprintf(to->modules.terrain.foliage[7].name, kPathMax, "rocks");
                to->modules.terrain.foliage[7].lod_distance = 120.0f;
                size_t tn = 0;
                char* text = save_scene_text(ed.scene, ed.project_dir, &tn);
                Scene back;
                SceneLoadReport rep;
                Object* bt = text && load_scene_text(back, text, tn, &rep) ? back.get(t) : nullptr;
                CHECK(bt && !strcmp(bt->modules.terrain.foliage[7].name, "rocks") && bt->modules.terrain.foliage[7].lod_distance == 120.0f &&
                      rep.skipped == 0, "foliage layer 8 round trips");
                free(text);
                const unsigned char pgm[] = { 'P', '5', '\n', '2', ' ', '2', '\n', '2', '5', '5', '\n', 0, 255, 0, 255 };
                char hp[600];
                snprintf(hp, sizeof hp, "%s/hm.pgm", ed.project_dir);
                platform::write_file(hp, pgm, sizeof pgm);
                ed.terrain_import_low = 10.0f;
                ed.terrain_import_high = 30.0f;
                TerrainData* hd = terrain_data(*ed.scene.get(t));
                int rev = hd ? hd->revision : 0;
                CHECK(ed.import_heightmap(t, hp), "Import heightmap");
                Vec3 org = terrain_origin(ed.scene, *ed.scene.get(t));
                CHECK(hd && absf_(hd->height_at(0, 5) + org.y - 10.0f) < 0.05f && absf_(hd->height_at(hd->size_x(), 5) + org.y - 30.0f) < 0.05f,
                      "black at the low height, white at the high one (%.2f, %.2f)", hd ? hd->height_at(0, 5) + org.y : 0.0f,
                      hd ? hd->height_at(hd->size_x(), 5) + org.y : 0.0f);
                CHECK(hd && hd->goto_revision(rev), "and it is one undo step");
                ed.scene.get(t)->modules.terrain.revision = rev;
                snprintf(to->modules.terrain.foliage[7].name, kPathMax, "%s", "");
            }
            // Streaming: chunks within the load distance, Dynamic objects with them.
            ObjectId near_c = ed.create_object("Near", MeshKind::Cube, Vec3(0, 3, 0));
            ObjectId far_c = ed.create_object("Far", MeshKind::Cube, Vec3(120, 3, 120));
            ObjectId off_c = ed.create_object("Off", MeshKind::Cube, Vec3(500, 3, 500));
            ObjectId unl = ed.create_object("Later", MeshKind::Cube, Vec3(2, 3, 0));
            ed.scene.get(unl)->streaming = StreamMode::Unloaded;
            to = ed.scene.get(t);
            to->modules.terrain.load_distance = 40.0f;
            rb.playing = true;
            rb.build(ed.scene, view, proj, Vec3(0, 20, 0), 0.1f, 500.0f);
            CHECK(rb.terrain.stats().chunks_unloaded > 40 && rb.terrain.stats().chunks_drawn < 24, "far chunks unload (%d drawn)", rb.terrain.stats().chunks_drawn);
            CHECK(rb.unloaded == 2, "the far cube and the Unloaded one are out (%d)", rb.unloaded);
            CHECK(rb.terrain.loaded(ed.scene, *ed.scene.get(near_c)) && !rb.terrain.loaded(ed.scene, *ed.scene.get(far_c)) &&
                  rb.terrain.loaded(ed.scene, *ed.scene.get(off_c)), "near loaded, far not, off the terrain always");
            ed.scene.get(far_c)->streaming = StreamMode::AlwaysLoaded;
            ed.scene.get(unl)->stream_loaded = true;
            rb.build(ed.scene, view, proj, Vec3(0, 20, 0), 0.1f, 500.0f);
            CHECK(rb.unloaded == 0, "Always loaded, and a loaded Unloaded object (%d)", rb.unloaded);
            rb.playing = false;
            rb.build(ed.scene, view, proj, Vec3(0, 20, 0), 0.1f, 500.0f);
            CHECK(rb.terrain.stats().chunks_unloaded == 0, "the editor shows every chunk unless stream_in_editor");
            ed.scene.remove(near_c); ed.scene.remove(far_c); ed.scene.remove(off_c); ed.scene.remove(unl);
            to = ed.scene.get(t);
            to->modules.terrain.load_distance = 250.0f;
        }
        // Play: a body dropped on the terrain lands on it.
        {
            float gh = td->height_at(128, 128);
            ObjectId bx = ed.create_object("Dropped", MeshKind::Cube, Vec3(0, gh + 6.0f, 0));
            ed.scene.add_module(bx, ModuleKind::RigidBody3D);
            ed.scene.add_module(bx, ModuleKind::Collider3D);
            ed.play();
            frames(150);
            Object* bo = ed.scene.get(bx);
            CHECK(ed.playing() && bo && absf_(bo->position.y - (gh + 0.5f)) < 0.1f, "in Play a body lands on the terrain (y %.2f, ground %.2f)",
                  bo ? bo->position.y : -1.0f, gh);
            ed.stop();
            frames(2);
            ed.scene.remove(bx);
            to = ed.scene.get(t);
            td = terrain_data(*to);
        }
        // Save, open: the data file comes back with the scene.
        float hs = td->height_at(128, 128);
        uint8_t dens = td->density(0, 128, 128);
        CHECK(ed.save_scene("scenes/terrain.fscene"), "save the terrain scene");
        char tf[512];
        snprintf(tf, sizeof tf, "%s/%s", ed.project_dir, to->modules.terrain.data);
        CHECK(platform::file_exists(tf), "the data file is written (%s)", tf);
        CHECK(!terrain_store_unsaved(), "nothing left unsaved");
        ed.new_scene();
        CHECK(ed.open_scene("scenes/terrain.fscene"), "open it again");
        Object* back = nullptr;
        for (int i = 0; i < ed.scene.count(); i++) if (ed.scene.at(i).kind == ObjectKind::Terrain) back = &ed.scene.at(i);
        TerrainData* bd = back ? terrain_data(*back) : nullptr;
        CHECK(bd && absf_(bd->height_at(128, 128) - hs) < 1e-5f && bd->density(0, 128, 128) == dens, "heights and foliage are back");
        // A duplicate gets its own data.
        if (back) {
            ObjectId dup = ed.duplicate_tree(back->id, kNoObject, true);
            Object* d = ed.scene.get(dup);
            CHECK(d && strcmp(d->modules.terrain.data, back->modules.terrain.data) != 0 && terrain_data(*d) && terrain_data(*d) != bd,
                  "a duplicated terrain has its own data file");
        }
        ed.new_scene();
        frames(2);
    }

    printf("editor_tests: %d checks, %d failed\n", g_checks, g_failed);
    ed.shutdown();
    return g_failed ? 1 : 0;
}
