// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app the whole screen (app.h).
#include "app.h"

#include "kavl/kavl_fec.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

// the line at the bottom: receiver, transmitter, delay, decoder speed
void statusBar(App& a) {
    const auto& st = a.now.rx;
    const auto& ps = a.now.play;
    ImGui::Separator();
    auto part = [&](ImVec4 col, const char* fmt, ...) {
        char buf[160];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        lamp(col);
        ImGui::TextUnformatted(buf);
        ImGui::SameLine(0, 24);
    };
    if (!a.rx.running()) part(pal::grey(), "RX stopped");
    else {
        const bool locked = lockedNow(st);
        const int prof = a.profile >= 0 ? a.profile : st.profile;
        part(locked ? pal::okGreen() : pal::warnAmber(), "RX %.4f MHz  %s%s%s", a.freqMHz, locked ? (ps.playing ? "playing" : "locked") : "searching",
             prof >= 0 ? (std::string("  ") + kavl::videoProfile(prof).name).c_str() : "",
             locked && !st.callsign.empty() ? (std::string("  from ") + st.callsign).c_str() : "");
    }
    if (a.rig.active()) {
        const auto rs = a.rig.status();
        part(!rs.connected ? pal::warnAmber() : rs.ptt ? pal::badRed() : pal::okGreen(), "Rig %s", !rs.connected ? "not connected" : rs.ptt ? "PTT" : "ready");
    }
    if (!a.tx.running()) part(pal::grey(), "TX off");
    else if (a.txDest == 0) {   // on the air: a solid red pill, the status line's ON AIR light
        char t[64];
        std::snprintf(t, sizeof(t), "ON AIR  %.4f MHz%s", a.txFreqMHz, a.tx.live() ? "  live" : "");
        ImGui::PushFont(a.bold);
        const ImVec2 ts = ImGui::CalcTextSize(t), p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y - 1), ImVec2(p.x + ts.x + 16, p.y + ts.y + 1), ImGui::GetColorU32(pal::badRed()), 3.f);
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 8, p.y), ImGui::GetColorU32(pal::onColour()), t);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(ts.x + 16, ts.y));
        ImGui::SameLine(0, 24);
    } else {
        part(pal::accent(), "TX %s %.4f MHz%s", a.txDest == 1 ? "writing a file" : "Loopback", a.txFreqMHz, a.tx.live() ? "  live" : "");
    }
    if (ps.delaySec >= 0) part(pal::accent(), "Delay %.2f s", ps.delaySec);
    if (a.rx.running() && st.speed > 0) part(st.speed < 1.0 ? pal::badRed() : pal::grey(), "Decoder %.2fx", st.speed);
    if (!a.narrowNote.empty()) {
        ImGui::TextDisabled("%s", a.narrowNote.c_str());
        ImGui::SameLine(0, 24);
    }
    ImGui::NewLine();
}

void aboutWindows(App& a) {
    if (a.showAbout) {
        ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
        if (ImGui::Begin("About KAVL", &a.showAbout, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::PushFont(a.big);
            ImGui::TextColored(pal::accent(), "KAVL %s", KAVL_VERSION);
            ImGui::PopFont();
            ImGui::TextWrapped("Narrow-band digital video: 720p60 to 4K120 with stereo sound in 160 kHz to 1.25 MHz, from 13.5 dB.");
            ImGui::Spacing();
            ImGui::TextUnformatted("Copyright (c) 2026 Kristijonas Adomaitis (adswill).");
            ImGui::TextUnformatted("All rights reserved. KAVL Source Licence: see LICENSE.");
            ImGui::TextDisabled("https://github.com/adswill/kavl");
            if (ImGui::Button("Third-party licences")) a.showLicences = true;
        }
        ImGui::End();
    }
    if (a.showLicences) {
        ImGui::SetNextWindowSize(ImVec2(620, 420), ImGuiCond_Appearing);
        if (ImGui::Begin("Third-party licences", &a.showLicences, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextWrapped(
                "Dear ImGui - MIT licence, Copyright (c) 2014-2025 Omar Cornut.\n\n"
                "GLFW - zlib licence, Copyright (c) 2002-2006 Marcus Geelnard, (c) 2006-2019 Camilla Lowy.\n\n"
                "miniaudio - public domain or MIT No Attribution, David Reid.\n\n"
                "dav1d - BSD 2-clause, Copyright (c) 2018-2025 VideoLAN and dav1d authors.\n\n"
                "libopus - BSD 3-clause, Copyright (c) 2001-2023 Xiph.Org, Skype Limited, Octasic, Jean-Marc Valin, Timothy B. Terriberry, CSIRO, Gregory "
                "Maxwell, Mark Borgerding, Erik de Castro Lopo.\n\n"
                "libhackrf - BSD 3-clause, Copyright (c) Great Scott Gadgets and contributors.\n\n"
                "libusb - LGPL 2.1 or later, linked dynamically (the system's copy).\n\n"
                "Inter - SIL Open Font License 1.1, Copyright (c) 2016 The Inter Project Authors.\n\n"
                "JetBrains Mono - SIL Open Font License 1.1, Copyright 2020 The JetBrains Mono Project Authors.\n\n"
                "Separate programs the app runs, not part of it: rtl_tcp (librtlsdr, GPL 2) for RTL-SDR dongles, rigctld (Hamlib, GPL 2) for rig control, and FFmpeg 8.0 (LGPL 2.1, bundled; built by tools/build_ffmpeg.sh from the ffmpeg.org source) for encoding.\n\n"
                "Full texts: THIRD_PARTY.md and the licences folder.");
        }
        ImGui::End();
    }
}

void frame(App& a) {
    std::string picked;
    if (a.chooser.poll(1, picked)) {
        a.iqPath = picked;
        a.srcKind = 0;
        if (const double f = frequencyFromName(picked)) a.freqMHz = f / 1e6;
    }
    if (a.chooser.poll(2, picked)) a.videoPath = picked, a.txSource = 0;
    if (a.chooser.poll(3, picked)) a.txFilePath = picked;
    if (a.chooser.poll(4, picked)) a.cs.font = picked;
    pollTxStart(a);   // a start waiting for the rig to key
    a.tx.setStationId(a.cs.text);   // the callsign goes in every frame header (station ID), whether or not it is on the picture
    takeSnapshot(a);   // one read of the engines' status for the whole frame
    uploadPicture(a);
    updateSpectrum(a);
    takeDevices(a);
    ImGui::GetStyle().FontScaleMain = a.textScale;   // takes effect from the next frame's layout

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("KAVL", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | (a.videoOnly ? 0 : ImGuiWindowFlags_MenuBar));
    const bool f11 = ImGui::IsKeyPressed(ImGuiKey_F11);
    if (!a.videoOnly && ImGui::BeginMenuBar()) {
        ImGui::PushStyleColor(ImGuiCol_Text, pal::accent());
        ImGui::PushFont(a.bold);
        const bool appMenu = ImGui::BeginMenu("KAVL##app");
        ImGui::PopFont();
        ImGui::PopStyleColor();
        if (appMenu) {
            if (ImGui::MenuItem("About KAVL")) a.showAbout = true;
            if (ImGui::MenuItem("Third-party licences")) a.showLicences = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Cmd+Q")) glfwSetWindowShouldClose(a.win, 1);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open IQ file...", "Cmd+O")) a.chooser.open(1, "Choose a KAVL IQ .wav", false);
            if (ImGui::MenuItem("Choose a video to transmit...")) {
                a.showTx = true;
                a.chooser.open(2, "Choose a video to transmit", false);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Receive column", "Cmd+1", &a.showRx);
            ImGui::MenuItem("Transmit column", "Cmd+2", &a.showTx);
            ImGui::Separator();
            if (ImGui::MenuItem("Video only", "F", a.videoOnly)) a.videoOnly = !a.videoOnly;
            if (ImGui::MenuItem("Full screen", "F11", a.fullscreen)) a.fullscreen = !a.fullscreen, glfwPostEmptyEvent();
            if (ImGui::MenuItem("Own window", "P", a.popout.isOpen())) a.wantPopout = !a.popout.isOpen();
            ImGui::Separator();
            ImGui::MenuItem("Waterfall", nullptr, &a.specView.waterfall);
            ImGui::Separator();
            if (ImGui::BeginMenu("Text size")) {   // for a small screen, outdoors, or tired eyes
                static const float sizes[] = {0.9f, 1.f, 1.15f, 1.3f};
                static const char* names[] = {"Small", "Normal", "Large", "Larger"};
                for (int i = 0; i < 4; i++)
                    if (ImGui::MenuItem(names[i], i == 1 ? "Cmd+0" : nullptr, std::abs(a.textScale - sizes[i]) < 0.01f)) a.textScale = sizes[i];
                ImGui::TextDisabled("Cmd+= and Cmd+- step");
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Radio")) {
            if (ImGui::MenuItem("Look for radios again")) refreshDevices(a);
            ImGui::TextDisabled("RTL-SDR: %zu, HackRF: %zu", a.rtlDevs.size(), a.hrfDevs.size());
            ImGui::Separator();
            if (ImGui::MenuItem("Rig control (CAT, PTT)...", nullptr, a.showRig)) a.showRig = !a.showRig;
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    // full screen (F11 or the menu)
    static bool fsApplied = false;
    if (f11) a.fullscreen = !a.fullscreen;
    if (a.fullscreen != fsApplied) {
        fsApplied = a.fullscreen;
        if (a.fullscreen) {
            glfwGetWindowPos(a.win, &a.winX, &a.winY);
            glfwGetWindowSize(a.win, &a.winW, &a.winH);
            GLFWmonitor* mon = glfwGetPrimaryMonitor();
            const GLFWvidmode* m = glfwGetVideoMode(mon);
            glfwSetWindowMonitor(a.win, mon, 0, 0, m->width, m->height, m->refreshRate);
        } else {
            glfwSetWindowMonitor(a.win, nullptr, a.winX, a.winY, a.winW, a.winH, 0);
        }
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_F) && !io.KeySuper) a.videoOnly = !a.videoOnly;
        if (ImGui::IsKeyPressed(ImGuiKey_P) && !io.KeySuper) a.wantPopout = !a.popout.isOpen();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) a.videoOnly = false;
        if (io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_O)) a.chooser.open(1, "Choose a KAVL IQ .wav", false);
        if (io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_1)) a.showRx = !a.showRx;
        if (io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_2)) a.showTx = !a.showTx;
        if (io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_Z, false)) undoTune(a);
        {   // text size: Cmd+= larger, Cmd+- smaller, Cmd+0 normal
            static const float sizes[] = {0.9f, 1.f, 1.15f, 1.3f};
            int cur = 1;
            for (int i = 0; i < 4; i++)
                if (std::abs(a.textScale - sizes[i]) < 0.01f) cur = i;
            if (io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_Equal, false)) a.textScale = sizes[std::min(3, cur + 1)];
            if (io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_Minus, false)) a.textScale = sizes[std::max(0, cur - 1)];
            if (io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_0, false)) a.textScale = 1.f;
        }
        // tuning steps on [ and ] (the arrows belong to keyboard navigation)
        const double step = io.KeyShift ? 0.05 : 0.005;
        if (!io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) setRxFrequency(a, (a.freqMHz - step) * 1e6, Tune::Key);
        if (!io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_RightBracket)) setRxFrequency(a, (a.freqMHz + step) * 1e6, Tune::Key);
    }
    if (a.videoOnly) {   // the picture alone: no columns, no spectrum
        drawVideo(a, ImGui::GetContentRegionAvail(), false);
        ImGui::End();
        aboutWindows(a);
        rigWindow(a);
        return;
    }
    // the station: Receive on the left, the picture and the spectrum in the middle, Transmit on the right, the status line at the bottom
    const float statusH = ImGui::GetFrameHeightWithSpacing() + 4;
    const float h = ImGui::GetContentRegionAvail().y - statusH;
    // The columns scale with the window; when the picture would get narrower than 520 px, a column that is not in use gives way (Transmit
    // first). A running column never hides: it holds its Stop. The status line says what was hidden.
    const float total = ImGui::GetContentRegionAvail().x, gap = ImGui::GetStyle().ItemSpacing.x, minMid = px(520);
    const float leftW = std::clamp(total * 0.2f * a.textScale, px(250), px(300)), rightW = std::clamp(total * 0.22f * a.textScale, px(270), px(330));
    bool showRx = a.showRx, showTx = a.showTx;
    auto midOf = [&]() { return total - (showRx ? leftW + gap : 0.f) - (showTx ? rightW + gap : 0.f); };
    if (midOf() < minMid && showTx && !a.tx.running() && !a.tx.preparing()) showTx = false;
    if (midOf() < minMid && showRx && !a.rx.running()) showRx = false;
    a.narrowNote = !(showTx == a.showTx && showRx == a.showRx)
                       ? std::string("Window too narrow: ") + (showTx != a.showTx && showRx != a.showRx ? "Receive and Transmit" : showTx != a.showTx ? "Transmit" : "Receive") +
                             " hidden (widen the window to show)"
                       : std::string();
    if (showRx) {
        receivePanel(a, leftW, h);
        ImGui::SameLine();
    }
    const float midW = ImGui::GetContentRegionAvail().x - (showTx ? rightW + gap : 0.f);
    ImGui::BeginChild("##main", ImVec2(midW, h), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 av = ImGui::GetContentRegionAvail();
    const float specH = std::clamp(av.y * (a.specView.waterfall ? 0.36f : 0.2f), px(140), px(460));
    const float rowH = ImGui::GetFrameHeightWithSpacing();
    drawVideo(a, ImVec2(av.x, av.y - specH - rowH - 6), true);
    ImGui::Dummy(ImVec2(0, 2));
    a.specView.options();
    drawSpectrum(a, ImVec2(av.x, ImGui::GetContentRegionAvail().y));
    ImGui::EndChild();
    if (showTx) {
        ImGui::SameLine();
        transmitPanel(a, rightW, h);
    }
    statusBar(a);
    ImGui::End();
    aboutWindows(a);
    rigWindow(a);
}
