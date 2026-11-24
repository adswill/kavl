// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app: Radio > Rig control, the window for a ham transceiver through Hamlib's rigctld (rig.h).
#include "app.h"
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {
// the serial ports macOS offers (/dev/cu.*), for the rig's CAT cable
std::vector<std::string> serialPorts() {
    std::vector<std::string> v;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/dev", ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("cu.", 0) == 0 && n != "cu.Bluetooth-Incoming-Port") v.push_back(e.path().string());
    }
    std::sort(v.begin(), v.end());
    return v;
}
}  // namespace

void rigWindow(App& a) {
    if (!a.showRig) return;
    ImGui::SetNextWindowSize(ImVec2(px(460), 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Rig control", &a.showRig, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    RigSettings& r = a.rigCfg;
    const bool on = a.rig.active();
    const auto st = a.rig.status();
    const bool haveHamlib = !RigControl::findRigctld().empty();
    ImGui::TextWrapped("A transceiver controlled through Hamlib's rigctld (most rigs with a CAT port): KAVL keys it (PTT) while it transmits, so a "
                       "sequencer, amplifier or preamp relay switches first, and can keep it on your frequency.");
    ImGui::Spacing();
    if (on) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##rigmode", &r.mode, "Off\0A rigctld that is already running\0Start rigctld for my rig\0");
    if (r.mode == 1) {
        static char host[64];
        std::snprintf(host, sizeof(host), "%s", r.host.c_str());
        ImGui::SetNextItemWidth(-px(100));
        if (ImGui::InputText("##righost", host, sizeof(host))) r.host = host;
        ImGui::SetItemTooltip("Where rigctld runs (127.0.0.1: this Mac)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##rigport", &r.port, 0);
        ImGui::SetItemTooltip("Its port (rigctld's own default is 4532)");
    } else if (r.mode == 2) {
        ImGui::SetNextItemWidth(px(120));
        ImGui::InputInt("Rig model", &r.model, 0);
        ImGui::SetItemTooltip("Hamlib's number for your rig: run  rigctl -l  in Terminal to list them (e.g. 3085 IC-705, 3081 IC-9700, 3073 IC-7300, "
                              "1035 FT-991, 1020 FT-817, 2014 TS-2000, 2045 KX3). 1 is a simulated rig, to try this without one.");
        const auto ports = serialPorts();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##rigdev", r.device.empty() ? "Serial port (none)" : r.device.c_str())) {
            if (ImGui::Selectable("(none)", r.device.empty())) r.device.clear();
            for (const auto& p : ports)
                if (ImGui::Selectable(p.c_str(), p == r.device)) r.device = p;
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The rig's CAT port (USB cable); plug it in first");
        static const int bauds[] = {0, 4800, 9600, 19200, 38400, 57600, 115200};
        int bi = 0;
        for (int i = 0; i < 7; i++)
            if (bauds[i] == r.baud) bi = i;
        ImGui::SetNextItemWidth(px(160));
        if (ImGui::Combo("Speed", &bi, "Rig default\0004800\0009600\00019200\00038400\00057600\000115200\0")) r.baud = bauds[bi];
        ImGui::SetNextItemWidth(px(160));
        ImGui::Combo("PTT through", &r.pttType, "CAT (the rig)\0RTS line\0DTR line\0");
        ImGui::SetItemTooltip("CAT: the rig keys itself. RTS or DTR: a line on the serial port keys a PTT interface or sequencer");
    }
    if (on) ImGui::EndDisabled();
    if (r.mode != 0) {
        ImGui::Checkbox("Key the rig while transmitting (PTT)", &r.ptt);
        ImGui::SetItemTooltip("Only when sending to the HackRF (on the air), not to loopback or a file. If the rig does not key, nothing is sent.");
        if (r.ptt) {
            ImGui::SetNextItemWidth(px(200));
            ImGui::SliderInt("##lead", &r.leadMs, 0, 1000, "Lead %d ms");
            ImGui::SetItemTooltip("Time from PTT to the first sample: relays switch first");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##tail", &r.tailMs, 0, 1000, "Tail %d ms");
            ImGui::SetItemTooltip("Time from the last sample to releasing PTT");
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##follow", &r.follow, "The rig keeps its own frequency\0The rig follows the receive frequency\0The rig follows the transmit "
                                                 "frequency\0"))
            rigFollow(a);
        ImGui::Spacing();
        if (!on) {
            const bool can = r.mode == 1 || haveHamlib;
            if (actionButton(a, "Connect", Action::Primary, can)) {
                a.rig.start(r);
                rigFollow(a);
            }
            if (!haveHamlib && r.mode == 2) ImGui::TextColored(pal::warnAmber(), "Hamlib is not installed: brew install hamlib");
        } else if (ImGui::Button("Disconnect", ImVec2(-1, 0))) {
            a.rig.stop();
        }
        if (on) {
            if (st.connected) {
                lamp(st.ptt ? pal::badRed() : pal::okGreen());
                ImGui::Text("Connected%s", st.ptt ? ", PTT on (transmitting)" : "");
                if (st.rigHz > 0) ImGui::TextDisabled("The rig is on %.4f MHz", st.rigHz / 1e6);
            } else {
                lamp(pal::warnAmber());
                ImGui::TextColored(pal::warnAmber(), "Not connected");
            }
        }
        if (!st.error.empty()) ImGui::TextWrapped("%s", st.error.c_str());
    }
    ImGui::End();
}
