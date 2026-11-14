#include "icons.h"

#include "core/math.h"

namespace fe {

void draw_icon(ImDrawList* dl, Icon icon, ImVec2 p, float s, ImU32 c) {
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
    }
}

bool icon_button(Icon icon, const char* tooltip, bool active, float size) {
    ImGui::PushID((int)icon);
    ImGui::PushID(tooltip);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##ib", ImVec2(size, size));
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();
    bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& st = ImGui::GetStyle();
    ImU32 bg = 0;
    if (active) bg = ImGui::GetColorU32(ImVec4(0.30f, 0.55f, 1.0f, held ? 0.40f : 0.25f));
    else if (held) bg = ImGui::GetColorU32(ImGuiCol_ButtonActive);
    else if (hovered && !disabled) bg = ImGui::GetColorU32(ImGuiCol_ButtonHovered);
    if (bg) dl->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), bg, st.FrameRounding);
    ImU32 fg = active ? ImGui::GetColorU32(ImVec4(0.45f, 0.65f, 1.0f, 1.0f)) : ImGui::GetColorU32(disabled ? ImGuiCol_TextDisabled : ImGuiCol_Text);
    float pad = size * 0.2f;
    draw_icon(dl, icon, ImVec2(pos.x + pad, pos.y + pad), size - pad * 2, fg);
    if (hovered && tooltip && *tooltip) ImGui::SetItemTooltip("%s", tooltip);
    ImGui::PopID();
    ImGui::PopID();
    return clicked;
}

}  // namespace fe
