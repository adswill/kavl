// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app the Transmit column (app.h).
#include "app.h"

#include "kavl/kavl_fec.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

void transmitPanel(App& a, float w, float h) {
    ImGui::BeginChild("##right", ImVec2(w, h), ImGuiChildFlags_Borders);
    const bool txBusy = a.tx.preparing() || a.tx.running();
    const bool txRun = a.tx.running();
    onAirBand(a);
    ImGui::PushFont(a.bold);
    const bool open = ImGui::CollapsingHeader("Transmit", ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopFont();
    const auto& vp = kavl::videoProfile(a.txProfile);
    if (open && !txRun) {   // what and how: set before starting; while transmitting they fold into the summary line below
        heading(a, "Programme");
        if (txBusy) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##txsrc", &a.txSource, "Video file\0Camera (live)\0Test pattern (live)\0");
        ImGui::SetItemTooltip("What to send: a video file (encoded first), the camera and microphone, or a test pattern with a tone");
        if (txBusy) ImGui::EndDisabled();
        if (a.txSource == 0) {
            if (txBusy) ImGui::BeginDisabled();
            if (ImGui::Button("Choose a video...", ImVec2(-1, 0))) a.chooser.open(2, "Choose a video to transmit", false);
            ImGui::SetItemTooltip("Or drop a video file on the window");
            if (txBusy) ImGui::EndDisabled();
            ImGui::TextWrapped("%s", a.videoPath.empty() ? "(no video chosen)" : std::filesystem::path(a.videoPath).filename().string().c_str());
        } else {
            if (a.txSource == 1 && !a.devicesListed) listDevices(a);
            if (txBusy) ImGui::BeginDisabled();
            if (a.txSource == 1) {
                if (a.listingCams) {
                    ImGui::TextDisabled("Looking for cameras...");
                } else if (a.cameras.empty()) {
                    ImGui::TextColored(pal::warnAmber(), "No camera found. Check that a camera is connected and that KAVL may use it (System Settings, Privacy, Camera).");
                } else {
                    ImGui::SetNextItemWidth(-1);
                    ImGui::Combo("##cam", &a.camera, [](void* d, int i) { return (*static_cast<std::vector<CaptureDevice>*>(d))[static_cast<size_t>(i)].name.c_str(); },
                                 &a.cameras, static_cast<int>(a.cameras.size()));
                    ImGui::SetItemTooltip("Camera (kept by name)");
                    int mic = a.microphone + 1;   // 0 = no sound
                    std::vector<std::string> names = {"No sound"};
                    for (const auto& m : a.microphones) names.push_back(m.name);
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::Combo("##mic", &mic, [](void* d, int i) { return (*static_cast<std::vector<std::string>*>(d))[static_cast<size_t>(i)].c_str(); }, &names,
                                     static_cast<int>(names.size())))
                        a.microphone = mic - 1;
                    ImGui::SetItemTooltip("Microphone (kept by name; the Mac's own one unless you pick another)");
                }
                if (ImGui::Button("Look for cameras again", ImVec2(-1, 0))) {
                    if (a.camera >= 0 && a.camera < static_cast<int>(a.cameras.size())) a.cameraName = a.cameras[static_cast<size_t>(a.camera)].name;
                    a.microphoneName = a.microphone < 0 ? "none" : a.microphone < static_cast<int>(a.microphones.size()) ? a.microphones[static_cast<size_t>(a.microphone)].name : a.microphoneName;
                    listDevices(a);
                }
            }
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##fps", &a.liveFps, "30 fps\0" "60 fps\0");
            ImGui::SetItemTooltip("Frame rate of the live picture (a camera that has only 30 fps sends 30)");
            if (txBusy) ImGui::EndDisabled();
        }
        heading(a, "Profile");
        if (txBusy) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##txprof", &a.txProfile, [](void*, int i) { return kavl::videoProfile(i).name; }, nullptr, kavl::kNumVideoProfiles);
        ImGui::SetItemTooltip("Picture size, frame rate and signal width. Robust: needs about 5 dB less signal, is wider");
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##txfec", &a.txFec, [](void*, int i) { return i == 0 ? "No extra protection" : kavl::fec::profileName(i); }, nullptr, kavl::fec::kNumProfiles);
        ImGui::SetItemTooltip("Parity frames: any lost frame of a group is rebuilt (a burst of interference, a fade). Costs video bits, and adds one group of delay");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%dx%d at %d fps: AV1 %d kbit/s + Opus %s %d kbit/s, %s, %.0f kHz wide, needs about %.1f dB", vp.width, vp.height,
                           a.txSource == 0 ? vp.fps : std::min(vp.fps, a.liveFps ? 60 : 30), kavl::videoKbps(vp, a.txFec), vp.soundChannels == 1 ? "mono" : "stereo",
                           vp.soundKbps, kavl::modcod(vp.modcod)->name,
                           kavl::occupiedHz(vp.symRate) / 1e3, thresholdDb(kavl::modcod(vp.modcod)));
        ImGui::PopStyleColor();
        if (a.txSource != 0 && vp.width > 2560) ImGui::TextColored(pal::warnAmber(), "Live 4K: the real-time encoder gives a softer picture than an encoded file.");
        if (a.txSource == 0) {
            if (ImGui::Button("Encode for this profile", ImVec2(-1, 0)) && !a.videoPath.empty())
                a.tx.prepare(a.videoPath, a.txProfile, a.txFec, callsignImage(a), callsignSig(a));
            ImGui::SetItemTooltip("AV1 + Opus for this profile with ffmpeg (slow, best quality); once per video and profile");
        }
        if (txBusy) ImGui::EndDisabled();
        if (a.txSource == 0 && (a.tx.preparing() || a.tx.ready() || !a.tx.prepareMessage().empty())) {
            ImGui::ProgressBar(a.tx.progress(), ImVec2(-1, 0));
            ImGui::TextWrapped("%s", a.tx.prepareMessage().c_str());
        }
        callsignEditor(a, txBusy);
    }
    if (open) {   // where: the radio, frequency and gain stay live while transmitting
        heading(a, "Send to");
        if (txRun) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(-1);
        if (!txRun) ImGui::Combo("##txdest", &a.txDest, "HackRF\0IQ file (.wav)\0Loopback (this Mac's receiver)\0");
        ImGui::SetItemTooltip("HackRF: on the air. IQ file: for later. Loopback: straight into this Mac's receiver (Receive source: Loopback)");
        if (!txRun && a.txDest == 0 && a.hrfDevs.size() > 1) {
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##txdev", &a.txHrfDev, [](void* d, int i) { return (*static_cast<std::vector<RadioDevice>*>(d))[static_cast<size_t>(i)].label.c_str(); },
                         &a.hrfDevs, static_cast<int>(a.hrfDevs.size()));
            ImGui::SetItemTooltip("Which HackRF sends");
        }
        if (!txRun && a.txDest == 1) {
            if (ImGui::Button("Save as...", ImVec2(-1, 0))) a.chooser.open(3, "Save the KAVL IQ file", true, "kavl_programme.wav");
            ImGui::TextWrapped("%s", a.txFilePath.empty() ? "(no file chosen)" : a.txFilePath.c_str());
        }
        if (txRun) ImGui::EndDisabled();
        // the frequency, the gain and the amplifier follow at once while transmitting
        if (a.txDest != 1) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputDouble("##txf", &a.txFreqMHz, 0.1, 1.0, "%.4f MHz") && txRun) a.tx.setFrequency(a.txFreqMHz * 1e6);
            ImGui::SetItemTooltip("Transmit frequency; changes at once while transmitting");
        }
        if (a.txDest == 0) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##txg", &a.txGain, 0, 47, "TX gain %d dB") && txRun) a.tx.setGain(a.txGain);
            ImGui::SetItemTooltip("HackRF transmit gain (VGA); changes at once");
            if (ImGui::Checkbox("RF amplifier (+14 dB)", &a.txAmp) && txRun) a.tx.setAmp(a.txAmp);
            ImGui::SetItemTooltip("The HackRF's RF amplifier; switch it off when the receiver is close by");
            if (!txRun) {   // the reminder before starting; the ON AIR band says the rest
                ImGui::PushStyleColor(ImGuiCol_Text, pal::warnAmber());
                ImGui::TextWrapped("Transmit only where your licence and the band plan allow it (%.0f kHz wide).", kavl::occupiedHz(vp.symRate) / 1e3);
                ImGui::PopStyleColor();
            }
        }
        const bool fileReady = a.tx.ready() && a.tx.profile() == a.txProfile && a.tx.fec() == a.txFec && a.tx.overlaySignature() == callsignSig(a);
        const bool liveReady = a.txSource == 2 || (a.txSource == 1 && !a.cameras.empty());
        const bool canStart = (a.txSource == 0 ? fileReady : liveReady) && (a.txDest != 1 || !a.txFilePath.empty()) && !a.txStopping;
        if (!txRun) {
            if (actionButton(a, a.txDest != 1 ? "Start transmitting" : "Write the IQ file", Action::Primary, canStart)) startTx(a);
            if (!canStart) {   // say what is missing
                const char* why = a.txSource == 0 && a.videoPath.empty()               ? "Choose a video first."
                                  : a.txSource == 0 && a.tx.preparing()                ? "Encoding the video; this starts when it is ready."
                                  : a.txSource == 0 && !fileReady                      ? "Encode the video for this profile first."
                                  : a.txSource == 1 && a.listingCams                   ? "Looking for cameras..."
                                  : a.txSource == 1 && a.cameras.empty()               ? "No camera found."
                                  : a.txDest == 1 && a.txFilePath.empty()              ? "Choose where to save the IQ file (Save as...)."
                                                                                       : "";
                if (*why) ImGui::TextDisabled("%s", why);
            }
            if (a.txSource == 0 && a.tx.ready() && (a.tx.profile() != a.txProfile || a.tx.fec() != a.txFec || a.tx.overlaySignature() != callsignSig(a)))
                ImGui::TextColored(pal::warnAmber(), "The profile, protection or callsign changed: encode again.");
        } else {
            if (a.txStopping) {   // the stop runs on a thread: say so, and take no second press
                ImGui::BeginDisabled();
                ImGui::Button("Stopping...", ImVec2(-1, ImGui::GetFrameHeight() + 8));
                ImGui::EndDisabled();
            } else if (a.txDest == 0) {   // on the air: the red stop
                if (actionButton(a, "Stop transmitting", Action::OnAir)) stopTxAsync(a);
            } else if (ImGui::Button(a.txDest == 1 ? "Stop writing" : "Stop sending", ImVec2(-1, ImGui::GetFrameHeight() + 8))) {
                stopTxAsync(a);   // loopback and file are not on the air: the plain button
            }
        }
    }
    const auto& ts = a.now.tx;
    if (a.tx.running() || ts.frames > 0 || !ts.message.empty()) {
        ImGui::PushFont(a.bold);
        // what the transmitter is doing: on the air (HackRF), sending to this Mac's receiver, or writing a file
        const char* title = a.txDest == 0 ? "On air###txstate" : a.txDest == 2 ? "Loopback###txstate" : "Writing###txstate";
        const bool airOpen = ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopFont();
        if (airOpen) {
            if (!ts.message.empty()) ImGui::TextWrapped("%s", ts.message.c_str());
            valueRow(a, a.txDest == 1 ? "Written" : "Running", "%.1f s", ts.seconds);
            if (ts.programmeSec > 0) valueRow(a, "Programme", "%.1f s, loops %ld", ts.programmeSec, ts.loops);
            valueRow(a, "Frames", "%ld", ts.frames);
            valueRow(a, "Sample rate", "%.3f Msps", ts.sampleRate / 1e6);
            valueRow(a, "Payload", "%.3f Mbit/s", ts.payloadMbps);
            if (ts.underruns) ImGui::TextColored(pal::warnAmber(), "The radio ran out of data %ld times (this Mac was too busy to keep up)", ts.underruns);
            if (a.tx.live()) {
                const auto ls = a.tx.liveStatus();
                valueRow(a, "Encoder", "%.1f fps, %.2f Mbit/s", ls.fps, ls.kbps / 1e3);
                valueRow(a, "Sound", "%ld packets", ls.audioPackets);
                if (ls.captureW) valueRow(a, "Camera", "%dx%d at %.0f fps", ls.captureW, ls.captureH, ls.captureFps);
                const std::string le = a.tx.liveError();
                if (!le.empty() && ls.videoUnits == 0) {   // notices are harmless once video flows
                    ImGui::PushStyleColor(ImGuiCol_Text, pal::warnAmber());
                    ImGui::TextWrapped("No picture from the live encoder yet.%s", a.txSource == 1 ? " Check that KAVL may use the camera and microphone (System Settings, Privacy & Security)." : "");
                    ImGui::PopStyleColor();
                    ImGui::TextDisabled("ffmpeg said:");
                    ImGui::PushFont(a.mono);
                    ImGui::TextWrapped("%s", le.c_str());
                    ImGui::PopFont();
                }
            }
        }
    }
    if (txRun) {   // what is being sent, in one line (the choices are locked while transmitting)
        static const char* srcNames[] = {"Video file", "Camera", "Test pattern"};
        const int fps = a.txSource == 0 ? vp.fps : std::min(vp.fps, a.liveFps ? 60 : 30);
        heading(a, "Sending");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s%s, %s, %d fps, %s", srcNames[std::clamp(a.txSource, 0, 2)], a.txSource == 0 ? "" : " (live)", vp.name, fps,
                           a.txFec == 0 ? "no extra protection" : kavl::fec::profileName(a.txFec));
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
}
