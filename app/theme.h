// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app look: OnAir's dark "instrument" theme (OnAir app/ui/theme.h): flat neutral greys, thin rules, dense spacing, one muted accent; red,
// amber and green appear only where they mean a status.
#pragma once
#include "imgui.h"

namespace pal {
inline ImVec4 accent(float a = 1.f) { return ImVec4(0.45f, 0.72f, 0.82f, a); }   // Signal Teal, the one accent
inline ImVec4 accentHover() { return ImVec4(0.557f, 0.796f, 0.878f, 1); }               // #8ecbe0
inline ImVec4 accentActive() { return ImVec4(0.373f, 0.624f, 0.722f, 1); }              // #5f9fb8
inline ImVec4 accentDeep() { return ImVec4(0.30f, 0.46f, 0.55f, 1); }                   // Signal Teal Deep: pressed headers and buttons
inline ImVec4 onColour() { return ImVec4(0.055f, 0.058f, 0.062f, 1); }                  // Bench Black: text on Signal Teal (8.7:1) and Transmit Red (5.5:1)
inline ImVec4 pictureBlack() { return ImVec4(0, 0, 0, 1); }
inline ImVec4 okGreen() { return ImVec4(0.40f, 0.76f, 0.52f, 1); }
inline ImVec4 warnAmber() { return ImVec4(0.90f, 0.70f, 0.28f, 1); }
inline ImVec4 badRed() { return ImVec4(0.88f, 0.38f, 0.34f, 1); }
inline ImVec4 grey(float a = 1.f) { return ImVec4(0.58f, 0.60f, 0.62f, a); }
inline ImVec4 heading() { return accent(); }   // the One Accent rule: headings in Signal Teal
inline ImVec4 plotBg() { return ImVec4(0.02f, 0.021f, 0.023f, 1); }
inline ImVec4 plotBorder() { return ImVec4(0.24f, 0.25f, 0.26f, 1); }
inline ImVec4 plotGrid() { return ImVec4(0.60f, 0.62f, 0.64f, 0.16f); }
}  // namespace pal

inline void applyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.FrameRounding = 3;
    s.TabRounding = 3;
    s.ChildRounding = 4;
    s.GrabRounding = 2;
    s.PopupRounding = 4;
    s.ScrollbarRounding = 4;
    s.FramePadding = ImVec2(6, 2);
    s.ItemSpacing = ImVec2(6, 3);
    s.WindowPadding = ImVec2(8, 6);
    s.CellPadding = ImVec2(5, 1);
    s.FrameBorderSize = 1;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.TabBarBorderSize = 1;
    s.ScrollbarSize = 9;
    s.GrabMinSize = 3;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.058f, 0.062f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.075f, 0.078f, 0.083f, 1);
    c[ImGuiCol_Border] = ImVec4(0.37f, 0.38f, 0.39f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.035f, 0.037f, 0.040f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.10f, 0.105f, 0.11f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.14f, 0.145f, 0.15f, 1);
    c[ImGuiCol_Button] = ImVec4(0.10f, 0.105f, 0.11f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.17f, 0.18f, 0.19f, 1);
    c[ImGuiCol_ButtonActive] = pal::accentDeep();
    c[ImGuiCol_Header] = ImVec4(0.13f, 0.14f, 0.15f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.17f, 0.18f, 0.19f, 1);
    c[ImGuiCol_HeaderActive] = pal::accentDeep();
    c[ImGuiCol_Tab] = ImVec4(0.055f, 0.058f, 0.062f, 1);
    c[ImGuiCol_TabHovered] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_TabSelected] = ImVec4(0.15f, 0.38f, 0.66f, 1);
    c[ImGuiCol_TabDimmed] = ImVec4(0.055f, 0.058f, 0.062f, 1);
    c[ImGuiCol_TabDimmedSelected] = ImVec4(0.14f, 0.33f, 0.57f, 1);
    c[ImGuiCol_TabSelectedOverline] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_SliderGrab] = ImVec4(0.55f, 0.58f, 0.60f, 1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.72f, 0.82f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(0.45f, 0.72f, 0.82f, 1);
    c[ImGuiCol_Separator] = ImVec4(0.20f, 0.21f, 0.22f, 1);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.02f);
    c[ImGuiCol_Text] = ImVec4(0.82f, 0.84f, 0.85f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.52f, 0.54f, 0.56f, 1);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.45f, 0.72f, 0.82f, 1);
    c[ImGuiCol_NavCursor] = ImVec4(0.96f, 0.97f, 0.98f, 1);
}
