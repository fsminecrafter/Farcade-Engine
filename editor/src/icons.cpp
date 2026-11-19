#include "icons.h"
#include "icon_font.h"
#include "imgui/imgui_internal.h"

#include "core/math.h"
#include "widgets.h"

namespace fe {

// The vector icon (editor/icons/<name>.svg) for each Icon, in enum order.
static const char* const kIconFile[] = {
    "pointer", "move", "rotate", "scale", "world", "local",
    "magnet", "grid", "play", "pause", "folder", "file",
    "image", "model", "script", "cube", "sphere", "cylinder",
    "plane", "up", "refresh", "trash", "info", "warning",
    "error", "stop", "eye", "eye_off", "plus", "search",
    "gear", "chevron_down", "chevron_right", "close", "sun", "monitor",
    "layers", "camera", "bulb", "cloud", "speaker", "wave",
    "body", "collider", "ear", "wand", "empty", "star",
    "sparks", "joint", "clip", "wind", "spawn", "reverb",
    "sprite", "text_t", "fullscreen", "fullscreen_exit",
};
static_assert(sizeof(kIconFile) / sizeof(kIconFile[0]) == (size_t)Icon::Count_, "one file per Icon");

// The icon's character in the vector icon font; 0 = none (draw the lines below).
static unsigned icon_glyph(Icon icon) {
    static unsigned cps[(size_t)Icon::Count_];
    static bool done = false;
    if (!done) {
        for (size_t i = 0; i < (size_t)Icon::Count_; i++) cps[i] = icon_codepoint(kIconFile[i]);
        done = true;
    }
    return (size_t)icon < (size_t)Icon::Count_ ? cps[(size_t)icon] : 0;
}

static void draw_icon_lines(ImDrawList* dl, Icon icon, ImVec2 p, float s, ImU32 c);

void draw_icon(ImDrawList* dl, Icon icon, ImVec2 p, float s, ImU32 c) {
    unsigned cp = icon_glyph(icon);
    ImFont* font = icon_box_font();
    if (cp && font && s >= 4.0f) {
        // Whole pixels and a whole size: the glyph is rasterized for exactly this box.
        char utf8[5] = {};
        ImTextCharToUtf8(utf8, cp);
        float size = (float)(int)(s + 0.5f);
        ImVec2 at((float)(int)(p.x + (s - size) * 0.5f + 0.5f), (float)(int)(p.y + (s - size) * 0.5f + 0.5f));
        dl->AddText(font, size, at, c, utf8);
        return;
    }
    draw_icon_lines(dl, icon, p, s, c);
}

// The original line drawings: used when the icon font is not there.
static void draw_icon_lines(ImDrawList* dl, Icon icon, ImVec2 p, float s, ImU32 c) {
    // Work in a 16x16 design grid scaled to the box.
    float k = s / 16.0f;
    float t = 1.5f * k;     // stroke width
    auto P = [&](float x, float y) { return ImVec2(p.x + x * k, p.y + y * k); };
    switch (icon) {
        case Icon::Pointer: {
            ImVec2 pts[] = { P(4, 2), P(4, 13), P(7, 10), P(9.5f, 14.5f), P(11, 13.8f), P(8.6f, 9.4f), P(12.5f, 9.4f) };
            dl->AddPolyline(pts, 7, c, ImDrawFlags_Closed, t);
            break;
        }
        case Icon::Move: {
            dl->AddLine(P(8, 2), P(8, 14), c, t);
            dl->AddLine(P(2, 8), P(14, 8), c, t);
            dl->AddTriangleFilled(P(8, 1), P(6, 4), P(10, 4), c);
            dl->AddTriangleFilled(P(8, 15), P(6, 12), P(10, 12), c);
            dl->AddTriangleFilled(P(1, 8), P(4, 6), P(4, 10), c);
            dl->AddTriangleFilled(P(15, 8), P(12, 6), P(12, 10), c);
            break;
        }
        case Icon::Rotate: {
            dl->PathArcTo(P(8, 8), 5.5f * k, radians(-60), radians(220), 16);
            dl->PathStroke(c, 0, t);
            ImVec2 tip = P(8 + 5.5f * fcos(radians(-60)), 8 + 5.5f * fsin(radians(-60)));
            dl->AddTriangleFilled(ImVec2(tip.x - 2.5f * k, tip.y - 2.0f * k), ImVec2(tip.x + 2.8f * k, tip.y - 0.5f * k),
                                  ImVec2(tip.x - 0.5f * k, tip.y + 2.8f * k), c);
            break;
        }
        case Icon::Scale: {
            dl->AddRect(P(2.5f, 6.5f), P(9.5f, 13.5f), c, 1.5f * k, 0, t);
            dl->AddLine(P(8, 8), P(13.5f, 2.5f), c, t);
            dl->AddLine(P(10, 2.5f), P(13.5f, 2.5f), c, t);
            dl->AddLine(P(13.5f, 2.5f), P(13.5f, 6), c, t);
            break;
        }
        case Icon::World: {
            dl->AddCircle(P(8, 8), 6 * k, c, 20, t);
            dl->AddEllipse(P(8, 8), ImVec2(2.6f * k, 6 * k), c, 0, 16, t);
            dl->AddLine(P(2, 8), P(14, 8), c, t);
            break;
        }
        case Icon::Local: {
            dl->AddLine(P(3, 13), P(3, 3), c, t);
            dl->AddLine(P(3, 13), P(13, 13), c, t);
            dl->AddLine(P(3, 13), P(9.5f, 7.5f), c, t);
            dl->AddCircleFilled(P(3, 13), 1.6f * k, c);
            break;
        }
        case Icon::Magnet: {
            dl->PathArcTo(P(8, 8), 4.5f * k, radians(0), radians(180), 12);
            dl->PathStroke(c, 0, 2.6f * k);
            dl->AddLine(P(3.5f, 8), P(3.5f, 3), c, 2.6f * k);
            dl->AddLine(P(12.5f, 8), P(12.5f, 3), c, 2.6f * k);
            dl->AddLine(P(2.2f, 4.5f), P(4.8f, 4.5f), IM_COL32(0, 0, 0, 90), 1.0f * k);
            dl->AddLine(P(11.2f, 4.5f), P(13.8f, 4.5f), IM_COL32(0, 0, 0, 90), 1.0f * k);
            break;
        }
        case Icon::Grid: {
            for (int i = 0; i < 4; i++) {
                float v = 2.5f + i * 3.66f;
                dl->AddLine(P(v, 2.5f), P(v, 13.5f), c, k);
                dl->AddLine(P(2.5f, v), P(13.5f, v), c, k);
            }
            break;
        }
        case Icon::Play: dl->AddTriangleFilled(P(4.5f, 3), P(4.5f, 13), P(13, 8), c); break;
        case Icon::Pause:
            dl->AddRectFilled(P(4, 3), P(6.8f, 13), c, 1.0f * k);
            dl->AddRectFilled(P(9.2f, 3), P(12, 13), c, 1.0f * k);
            break;
        case Icon::Folder:
            dl->AddRectFilled(P(1.5f, 3.5f), P(7, 6), c, 1.5f * k);
            dl->AddRectFilled(P(1.5f, 5), P(14.5f, 13), c, 2.0f * k);
            break;
        case Icon::File: case Icon::Script: case Icon::Image: case Icon::Model: {
            ImVec2 pts[] = { P(3.5f, 1.5f), P(9.5f, 1.5f), P(12.5f, 4.5f), P(12.5f, 14.5f), P(3.5f, 14.5f) };
            dl->AddPolyline(pts, 5, c, ImDrawFlags_Closed, t * 0.85f);
            if (icon == Icon::Script) {
                dl->AddLine(P(5.5f, 7), P(10.5f, 7), c, k);
                dl->AddLine(P(5.5f, 9.5f), P(9, 9.5f), c, k);
                dl->AddLine(P(5.5f, 12), P(10, 12), c, k);
            } else if (icon == Icon::Image) {
                dl->AddTriangleFilled(P(5, 12.5f), P(7.5f, 8.5f), P(10, 12.5f), c);
                dl->AddCircleFilled(P(9.5f, 7), 1.2f * k, c);
            } else if (icon == Icon::Model) {
                ImVec2 cube[] = { P(8, 6), P(10.8f, 7.5f), P(10.8f, 11), P(8, 12.5f), P(5.2f, 11), P(5.2f, 7.5f) };
                dl->AddPolyline(cube, 6, c, ImDrawFlags_Closed, k);
                dl->AddLine(P(8, 9.2f), P(8, 12.5f), c, k);
                dl->AddLine(P(8, 9.2f), P(5.2f, 7.5f), c, k);
                dl->AddLine(P(8, 9.2f), P(10.8f, 7.5f), c, k);
            }
            break;
        }
        case Icon::Cube: {
            ImVec2 pts[] = { P(8, 2), P(13.5f, 5), P(13.5f, 11), P(8, 14), P(2.5f, 11), P(2.5f, 5) };
            dl->AddPolyline(pts, 6, c, ImDrawFlags_Closed, t);
            dl->AddLine(P(8, 8), P(8, 14), c, t);
            dl->AddLine(P(8, 8), P(2.5f, 5), c, t);
            dl->AddLine(P(8, 8), P(13.5f, 5), c, t);
            break;
        }
        case Icon::Sphere:
            dl->AddCircle(P(8, 8), 6 * k, c, 20, t);
            dl->AddEllipse(P(8, 8), ImVec2(6 * k, 2.2f * k), c, 0, 16, k);
            break;
        case Icon::Cylinder:
            dl->AddEllipse(P(8, 4), ImVec2(5 * k, 2 * k), c, 0, 16, t);
            dl->AddLine(P(3, 4), P(3, 12), c, t);
            dl->AddLine(P(13, 4), P(13, 12), c, t);
            dl->PathArcTo(P(8, 12), 5 * k, 0, kPi, 12);
            dl->PathStroke(c, 0, t);
            break;
        case Icon::Plane: {
            ImVec2 pts[] = { P(1.5f, 10), P(7, 6), P(14.5f, 6), P(9, 10) };
            dl->AddPolyline(pts, 4, c, ImDrawFlags_Closed, t);
            break;
        }
        case Icon::Up:
            dl->AddLine(P(8, 13), P(8, 3.5f), c, t);
            dl->AddLine(P(8, 3), P(4, 7), c, t);
            dl->AddLine(P(8, 3), P(12, 7), c, t);
            break;
        case Icon::Refresh:
            dl->PathArcTo(P(8, 8), 5 * k, radians(30), radians(320), 16);
            dl->PathStroke(c, 0, t);
            dl->AddTriangleFilled(P(12.5f, 2), P(13.5f, 6.5f), P(9, 5.5f), c);
            break;
        case Icon::Trash:
            dl->AddLine(P(2.5f, 4), P(13.5f, 4), c, t);
            dl->AddRect(P(4, 4), P(12, 14), c, 1.5f * k, 0, t);
            dl->AddLine(P(6.5f, 2), P(9.5f, 2), c, t);
            break;
        case Icon::Info:
            dl->AddCircleFilled(P(8, 8), 6 * k, c, 20);
            dl->AddRectFilled(P(7.2f, 7), P(8.8f, 11.5f), IM_COL32(0, 0, 0, 200));
            dl->AddCircleFilled(P(8, 5), 1.0f * k, IM_COL32(0, 0, 0, 200));
            break;
        case Icon::Warning:
            dl->AddTriangleFilled(P(8, 2), P(14.5f, 13.5f), P(1.5f, 13.5f), c);
            dl->AddRectFilled(P(7.3f, 6), P(8.7f, 10), IM_COL32(0, 0, 0, 200));
            dl->AddCircleFilled(P(8, 11.8f), 0.9f * k, IM_COL32(0, 0, 0, 200));
            break;
        case Icon::Error:
            dl->AddCircleFilled(P(8, 8), 6 * k, c, 20);
            dl->AddLine(P(5.5f, 5.5f), P(10.5f, 10.5f), IM_COL32(0, 0, 0, 200), t);
            dl->AddLine(P(10.5f, 5.5f), P(5.5f, 10.5f), IM_COL32(0, 0, 0, 200), t);
            break;
        case Icon::Stop: dl->AddRectFilled(P(4, 4), P(12, 12), c, 1.5f * k); break;
        case Icon::Eye:
        case Icon::EyeOff: {
            dl->AddEllipse(P(8, 8), ImVec2(6.5f * k, 4.0f * k), c, 0, 20, t);
            dl->AddCircleFilled(P(8, 8), 2.0f * k, c, 12);
            if (icon == Icon::EyeOff) dl->AddLine(P(2.5f, 13.5f), P(13.5f, 2.5f), c, t);
            break;
        }
        case Icon::Plus:
            dl->AddLine(P(8, 3), P(8, 13), c, t * 1.1f);
            dl->AddLine(P(3, 8), P(13, 8), c, t * 1.1f);
            break;
        case Icon::Search:
            dl->AddCircle(P(7, 7), 4.2f * k, c, 16, t);
            dl->AddLine(P(10.2f, 10.2f), P(13.5f, 13.5f), c, t * 1.2f);
            break;
        case Icon::Gear: {
            const int teeth = 8;
            for (int i = 0; i < teeth; i++) {
                float a = 2 * kPi * i / teeth;
                ImVec2 d(fcos(a), fsin(a));
                dl->AddLine(P(8 + d.x * 4.5f, 8 + d.y * 4.5f), P(8 + d.x * 6.6f, 8 + d.y * 6.6f), c, 2.2f * k);
            }
            dl->AddCircle(P(8, 8), 4.6f * k, c, 16, t);
            dl->AddCircle(P(8, 8), 1.8f * k, c, 10, t);
            break;
        }
        case Icon::ChevronDown:
            dl->AddLine(P(4, 6), P(8, 10), c, t);
            dl->AddLine(P(8, 10), P(12, 6), c, t);
            break;
        case Icon::ChevronRight:
            dl->AddLine(P(6, 4), P(10, 8), c, t);
            dl->AddLine(P(10, 8), P(6, 12), c, t);
            break;
        case Icon::Close:
            dl->AddLine(P(4.5f, 4.5f), P(11.5f, 11.5f), c, t);
            dl->AddLine(P(11.5f, 4.5f), P(4.5f, 11.5f), c, t);
            break;
        case Icon::Camera:
            dl->AddRect(P(1.5f, 4.5f), P(10.5f, 12.5f), c, 1.5f * k, 0, t);
            { ImVec2 lens[] = { P(10.5f, 7), P(14.5f, 5), P(14.5f, 12), P(10.5f, 10) };
              dl->AddPolyline(lens, 4, c, ImDrawFlags_Closed, t); }
            break;
        case Icon::Bulb:
            dl->AddCircle(P(8, 6.5f), 4.3f * k, c, 16, t);
            dl->AddLine(P(6, 11), P(10, 11), c, t);
            dl->AddLine(P(6.5f, 13.5f), P(9.5f, 13.5f), c, t);
            dl->AddLine(P(6.3f, 9.8f), P(6.3f, 11), c, t);
            dl->AddLine(P(9.7f, 9.8f), P(9.7f, 11), c, t);
            break;
        case Icon::Cloud:
            dl->AddCircle(P(5.5f, 9.5f), 3.0f * k, c, 14, t);
            dl->AddCircle(P(9, 7), 3.8f * k, c, 16, t);
            dl->AddCircle(P(12, 10), 2.6f * k, c, 12, t);
            break;
        case Icon::Speaker: {
            ImVec2 sp[] = { P(2, 6), P(5, 6), P(9, 2.5f), P(9, 13.5f), P(5, 10), P(2, 10) };
            dl->AddPolyline(sp, 6, c, ImDrawFlags_Closed, t);
            dl->PathArcTo(P(9, 8), 3.5f * k, radians(-50), radians(50), 8);
            dl->PathStroke(c, 0, t);
            dl->PathArcTo(P(9, 8), 6.0f * k, radians(-50), radians(50), 10);
            dl->PathStroke(c, 0, t);
            break;
        }
        case Icon::Wave:
            for (int i = 0; i < 7; i++) {
                static const float hgt[7] = { 2, 5, 9, 6, 11, 4, 2 };
                float x = 2 + i * 2.0f;
                dl->AddLine(P(x, 8 - hgt[i] * 0.5f), P(x, 8 + hgt[i] * 0.5f), c, t);
            }
            break;
        case Icon::Body:
            dl->AddCircleFilled(P(8, 6.5f), 3.5f * k, c, 14);
            dl->AddLine(P(8, 11), P(8, 14.5f), c, t);
            dl->AddTriangleFilled(P(8, 15.5f), P(6, 12.5f), P(10, 12.5f), c);
            break;
        case Icon::Collider: {
            ImVec2 a[] = { P(8, 1.5f), P(14, 4.8f), P(14, 11.2f), P(8, 14.5f), P(2, 11.2f), P(2, 4.8f) };
            for (int i = 0; i < 6; i++) {
                ImVec2 q = a[i], r = a[(i + 1) % 6];
                for (int d = 0; d < 4; d++) {
                    float u0 = d / 4.0f, u1 = u0 + 0.13f;
                    dl->AddLine(ImVec2(q.x + (r.x - q.x) * u0, q.y + (r.y - q.y) * u0), ImVec2(q.x + (r.x - q.x) * u1, q.y + (r.y - q.y) * u1), c, t);
                }
            }
            break;
        }
        case Icon::Ear:
            dl->PathArcTo(P(8.5f, 6.5f), 4.5f * k, radians(180), radians(400), 16);
            dl->PathStroke(c, 0, t);
            dl->AddLine(P(11.9f, 9.4f), P(8, 14), c, t);
            dl->AddLine(P(8, 14), P(6, 13), c, t);
            break;
        case Icon::Wand:
            dl->AddLine(P(2.5f, 13.5f), P(10, 6), c, 2.0f * k);
            dl->AddLine(P(12, 1.5f), P(12, 5.5f), c, t);
            dl->AddLine(P(10, 3.5f), P(14, 3.5f), c, t);
            dl->AddCircleFilled(P(13.5f, 9.5f), 1.0f * k, c);
            dl->AddCircleFilled(P(6.5f, 2.5f), 0.9f * k, c);
            break;
        case Icon::Empty:
            dl->AddLine(P(8, 2), P(8, 14), c, t);
            dl->AddLine(P(2, 8), P(14, 8), c, t);
            dl->AddCircle(P(8, 8), 3.0f * k, c, 12, t);
            break;
        case Icon::Sparks: {
            static const float pts[7][3] = { { 8, 3, 1.4f }, { 4, 7, 1.0f }, { 12, 6, 1.1f }, { 6, 11, 1.2f }, { 11, 11, 0.9f }, { 8, 14, 0.8f }, { 2.5f, 3.5f, 0.7f } };
            for (auto& q : pts) dl->AddCircleFilled(P(q[0], q[1]), q[2] * k, c, 8);
            break;
        }
        case Icon::Joint:
            dl->AddCircle(P(4, 8), 2.6f * k, c, 12, t);
            dl->AddCircle(P(12, 8), 2.6f * k, c, 12, t);
            dl->AddLine(P(6.6f, 8), P(9.4f, 8), c, t);
            break;
        case Icon::Clip:
            dl->AddRect(P(2, 3), P(14, 13), c, 1.5f * k, 0, t);
            dl->AddTriangleFilled(P(6.5f, 5.5f), P(6.5f, 10.5f), P(10.8f, 8), c);
            break;
        case Icon::Wind:
            for (int i = 0; i < 3; i++) {
                float y = 4.5f + i * 3.5f;
                dl->AddLine(P(2, y), P(11 - i * 1.5f, y), c, t);
                dl->PathArcTo(P(11 - i * 1.5f, y - 1.6f), 1.6f * k, radians(90), radians(-90), 8);
                dl->PathStroke(c, 0, t);
            }
            break;
        case Icon::Spawn:
            dl->AddRect(P(2, 7), P(9, 14), c, 1.0f * k, 0, t);
            dl->AddRect(P(7, 2), P(14, 9), c, 1.0f * k, 0, t);
            break;
        case Icon::Reverb:
            for (int i = 0; i < 3; i++) {
                dl->PathArcTo(P(3, 8), (3.5f + i * 3.0f) * k, radians(-55), radians(55), 10);
                dl->PathStroke(c, 0, t);
            }
            dl->AddCircleFilled(P(3, 8), 1.4f * k, c, 8);
            break;
        case Icon::Sprite:
            dl->AddRect(P(2, 2), P(14, 14), c, 1.5f * k, 0, t);
            dl->AddTriangleFilled(P(4, 12), P(8, 6), P(12, 12), c);
            dl->AddCircleFilled(P(11.2f, 5), 1.3f * k, c, 8);
            break;
        case Icon::TextT:
            dl->AddLine(P(3, 3.5f), P(13, 3.5f), c, t);
            dl->AddLine(P(8, 3.5f), P(8, 13.5f), c, t);
            break;
        case Icon::Count_: break;
        case Icon::Fullscreen:
        case Icon::FullscreenExit: {
            // Four corners: brackets at the edges (fullscreen) or pointing in (exit).
            bool out = icon == Icon::Fullscreen;
            for (int i = 0; i < 4; i++) {
                float sx = (i & 1) ? -1.0f : 1.0f, sy = (i & 2) ? -1.0f : 1.0f;
                float cx = (i & 1) ? 13.5f : 2.5f, cy = (i & 2) ? 13.5f : 2.5f;
                ImVec2 k = out ? P(cx, cy) : P(cx + sx * 3.5f, cy + sy * 3.5f);
                dl->AddLine(k, P(cx + sx * 3.5f, cy), c, t);
                dl->AddLine(k, P(cx, cy + sy * 3.5f), c, t);
            }
            break;
        }
        case Icon::Star: {
            ImVec2 st[10];
            for (int i = 0; i < 10; i++) {
                float a = -kPi * 0.5f + kPi * i / 5.0f;
                float r = (i & 1) ? 2.8f : 6.8f;
                st[i] = P(8 + fcos(a) * r, 8.6f + fsin(a) * r);
            }
            ImVec2 inner[5] = { st[1], st[3], st[5], st[7], st[9] };
            dl->AddConvexPolyFilled(inner, 5, c);
            for (int i = 0; i < 5; i++) dl->AddTriangleFilled(st[i * 2], st[(i * 2 + 9) % 10], st[i * 2 + 1], c);
            break;
        }
        case Icon::Sun: {
            dl->AddCircleFilled(P(8, 8), 3.0f * k, c, 12);
            for (int i = 0; i < 8; i++) {
                float a = 2 * kPi * i / 8;
                dl->AddLine(P(8 + fcos(a) * 4.8f, 8 + fsin(a) * 4.8f), P(8 + fcos(a) * 6.6f, 8 + fsin(a) * 6.6f), c, t);
            }
            break;
        }
        case Icon::Monitor:
            dl->AddRect(P(1.5f, 2.5f), P(14.5f, 11), c, 1.5f * k, 0, t);
            dl->AddLine(P(8, 11), P(8, 13.5f), c, t);
            dl->AddLine(P(5, 13.8f), P(11, 13.8f), c, t);
            break;
        case Icon::Layers: {
            ImVec2 a[] = { P(8, 2.5f), P(14, 5.5f), P(8, 8.5f), P(2, 5.5f) };
            dl->AddPolyline(a, 4, c, ImDrawFlags_Closed, t);
            ImVec2 b[] = { P(2, 8.5f), P(8, 11.5f), P(14, 8.5f) };
            dl->AddPolyline(b, 3, c, 0, t);
            ImVec2 d[] = { P(2, 11.5f), P(8, 14.5f), P(14, 11.5f) };
            dl->AddPolyline(d, 3, c, 0, t);
            break;
        }
    }
}

bool icon_button(Icon icon, const char* tooltip, bool active, float size, uint32_t tint) {
    const Palette& pal = ui_palette();
    ImGui::PushID((int)icon);
    ImGui::PushID(tooltip);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##ib", ImVec2(size, size));
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();
    bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rounding = ImGui::GetStyle().FrameRounding;
    ImU32 bg = 0;
    if (active) bg = pal.accent_soft;
    else if (held) bg = pal.active;
    else if (hovered && !disabled) bg = pal.hover;
    if (bg) dl->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), bg, rounding);
    ImU32 fg = active ? pal.accent_text : (disabled ? pal.text_faint : (tint ? tint : (hovered ? pal.text : pal.text_dim)));
    float pad = (float)(int)(size * 0.22f);
    draw_icon(dl, icon, ImVec2(pos.x + pad, pos.y + pad), size - pad * 2, fg);
    if (hovered && tooltip && *tooltip) ImGui::SetItemTooltip("%s", tooltip);
    ImGui::PopID();
    ImGui::PopID();
    return clicked;
}

void draw_logo(ImDrawList* dl, ImVec2 p, float s, ImU32 accent) {
    // The vector logo (docs/logo/farcade-logo.svg) in its own colors.
    ImFont* font = icon_box_font();
    if (font && icon_codepoint("logo") && s >= 4.0f) {
        char utf8[5] = {};
        ImTextCharToUtf8(utf8, icon_codepoint("logo"));
        float size = (float)(int)(s + 0.5f);
        dl->AddText(font, size, ImVec2((float)(int)(p.x + 0.5f), (float)(int)(p.y + 0.5f)), IM_COL32_WHITE, utf8);
        return;
    }
    float k = s / 16.0f;
    auto P = [&](float x, float y) { return ImVec2(p.x + x * k, p.y + y * k); };
    dl->AddRectFilled(P(0, 0), P(16, 16), accent, 4.0f * k);
    ImU32 w = IM_COL32(255, 255, 255, 255);
    // An "F" leaning forward, with a speed line under the top bar.
    dl->AddQuadFilled(P(5.2f, 3.5f), P(12.6f, 3.5f), P(12.0f, 5.6f), P(4.6f, 5.6f), w);
    dl->AddQuadFilled(P(4.6f, 5.6f), P(6.8f, 5.6f), P(5.0f, 12.5f), P(2.8f, 12.5f), w);
    dl->AddQuadFilled(P(5.9f, 7.2f), P(10.8f, 7.2f), P(10.3f, 9.2f), P(5.4f, 9.2f), w);
    dl->AddLine(P(11.6f, 8.2f), P(13.4f, 8.2f), IM_COL32(255, 255, 255, 150), 1.2f * k);
}

}  // namespace fe
