#include "widgets.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <stdio.h>
#include <string.h>

namespace fe {

namespace {
Palette g_pal;
ImFont* g_medium = nullptr;
}  // namespace

const Palette& ui_palette() { return g_pal; }

void set_ui_palette(const Palette& p, ImFont* medium) {
    g_pal = p;
    g_medium = medium;
}

void heading(const char* text) {
    if (g_medium) ImGui::PushFont(g_medium, 0.0f);
    ImGui::TextUnformatted(text);
    if (g_medium) ImGui::PopFont();
}

bool section_header(const char* title, Icon icon, bool default_open) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    ImGuiID id = window->GetID(title);
    ImGuiStorage* storage = ImGui::GetStateStorage();
    bool open = storage->GetBool(id, default_open);

    float h = ImGui::GetFrameHeight() + 2.0f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(title);
    bool clicked = ImGui::InvisibleButton("##section", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (clicked) {
        open = !open;
        storage->SetBool(id, open);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = hovered ? g_pal.hover : g_pal.header;
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), bg, ImGui::GetStyle().FrameRounding);
    float fs = ImGui::GetFontSize();
    float cy = pos.y + h * 0.5f;
    draw_icon(dl, open ? Icon::ChevronDown : Icon::ChevronRight, ImVec2(pos.x + 6, cy - fs * 0.4f), fs * 0.8f, g_pal.text_faint);
    draw_icon(dl, icon, ImVec2(pos.x + 8 + fs, cy - fs * 0.45f), fs * 0.9f, g_pal.accent_text);
    if (g_medium) ImGui::PushFont(g_medium, 0.0f);
    dl->AddText(ImVec2(pos.x + 14 + fs * 2.0f, cy - ImGui::GetFontSize() * 0.5f), g_pal.text, title);
    if (g_medium) ImGui::PopFont();
    if (open) ImGui::Dummy(ImVec2(0, 1));
    return open;
}

float prop_label_width() {
    float w = ImGui::GetContentRegionAvail().x * 0.34f;
    if (w < 64) w = 64;
    if (w > 112) w = 112;
    return w;
}

void prop_label(const char* label) {
    float lw = prop_label_width();
    float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(g_pal.text_dim));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(x0 + lw);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

bool toggle_switch(const char* id, bool* value) {
    float h = ImGui::GetTextLineHeight() * 0.95f;
    float w = h * 1.8f;
    float fh = ImGui::GetFrameHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, fh));
    if (clicked) *value = !*value;
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = p.y + (fh - h) * 0.5f;
    ImU32 track = *value ? g_pal.accent : (hovered ? g_pal.active : g_pal.hover);
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + h), track, h * 0.5f);
    float r = h * 0.5f - 2.0f;
    float cx = *value ? p.x + w - h * 0.5f : p.x + h * 0.5f;
    dl->AddCircleFilled(ImVec2(cx, y + h * 0.5f), r, IM_COL32(255, 255, 255, 255), 16);
    return clicked;
}

bool chip(const char* id, Icon icon, uint32_t icon_color, const char* label, bool on) {
    float fs = ImGui::GetFontSize();
    float h = ImGui::GetFrameHeight();
    float tw = ImGui::CalcTextSize(label).x;
    float w = 8 + fs + 5 + tw + 9;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rounding = h * 0.5f;
    if (on) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hovered ? g_pal.active : g_pal.hover, rounding);
    else if (hovered) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), g_pal.hover, rounding);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), g_pal.border, rounding);
    draw_icon(dl, icon, ImVec2(p.x + 8, p.y + (h - fs) * 0.5f), fs, on ? icon_color : g_pal.text_faint);
    dl->AddText(ImVec2(p.x + 8 + fs + 5, p.y + (h - fs) * 0.5f), on ? g_pal.text : g_pal.text_faint, label);
    return clicked;
}

bool flat_button(const char* label, float height) {
    ImVec2 size(0, height);
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(g_pal.frame));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor();
    return r;
}

}  // namespace fe
