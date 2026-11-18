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
    // "ball.lua##script0": the part after ## keeps headers with the same
    // text apart and is not shown.
    dl->AddText(ImVec2(pos.x + 14 + fs * 2.0f, cy - ImGui::GetFontSize() * 0.5f), g_pal.text, title, ImGui::FindRenderedTextEnd(title));
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

// ---------------------------------------------------------------- floating windows

namespace {

ImVec4 col4(uint32_t c) { return ImGui::ColorConvertU32ToFloat4(c); }

// A soft drop shadow around the current window, drawn in its own draw list
// (so it lies over the panels below it) and only outside its rectangle.
void window_shadow() {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w) return;
    ImDrawList* dl = w->DrawList;
    ImVec2 a = w->Pos, b = ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
    float r = w->WindowRounding;
    uint32_t base = g_pal.shadow & 0x00FFFFFFu;
    float alpha = (float)((g_pal.shadow >> 24) & 0xFF) / 255.0f;
    dl->PushClipRectFullScreen();
    // Rings 1..layers px outside the rectangle, fading out; the ones below
    // the window reach a little further (light from above).
    const int layers = 16;
    for (int i = 1; i <= layers; i++) {
        float t = (float)(i - 1) / (float)layers;
        float k = (1.0f - t) * (1.0f - t) * (1.0f - t);
        uint32_t c = base | ((uint32_t)(alpha * k * 255.0f) << 24);
        float e = (float)i, eb = (float)i * 1.35f;
        dl->AddRect(ImVec2(a.x - e, a.y - e), ImVec2(b.x + e, b.y + eb), c, r + e, 0, 1.25f);
    }
    dl->PopClipRect();
}

// A line under a floating window's title bar.
void title_divider() {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (!w || (w->Flags & ImGuiWindowFlags_NoTitleBar)) return;
    float y = w->Pos.y + w->TitleBarHeight;
    w->DrawList->AddLine(ImVec2(w->Pos.x + 1, y - 0.5f), ImVec2(w->Pos.x + w->Size.x - 1, y - 0.5f), g_pal.float_border, 1.0f);
}

void push_float_style() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, col4(g_pal.float_border));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, col4(g_pal.float_bg));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, col4(g_pal.float_bg));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, col4(g_pal.float_title));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, col4(g_pal.float_title));
    ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, col4(g_pal.float_title));
}

void pop_float_style() {
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(2);
}

}  // namespace

bool begin_floating(const char* name, bool* open, int flags) {
    push_float_style();
    bool visible = ImGui::Begin(name, open, (ImGuiWindowFlags)flags | ImGuiWindowFlags_NoDocking);
    pop_float_style();
    if (visible) { window_shadow(); title_divider(); }
    return visible;
}

void end_floating() { ImGui::End(); }

bool begin_modal(const char* name, bool* open, int flags) {
    push_float_style();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
    bool visible = ImGui::BeginPopupModal(name, open, (ImGuiWindowFlags)flags);
    ImGui::PopStyleVar(2);
    pop_float_style();
    if (visible) { window_shadow(); title_divider(); }
    return visible;
}

bool primary_button(const char* label) {
    ImVec4 a = col4(g_pal.accent);
    ImVec4 h = ImVec4(a.x + (1.0f - a.x) * 0.12f, a.y + (1.0f - a.y) * 0.12f, a.z + (1.0f - a.z) * 0.12f, 1.0f);
    ImVec4 d = ImVec4(a.x * 0.85f, a.y * 0.85f, a.z * 0.85f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, a);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, h);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, d);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x + 6.0f, ImGui::GetStyle().FramePadding.y));
    bool clicked = ImGui::Button(label);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return clicked;
}

bool nav_item(const char* id, Icon icon, const char* label, bool selected) {
    ImGuiStyle& st = ImGui::GetStyle();
    float h = ImGui::GetFrameHeight() + 6.0f;
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), g_pal.accent_soft, st.FrameRounding);
        dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.25f), ImVec2(p.x + 3.0f, p.y + h * 0.75f), g_pal.accent, 1.5f);
    } else if (hovered) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), g_pal.hover, st.FrameRounding);
    }
    float is = ImGui::GetFontSize() * 1.05f;
    ImU32 ic = selected ? g_pal.accent_text : (hovered ? g_pal.text : g_pal.text_dim);
    draw_icon(dl, icon, ImVec2(p.x + 12.0f, p.y + (h - is) * 0.5f), is, ic);
    ImU32 tc = selected ? g_pal.text : (hovered ? g_pal.text : g_pal.text_dim);
    dl->AddText(ImVec2(p.x + 12.0f + is + 9.0f, p.y + (h - ImGui::GetFontSize()) * 0.5f), tc, label);
    return clicked;
}

}  // namespace fe
