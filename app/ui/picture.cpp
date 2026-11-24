// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app the received picture and the spectrum (app.h).
#include "app.h"

#include "kavl/kavl_fec.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

// What the picture area says while there is no picture: first use (what will be here, the steps, a one-click try), looking for a signal (on
// which frequency, what to check), and the short waits after a signal is found.
void pictureEmptyState(App& a, ImVec2 p0, ImVec2 size) {
    auto* dl = ImGui::GetWindowDrawList();
    const ImU32 strong = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float colW = std::min(px(440), size.x - 40.f);
    const float x0 = p0.x + (size.x - colW) * 0.5f;
    auto lineH = [](ImFont* f) {
        ImGui::PushFont(f);
        const float h = ImGui::GetTextLineHeightWithSpacing();
        ImGui::PopFont();
        return h;
    };
    auto text = [&](ImFont* f, ImU32 col, float& y, const char* t) {   // one line, centred in the column
        ImGui::PushFont(f);
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(x0 + (colW - ts.x) * 0.5f, y), col, t);
        y += ImGui::GetTextLineHeightWithSpacing();
        ImGui::PopFont();
    };
    const auto& st = a.now.rx;
    char buf[160];
    if (a.rx.running()) {
        // a frame lately, not ever (within 1.5 s, or 2.5 frame times on the narrow profiles)
        const double recent = st.profile >= 0 ? std::max(1.5, 2.5 * kavl::frameSeconds(kavl::videoProfile(st.profile).symRate)) : 1.5;
        const bool found = st.lastFrameSec >= 0 && st.streamSec - st.lastFrameSec < recent;
        float y = p0.y + size.y * 0.5f - lineH(a.big) - 4;
        if (!found) {
            text(a.big, strong, y, "Looking for a KAVL signal");
            const int prof = a.profile >= 0 ? a.profile : st.profile;
            std::snprintf(buf, sizeof(buf), "on %.4f MHz, profile %s", a.freqMHz, a.profile < 0 ? "Auto" : kavl::videoProfile(prof).name);
            text(a.ui, muted, y, buf);
            y += 6;
            if (a.srcKind == 3 && !a.tx.running()) text(a.ui, muted, y, "Nothing is being sent: start Transmit (Send to: Loopback).");
            else text(a.ui, muted, y, "Nothing yet? Check the transmitter's frequency and profile, or click its signal below.");
        } else {
            text(a.big, strong, y, a.now.play.srcW ? "Starting the picture" : "Signal found");
            text(a.ui, muted, y, a.now.play.srcW ? "a moment's buffer" : "waiting for a key frame (every 2 to 4 s)");
        }
        return;
    }
    // first use: a small drawn spectrum (noise and one KAVL signal), what will be here, the steps, and the try
    const float glyphW = 132, glyphH = 34;
    const float blockH = glyphH + 14 + lineH(a.big) + lineH(a.ui) + 12 + 3 * lineH(a.ui) + 16 + ImGui::GetFrameHeight() + 8 + 6 + lineH(a.ui);
    float y = p0.y + std::max(12.f, (size.y - blockH) * 0.5f);
    {
        const float gx = x0 + (colW - glyphW) * 0.5f, base = y + glyphH;
        std::vector<ImVec2> noise;
        for (int i = 0; i <= 44; i++) {
            const float x = gx + glyphW * i / 44.f;
            const bool in = i >= 17 && i <= 27;
            const float jitter = static_cast<float>(((i * 37) % 7) - 3) * 0.9f;
            noise.emplace_back(x, in ? y + 6 + jitter * 0.5f : base - 6 + jitter);
        }
        for (size_t i = 1; i < noise.size(); i++)
            dl->AddQuadFilled(ImVec2(noise[i - 1].x, base), noise[i - 1], noise[i], ImVec2(noise[i].x, base), ImGui::GetColorU32(pal::accent(0.12f)));
        dl->AddPolyline(noise.data(), static_cast<int>(noise.size()), ImGui::GetColorU32(pal::accent()), 0, 1.4f);
        dl->AddLine(ImVec2(gx, base + 1), ImVec2(gx + glyphW, base + 1), ImGui::GetColorU32(pal::plotBorder()), 1.f);
        y += glyphH + 14;
    }
    text(a.big, strong, y, "No picture yet");
    text(a.ui, muted, y, "The received picture appears here once a KAVL signal is found.");
    y += 12;
    const char* steps[] = {"Pick where the signal comes from, on the left", "Set its frequency", "Press Start receiving"};
    ImGui::PushFont(a.ui);
    float stepW = 0;
    for (const char* s : steps) stepW = std::max(stepW, ImGui::CalcTextSize(s).x);
    const float sx = x0 + (colW - stepW - 22) * 0.5f;
    for (int i = 0; i < 3; i++) {   // a real sequence, so numbered
        char n[4];
        std::snprintf(n, sizeof(n), "%d", i + 1);
        dl->AddText(ImVec2(sx, y), ImGui::GetColorU32(pal::accent()), n);
        dl->AddText(ImVec2(sx + 22, y), strong, steps[i]);
        y += ImGui::GetTextLineHeightWithSpacing();
    }
    ImGui::PopFont();
    y += 16;
    const float bw = 240;
    const ImVec2 after = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(x0 + (colW - bw) * 0.5f, y));
    ImGui::PushItemWidth(bw);
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::BeginChild("##trybtn", ImVec2(bw, ImGui::GetFrameHeight() + 8), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    if (actionButton(a, "Try it with Loopback", Action::Primary)) tryLoopback(a);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    ImGui::PopItemWidth();
    ImGui::SetItemTooltip("Starts the test pattern on the Transmit side, sent straight to this Mac's receiver");
    y += ImGui::GetFrameHeight() + 8 + 6;
    text(a.ui, muted, y, "This Mac sends a test pattern to itself. No radio needed.");
    ImGui::SetCursorScreenPos(after);
}

void drawVideo(App& a, ImVec2 size, bool hint) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), ImGui::GetColorU32(pal::pictureBlack()));
    const float aspect = a.video.height() > 0 ? static_cast<float>(a.video.width()) / static_cast<float>(a.video.height()) : 16.f / 9.f;
    const float vw = std::min(size.x, size.y * aspect), vh = vw / aspect;
    const ImVec2 q0(p0.x + (size.x - vw) * 0.5f, p0.y + (size.y - vh) * 0.5f);
    const bool showing = a.havePic && a.rx.running();
    if (showing) dl->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(a.video.texture())), q0, ImVec2(q0.x + vw, q0.y + vh));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##video", size);
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) a.videoOnly = !a.videoOnly;
    if (hint && ImGui::IsItemHovered() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) ImGui::SetItemTooltip("Double-click or F: video only. Esc: back. F11: full screen. P: Own window.");
    if (!showing) pictureEmptyState(a, p0, size);
    if (hovered || a.popout.isOpen()) {   // the pop-out button, top right of the picture
        const ImVec2 after = ImGui::GetCursorScreenPos();
        const char* lbl = a.popout.isOpen() ? "Close own window" : "Own window";
        ImGui::SetCursorScreenPos(ImVec2(p0.x + size.x - ImGui::CalcTextSize(lbl).x - 20, p0.y + 8));
        if (chipButton(lbl)) a.wantPopout = !a.popout.isOpen();
        ImGui::SetItemTooltip("The picture in a window of its own (P), e.g. for a second screen. There: F or double-click for full screen, Esc to close");
        ImGui::SetCursorScreenPos(after);
    }
}

void drawSpectrum(App& a, ImVec2 size) {
    SpectrumView::Input in;
    in.db = &a.spec;
    in.fs = a.specFs;
    in.rxHz = a.freqMHz * 1e6;
    in.radioHz = in.rxHz + a.specOff;
    in.fresh = a.specFresh;
    const int shown = a.profile >= 0 ? a.profile : a.now.rx.profile;
    in.bandHalfHz = shown >= 0 ? kavl::occupiedHz(kavl::videoProfile(shown).symRate) / 2.0 : 0.0;
    if (a.tx.running() && a.txDest != 1) in.txHz = a.txFreqMHz * 1e6;
    double hz = 0;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (a.specView.draw(in, size, a.mono, hz)) setRxFrequency(a, hz, Tune::Plot);
    if (ImGui::GetTime() < a.chipUntil) {   // a click on the spectrum moved the receiver: one click back
        const ImVec2 after = ImGui::GetCursorScreenPos();
        char t[64];
        std::snprintf(t, sizeof(t), "Back to %.4f MHz", a.chipFromMHz);
        ImGui::SetCursorScreenPos(ImVec2(at.x + 46, at.y + 6));
        if (chipButton(t)) {
            a.chipUntil = 0;
            undoTune(a);
        }
        ImGui::SetItemTooltip("Undo the retune (Cmd+Z)");
        ImGui::SetCursorScreenPos(after);
        ImGui::Dummy(ImVec2(0, 0));   // an item where the cursor was put back, so the window's bounds stay valid
    }
}
