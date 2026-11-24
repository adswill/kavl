// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app shared drawing pieces (app.h).
#include "app.h"

#include "kavl/kavl_fec.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

void lamp(ImVec4 col) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + h * 0.4f, p.y + h * 0.5f), h * 0.32f, ImGui::GetColorU32(col));
    ImGui::Dummy(ImVec2(h * 0.9f, h));
    ImGui::SameLine();
}

void heading(App& a, const char* t) {
    ImGui::Dummy(ImVec2(0, 6));   // more space above a heading than below it
    ImGui::PushFont(a.bold);
    ImGui::TextColored(pal::heading(), "%s", t);
    ImGui::PopFont();
}

void valueRow(App& a, const char* label, const char* fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(px(110));
    ImGui::PushFont(a.mono);
    ImGui::TextUnformatted(buf);
    ImGui::PopFont();
}

// The action buttons: primary (Start: Signal Teal, dark bold label) and on-air (Stop transmitting / recording: Transmit Red, dark bold label).
// Taller than the other controls, so the actions that start or stop something are the first thing found. A disabled primary falls back to the
// plain button, so a greyed teal never looks like an available action.
bool actionButton(App& a, const char* label, Action kind, bool enabled) {
    const ImVec2 size(-1, ImGui::GetFrameHeight() + 8);
    if (!enabled) {
        ImGui::BeginDisabled();
        ImGui::Button(label, size);
        ImGui::EndDisabled();
        return false;
    }
    const bool primary = kind == Action::Primary;
    ImGui::PushStyleColor(ImGuiCol_Button, primary ? pal::accent() : pal::badRed());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, primary ? pal::accentHover() : pal::onAirHover());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, primary ? pal::accentActive() : pal::onAirPressed());
    ImGui::PushStyleColor(ImGuiCol_Text, pal::onColour());
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushFont(a.bold);
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopFont();
    ImGui::PopStyleColor(5);
    return pressed;
}

// The studio light: a solid Transmit Red band while a HackRF is on the air; a quiet band for the loopback and for writing a file (not on the air).
void onAirBand(App& a) {
    if (!a.tx.running()) return;
    const bool onAir = a.txDest == 0;
    char t[64];
    if (a.txDest == 1) std::snprintf(t, sizeof(t), "WRITING IQ FILE");
    else std::snprintf(t, sizeof(t), "%s  \xC2\xB7  %.4f MHz", onAir ? "ON AIR" : "LOOPBACK", a.txFreqMHz);
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = px(onAir ? 36.f : 26.f);
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), ImGui::GetColorU32(onAir ? pal::badRed() : ImGui::GetStyleColorVec4(ImGuiCol_Header)), 3.f);
    ImGui::PushFont(onAir ? a.big : a.bold);
    const ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2(p0.x + (w - ts.x) * 0.5f, p0.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(onAir ? pal::onColour() : pal::accent()), t);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(w, h));
    if (onAir) ImGui::SetItemTooltip("This Mac's HackRF is transmitting on %.4f MHz", a.txFreqMHz);
}

// A small button that is still a 24 px target (ImGui's SmallButton is about 15 px tall).
bool chipButton(const char* label) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4.5f));
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleVar();
    return pressed;
}

void drawConstellation(const std::vector<kavl::cf>& pts, float size) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size, p0.y + size);
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(pal::plotBg()), 4.f);
    dl->AddRect(p0, p1, ImGui::GetColorU32(pal::plotBorder()), 4.f);
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    dl->AddLine(ImVec2(p0.x, c.y), ImVec2(p1.x, c.y), ImGui::GetColorU32(pal::plotGrid()));
    dl->AddLine(ImVec2(c.x, p0.y), ImVec2(c.x, p1.y), ImGui::GetColorU32(pal::plotGrid()));
    const float s = size / 3.2f;
    const ImU32 col = ImGui::GetColorU32(pal::accent(0.8f));
    for (const auto& z : pts) {
        const float x = c.x + z.real() * s, y = c.y - z.imag() * s;
        if (x > p0.x && x < p1.x && y > p0.y && y < p1.y) dl->AddRectFilled(ImVec2(x - 0.8f, y - 0.8f), ImVec2(x + 0.8f, y + 0.8f), col);
    }
    ImGui::Dummy(ImVec2(size, size));
}

// The converter between a radio and the antenna (LNB, transverter): its LO and side; the frequencies on screen stay those on the air, and the
// line under it says what the radio itself tunes. Locked while the radio runs (a mirrored spectrum needs a restart).
void converterUi(App& a, Converter& c, const char* id, bool locked, double airMHz) {
    (void)a;
    ImGui::PushID(id);
    if (locked) ImGui::BeginDisabled();
    ImGui::Checkbox("Through a converter", &c.on);
    ImGui::SetItemTooltip("An LNB, a transverter or an up/down converter between the radio and the antenna. Frequencies here stay on the air; the "
                          "radio tunes the IF.");
    if (c.on) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputDouble("##lo", &c.loMHz, 1.0, 10.0, "LO %.4f MHz");
        ImGui::SetItemTooltip("The converter's local oscillator: 9750 for a QO-100 LNB (10489.5 comes in on 739.5), 10224 for a 10 GHz transverter "
                              "with a 144 MHz IF, 1968 for a 2400 MHz up-converter from 432");
        ImGui::Checkbox("LO above the signal (mirrors)", &c.invert);
        ImGui::SetItemTooltip("High-side LO: the IF is the LO minus the signal and the spectrum comes mirrored; KAVL turns it back");
    }
    if (locked) ImGui::EndDisabled();
    if (c.on) {
        const double r = c.radioMHz(airMHz);
        if (r <= 0) ImGui::TextColored(pal::warnAmber(), "The radio would tune %.4f MHz: check the LO", r);
        else ImGui::TextDisabled("The radio tunes %.4f MHz", r);
    }
    ImGui::PopID();
}
