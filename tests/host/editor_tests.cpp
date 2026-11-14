// Editor interaction tests on the headless platform: mouse and keyboard
// events go in, the editor's state is checked after a few frames.
#include "editor.h"
#include "platform/headless.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imguizmo/ImGuizmo.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

using namespace fe;

static int g_checks, g_failed;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

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

static const Editor::TileRect* tile(const char* name) {
    for (int i = 0; i < E->file_tile_count; i++)
        if (!strcmp(path_filename(E->file_tiles[i].path), name)) return &E->file_tiles[i];
    return nullptr;
}

int main() {
    const char* dir = "build/host/test-project";
    mkdir("build/host", 0755);
    mkdir(dir, 0755);
    platform::write_file("build/host/test-project/crate.png", "x", 1);
    platform::write_file("build/host/test-project/ship.mof", "x", 1);
    platform::write_file("build/host/test-project/player.lua", "x", 1);

    static Editor ed;
    E = &ed;
    CHECK(ed.init(1280, 800), "init");
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
    ImGuiWindow* hw = ImGui::FindWindowByName("Hierarchy");
    CHECK(hw != nullptr, "hierarchy window exists");
    if (hw) {
        float row_h = ImGui::GetTextLineHeight() + 8 + ImGui::GetStyle().ItemSpacing.y;
        float x = hw->InnerRect.Min.x + 40;
        float y0 = hw->InnerRect.Min.y + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2 + 8;
        ed.scene.clear_selection();
        frames(1);
        click(x, y0 + row_h * 0.5f);
        CHECK(ed.scene.selected_count() == 1 && ed.scene.is_selected(a), "clicking the first row selects A");
        shift_click(x, y0 + row_h * 1.5f);
        CHECK(ed.scene.selected_count() == 2, "shift-clicking the second row adds B (count %d)", ed.scene.selected_count());
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
        if (g.OpenPopupStack.Size == 1 && g.OpenPopupStack[0].Window) {
            ImGuiWindow* pw = g.OpenPopupStack[0].Window;
            float lh = ImGui::GetTextLineHeight(), sp = ImGui::GetStyle().ItemSpacing.y;
            float iy = pw->Pos.y + ImGui::GetStyle().WindowPadding.y + (lh + sp) * 1 + lh * 0.5f;
            click(pw->Pos.x + 40, iy);
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

    // ---- panels can be dragged out of the dock
    ImGuiWindow* iw = ImGui::FindWindowByName("Inspector");
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

    printf("editor_tests: %d checks, %d failed\n", g_checks, g_failed);
    ed.shutdown();
    return g_failed ? 1 : 0;
}
