// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app the Receive column (app.h).
#include "app.h"

#include "kavl/kavl_fec.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

void radioSettings(App& a) {
    const bool run = a.rx.running();
    if (a.srcKind == 0) {
        ImGui::TextDisabled("File");
        ImGui::TextWrapped("%s", a.iqPath.empty() ? "(none)" : std::filesystem::path(a.iqPath).filename().string().c_str());
        if (!run && ImGui::Button("Open IQ file...", ImVec2(-1, 0))) a.chooser.open(1, "Choose a KAVL IQ .wav", false);
        return;
    }
    // the frequency and the gains follow at once while receiving; the device choice waits for a stop
    auto freqInput = [&]() {
        ImGui::SetNextItemWidth(-px(72));
        const double shown = a.freqMHz;
        // a narrow column drops the last digit rather than clip the unit
        const char* ffmt = ImGui::GetContentRegionAvail().x - px(72) < px(150) ? "%.3f MHz" : "%.4f MHz";
        if (ImGui::InputDouble("##freq", &a.freqMHz, 0.1, 1.0, ffmt)) {
            const double typed = a.freqMHz;
            a.freqMHz = shown;
            setRxFrequency(a, typed * 1e6, Tune::Typed);
        }
        ImGui::SetItemTooltip("Receive frequency; changes at once while receiving. Or click the spectrum. [ and ] step 5 kHz (Shift: 50 kHz), Cmd+Z goes back.");
        ImGui::SameLine();
        if (ImGui::Button("Recent", ImVec2(-1, 0))) ImGui::OpenPopup("##recent");
        ImGui::SetItemTooltip("The last frequencies received on");
        if (ImGui::BeginPopup("##recent")) {
            if (a.recent.empty()) ImGui::TextDisabled("Frequencies you receive on are kept here.");
            for (double r : a.recent) {
                char t[32];
                std::snprintf(t, sizeof(t), "%.4f MHz", r);
                if (ImGui::Selectable(t, std::abs(r - a.freqMHz) < 0.0005)) setRxFrequency(a, r * 1e6, Tune::Recent);
            }
            ImGui::EndPopup();
        }
        if (ImGui::GetTime() < a.noteUntil) {
            ImGui::PushStyleColor(ImGuiCol_Text, pal::warnAmber());
            ImGui::TextWrapped("%s", a.freqNote.c_str());
            ImGui::PopStyleColor();
        }
    };
    if (a.srcKind == 3) {
        if (!run) ImGui::TextDisabled("From this Mac's transmitter (Send to: Loopback)");
        freqInput();
        return;
    }
    if (!run && a.srcKind == 1) {   // the device choice: only while stopped
        ImGui::Checkbox("Remote rtl_tcp server", &a.rtl.remote);
        if (a.rtl.remote) {
            ImGui::SetNextItemWidth(-60);
            ImGui::InputText("##host", a.rtlHost, sizeof(a.rtlHost));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputInt("##port", &a.rtl.port, 0);
        } else {
            ImGui::SetNextItemWidth(-70);
            if (a.rtlDevs.empty()) ImGui::TextDisabled("No RTL-SDR found");
            else ImGui::Combo("##rtldev", &a.rtlDev, [](void* d, int i) { return (*static_cast<std::vector<RadioDevice>*>(d))[static_cast<size_t>(i)].label.c_str(); },
                              &a.rtlDevs, static_cast<int>(a.rtlDevs.size()));
            ImGui::SameLine();
            if (ImGui::Button("Rescan##refr", ImVec2(-1, 0))) refreshDevices(a);
            ImGui::SetItemTooltip("Look for radios again");
            if (findRtlTcp().empty()) ImGui::TextColored(pal::warnAmber(), "rtl_tcp missing: brew install librtlsdr");
        }
    } else if (!run) {
        ImGui::SetNextItemWidth(-70);
        if (a.hrfDevs.empty()) ImGui::TextDisabled("No HackRF found");
        else ImGui::Combo("##hrfdev", &a.hrfDev, [](void* d, int i) { return (*static_cast<std::vector<RadioDevice>*>(d))[static_cast<size_t>(i)].label.c_str(); },
                          &a.hrfDevs, static_cast<int>(a.hrfDevs.size()));
        ImGui::SameLine();
        if (ImGui::Button("Rescan##refh", ImVec2(-1, 0))) refreshDevices(a);
        ImGui::SetItemTooltip("Look for radios again");
    }
    freqInput();
    bool changed = false;
    if (a.srcKind == 1) {
        bool autoGain = a.rtl.gainTenthsDb < 0;
        if (ImGui::Checkbox("Automatic gain", &autoGain)) a.rtl.gainTenthsDb = autoGain ? -1 : 300, changed = true;
        ImGui::SetItemTooltip("The tuner chooses its gain. Off: set it below (lower it when a transmitter close by overloads the dongle).");
        if (!autoGain) {
            float g = a.rtl.gainTenthsDb / 10.f;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##rtlg", &g, 0.f, 49.6f, "Gain %.1f dB")) a.rtl.gainTenthsDb = static_cast<int>(std::lround(g * 10)), changed = true;
            ImGui::SetItemTooltip("Tuner gain, changes at once");
        }
        ImGui::SetNextItemWidth(-1);
        changed |= ImGui::SliderInt("##ppm", &a.rtl.ppm, -100, 100, "Correction %d ppm");
        ImGui::SetItemTooltip("Frequency correction of the dongle's crystal");
        changed |= ImGui::Checkbox("Bias tee", &a.rtl.biasTee);
        ImGui::SetItemTooltip("Power on the antenna socket (for an active antenna or an LNA)");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("RTL AGC", &a.rtl.agc);
        ImGui::SetItemTooltip("The RTL2832U's own digital gain control");
        if (changed && run && a.rx.source()) a.rx.source()->update(a.rtl);
    } else {
        ImGui::SetNextItemWidth(-1);
        changed |= ImGui::SliderInt("##lna", &a.hrf.lna, 0, 40, "LNA %d dB");
        ImGui::SetItemTooltip("RF gain, 8 dB steps");
        a.hrf.lna = a.hrf.lna / 8 * 8;
        ImGui::SetNextItemWidth(-1);
        changed |= ImGui::SliderInt("##vga", &a.hrf.vga, 0, 62, "VGA %d dB");
        ImGui::SetItemTooltip("Baseband gain, 2 dB steps");
        a.hrf.vga = a.hrf.vga / 2 * 2;
        changed |= ImGui::Checkbox("Amplifier +14 dB", &a.hrf.amp);
        ImGui::SetItemTooltip("The HackRF's RF amplifier");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Antenna power", &a.hrf.antennaPower);
        ImGui::SetItemTooltip("Bias on the antenna port (for an active antenna)");
        if (changed && run && a.rx.source()) a.rx.source()->update(a.hrf);
    }
}

void receivePanel(App& a, float w, float h) {
    const auto& st = a.now.rx;
    const auto& ps = a.now.play;
    ImGui::BeginChild("##left", ImVec2(w, h), ImGuiChildFlags_Borders);
    const bool run = a.rx.running();
    ImGui::PushFont(a.bold);
    const bool rxOpen = ImGui::CollapsingHeader("Receive", ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopFont();
    if (rxOpen && run) {   // the choices are locked while receiving: one line says what was chosen
        static const char* srcNames[] = {"IQ file", "RTL-SDR", "HackRF", "Loopback (this Mac's transmitter)"};
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s%s%s", srcNames[std::clamp(a.srcKind, 0, 3)], a.profile >= 0 ? ", " : "", a.profile >= 0 ? kavl::videoProfile(a.profile).name : "");
        ImGui::PopStyleColor();
    }
    if (rxOpen) {
        if (!run) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##src", &a.srcKind, "IQ file\0RTL-SDR (rtl_tcp)\0HackRF\0Loopback (this Mac's transmitter)\0") && a.srcKind > 0) refreshDevices(a);
        ImGui::SetItemTooltip("Where the signal comes from. Loopback: this Mac's own transmitter, no radio needed");
        ImGui::SetNextItemWidth(-1);
        int sel = a.profile + 1;   // 0 = Auto
        if (ImGui::Combo("##prof", &sel, [](void*, int i) { return i == 0 ? "Auto (find the profile)" : kavl::videoProfile(i - 1).name; }, nullptr,
                         kavl::kNumVideoProfiles + 1))
            a.profile = sel - 1;
        ImGui::SetItemTooltip("The transmission's profile. Auto finds it from the signal's width and header");
        }
        if (a.profile < 0 && run) {
            if (st.profile >= 0) ImGui::TextColored(pal::okGreen(), "Auto: %s", kavl::videoProfile(st.profile).name);
            else ImGui::TextColored(pal::warnAmber(), "Auto: looking for a signal...");
        }
        radioSettings(a);
        if (!run) {
            if (actionButton(a, "Start receiving", Action::Primary)) startRx(a);
            ImGui::SetItemTooltip("Opens the radio (or the file) and looks for a KAVL signal");
        } else {
            if (ImGui::Button("Stop receiving", ImVec2(-1, ImGui::GetFrameHeight() + 8))) a.rx.stop();   // not a status: the plain button
        }
        if (run) {   // recording what the receiver gets, for a later look (the frequency goes into the file name)
        if (!a.rx.recording()) {
            if (ImGui::Button("Record IQ", ImVec2(-1, 0))) {
                const std::string dir = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/Documents/KAVL";
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                char name[160];
                const std::time_t now = std::time(nullptr);
                std::strftime(name, sizeof(name), "%Y%m%d_%H%M%S", std::localtime(&now));
                const std::string path = dir + "/kavl_rec_" + std::to_string(static_cast<long long>(std::llround(a.freqMHz * 1e6))) + "Hz_" + name + ".wav";
                if (!a.rx.startRecording(path)) a.rxError = "Could not start the recording";
            }
            ImGui::SetItemTooltip("Saves what the receiver gets as an IQ .wav in Documents/KAVL: about %.0f MB a minute", a.rx.source() ? a.rx.source()->sampleRate() * 4 * 60 / 1e6 : 0.0);
        } else {
            char lbl[64];
            std::snprintf(lbl, sizeof(lbl), "Stop recording (%.0f s, %.0f MB)", a.rx.recordedSec(), a.rx.recordedSec() * a.rx.source()->sampleRate() * 4 / 1e6);
            if (actionButton(a, lbl, Action::OnAir)) a.rx.stopRecording();
        }
        if (!a.rx.recordingPath().empty() && !a.rx.recording()) {
            if (chipButton("Show the recording in Finder")) {
                const std::string cmd = "open -R '" + a.rx.recordingPath() + "'";
                std::system(cmd.c_str());
            }
        }
    }
        if (!a.rxError.empty()) ImGui::TextColored(pal::badRed(), "%s", a.rxError.c_str());
    }
    ImGui::PushFont(a.bold);
    const bool sigOpen = ImGui::CollapsingHeader("Signal", ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopFont();
    if (sigOpen) {
        const bool locked = run && st.lastFrameSec >= 0 && st.streamSec - st.lastFrameSec < 0.5;
        const bool clean = locked && st.lastOk == st.lastBlocks;
        if (!run) {
            lamp(pal::grey());
            ImGui::TextDisabled("Stopped");
        } else if (!locked) {
            lamp(pal::warnAmber());
            ImGui::TextColored(pal::warnAmber(), "Searching");
        } else if (!clean) {
            lamp(pal::warnAmber());
            ImGui::TextColored(pal::warnAmber(), "Locked, errors (%d/%d)", st.lastOk, st.lastBlocks);
        } else {
            lamp(pal::okGreen());
            ImGui::TextColored(pal::okGreen(), ps.playing ? "Locked, playing" : "Locked");
        }
        const double margin = st.esn0 - thresholdDb(st.mc);
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), locked ? "%+.1f dB margin" : "margin \xE2\x80\x93", margin);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, margin < 1 ? pal::badRed() : margin < 3 ? pal::warnAmber() : pal::okGreen());
        ImGui::ProgressBar(locked ? static_cast<float>(std::clamp(margin / 10.0, 0.0, 1.0)) : 0.f, ImVec2(-1, 0), lbl);
        ImGui::SetItemTooltip("Es/N0 above what this mode needs. Under 3 dB: close to dropping out");
        if (locked && margin < 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, pal::warnAmber());
            ImGui::TextWrapped("Signal too poor for this profile. A transmitter close by often overloads: lower the TX gain, turn its amplifier off and lower "
                               "the gain here. Or use the profile's robust form (needs about 5 dB less).");
            ImGui::PopStyleColor();
        }
        ImGui::PopStyleColor();
        if (run && st.frames > 0) drawConstellation(a.rx.constellation(), std::min(w - 18, 200.f));   // only with a signal to show
        // the Dash rule: only what is being measured shows a number
        const char* dash = "\xE2\x80\x93";
        const bool had = run && st.frames > 0;
        valueRow(a, "Mode", "%s", locked && st.mc ? st.mc->name : dash);
        if (locked) {
            valueRow(a, "Es/N0", "%.1f dB", st.esn0);
            valueRow(a, "MER", "%.1f dB", st.mer);
            ImGui::Dummy(ImVec2(0, 2));   // quality, then timing, then data
            valueRow(a, "Offset", "%.0f Hz", st.cfo);
            valueRow(a, "Clock", "%.1f ppm", st.ppm);
        } else {
            for (const char* k : {"Es/N0", "MER"}) valueRow(a, k, "%s", dash);
            ImGui::Dummy(ImVec2(0, 2));
            for (const char* k : {"Offset", "Clock"}) valueRow(a, k, "%s", dash);
        }
        ImGui::Dummy(ImVec2(0, 2));
        if (had) {
            valueRow(a, "Code words", "%ld/%ld", st.codeWordsOk, st.codeWords);
            valueRow(a, "Lost", "%ld", st.lost);
        } else {
            valueRow(a, "Code words", "%s", dash);
            valueRow(a, "Lost", "%s", dash);
        }
        if (st.fec > 0) valueRow(a, "Protection", "%s, %ld rebuilt", st.fec == 1 ? "1 in 16" : st.fec == 2 ? "2 in 32" : "1 in 8", st.rebuilt);
        if (st.restarts > 0) valueRow(a, "Restarts", "%ld", st.restarts);
    }
    ImGui::PushFont(a.bold);
    const bool progOpen = ImGui::CollapsingHeader("Programme", ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopFont();
    if (progOpen) {
        const char* dash = "\xE2\x80\x93";
        if (run && ps.srcW) valueRow(a, "Video", "%dx%d %.0f fps", ps.srcW, ps.srcH, ps.fps);
        else valueRow(a, "Video", "%s", dash);
        if (run && ps.videoKbps > 0) valueRow(a, "Video rate", "%.2f Mbit/s", ps.videoKbps / 1e3);
        else valueRow(a, "Video rate", "%s", dash);
        if (ps.playing) valueRow(a, "Buffer", "%.1f s", ps.bufferSec);
        else valueRow(a, "Buffer", "%s", dash);
        ImGui::SetItemTooltip("How far ahead of the picture on screen the received data is");
        if (ps.delaySec >= 0) {
            valueRow(a, "Delay", "%.2f s", ps.delaySec);
            ImGui::SetItemTooltip("From the transmitter's encoder to this screen (needs both clocks to agree: the same Mac, or synced Macs)");
        }
        if (!ps.soundDevice) valueRow(a, "Sound", "%s", "no sound output found");
        else if (run && ps.audioPackets > 0) valueRow(a, "Sound", "%ld packets", ps.audioPackets);
        else valueRow(a, "Sound", "%s", dash);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##vol", &a.volume, 0.f, 1.5f, "Volume %.2f")) a.player.setVolume(a.volume);
        ImGui::SetItemTooltip("Sound volume");
        if (st.droppedSec > 0.01) ImGui::TextColored(pal::badRed(), "The decoder fell behind: %.1f s of signal skipped (this Mac is too busy)", st.droppedSec);
        if (run && st.speed > 0) ImGui::TextDisabled("Decoder %.2fx real time", st.speed);
    }
    ImGui::EndChild();
}
