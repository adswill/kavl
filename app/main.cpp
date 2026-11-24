// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL: receiver and transmitter of the KAVL narrow-band video mode.
//   Receive:  IQ file / RTL-SDR (rtl_tcp) / HackRF -> KAVL receiver -> video + sound; signal state, link margin, constellation, spectrum, waterfall.
//   Transmit: a video file -> AV1 + Opus for a profile (ffmpeg) -> KAVL signal -> HackRF or IQ .wav.
// Options: --file IQ.wav (start receiving it), --profile N, --tab rx|tx, --mute, --hidden, --shot OUT.png [--shot-after S] (a picture of the
// window after S seconds, then quit). Checks without radios: --loopback (test pattern sent to this Mac's receiver), --fec N, --report (a status
// line every second on stdout), --popout (the video window too; hidden with --hidden; --shot also saves it as OUT_popout.png), --run-for S (quit after S seconds), --tx-gap A:B[:P] (the transmitter stops at A s and starts again at B s, with
// profile P), --retune T:MHz (the receive frequency changes at T s; more than one may be given).
#include "app.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <OpenGL/gl3.h>

#include "kavl/kavl_fec.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

void printReport(App& a, double t) {
    const auto st = a.rx.stats();
    const auto ps = a.player.status();
    const auto ts = a.tx.status();
    const auto rs = a.rig.status();
    const std::string rig = !a.rig.active() ? "-" : !rs.connected ? "connecting" : (rs.ptt ? "ptt@" : "rx@") + std::to_string(std::lround(rs.rigHz));
    std::printf("t=%5.1f rx frames=%ld lost=%ld profile=%d play=%d shown=%ld late=%ld buf=%.2f delay=%.2f snd=%ld restarts=%ld dropped=%.1f id=%s w=%.1f rig=%s | tx frames=%ld underruns=%ld\n", t, st.frames,
                st.lost, st.profile, ps.playing ? 1 : 0, ps.shown, ps.late, ps.bufferSec, ps.delaySec, ps.audioPackets, st.restarts, st.droppedSec, st.callsign.empty() ? "-" : st.callsign.c_str(), a.rx.signalWidthHz() / 1e3, rig.c_str(), ts.frames, ts.underruns);
    std::fflush(stdout);
}

void writeShot(GLFWwindow* w, const std::string& path) {
    int fw, fh;
    glfwGetFramebufferSize(w, &fw, &fh);
    std::vector<uint8_t> px(static_cast<size_t>(fw) * fh * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fw, fh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    const std::string ppm = path + ".ppm";
    if (FILE* f = std::fopen(ppm.c_str(), "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", fw, fh);
        for (int y = fh - 1; y >= 0; y--) std::fwrite(px.data() + static_cast<size_t>(y) * fw * 3, 1, static_cast<size_t>(fw) * 3, f);
        std::fclose(f);
        const std::string cmd = "sips -s format png '" + ppm + "' --out '" + path + "' >/dev/null 2>&1 && rm -f '" + ppm + "'";
        std::system(cmd.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string file, shot;
    double shotAfter = 8.0;
    bool hidden = false, mute = false, loopback = false, report = false, popoutArg = false;
    int profile = -1, tab = -1, fecArg = -1;
    int rxProfileArg = -2;
    int rigModelArg = 0;   // --rx-profile N: the receiver's own (-1 Auto), after --profile
    double runFor = 0, gapStop = 0, gapStart = 0;
    int winW = 1600, winH = 950;   // --size WxH (checks at other window sizes)
    float textScaleArg = 0;        // --text-scale F (checks)
    std::string callsignArg;       // --callsign TEXT (checks)
    std::vector<std::pair<bool, std::string>> convArgs;   // --rx-lo / --tx-lo MHz[:mirror]
    int gapProfile = -1;
    std::vector<std::pair<double, double>> retunes;   // --retune T:MHz: the receive frequency at T s   // --tx-gap A:B[:P]: the loopback transmitter stops at A s and starts again at B s (with profile P)
    for (int i = 1; i < argc; i++) {
        const std::string k = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (k == "--file") file = val();
        else if (k == "--profile") profile = std::clamp(std::atoi(val().c_str()), 0, kavl::kNumVideoProfiles - 1);
        else if (k == "--rig") rigModelArg = std::atoi(val().c_str());   // KAVL starts rigctld for this Hamlib model (1 = simulated rig)
        else if (k == "--rx-profile") rxProfileArg = std::clamp(std::atoi(val().c_str()), -1, kavl::kNumVideoProfiles - 1);   // -1 = Auto
        else if (k == "--shot") shot = val();
        else if (k == "--shot-after") shotAfter = std::atof(val().c_str());
        else if (k == "--hidden") hidden = true;
        else if (k == "--mute") mute = true;
        else if (k == "--tab") tab = val() == "tx" ? 1 : 0;
        else if (k == "--loopback") loopback = true;
        else if (k == "--fec") fecArg = std::clamp(std::atoi(val().c_str()), 0, kavl::fec::kNumProfiles - 1);
        else if (k == "--report") report = true;
        else if (k == "--popout") popoutArg = true;
        else if (k == "--size") std::sscanf(val().c_str(), "%dx%d", &winW, &winH);
        else if (k == "--text-scale") textScaleArg = static_cast<float>(std::atof(val().c_str()));
        else if (k == "--callsign") callsignArg = val();
        else if (k == "--rx-lo" || k == "--tx-lo") {   // MHz[:mirror] a converter between the radio and the antenna
            const std::string v = val();
            convArgs.emplace_back(k == "--rx-lo", v);
        }
        else if (k == "--run-for") runFor = std::atof(val().c_str());
        else if (k == "--tx-gap") std::sscanf(val().c_str(), "%lf:%lf:%d", &gapStop, &gapStart, &gapProfile);
        else if (k == "--retune") {   // T:MHz, may be given more than once
            double t = 0, f = 0;
            if (std::sscanf(val().c_str(), "%lf:%lf", &t, &f) == 2) retunes.emplace_back(t, f);
        }
    }
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    if (hidden) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* w = glfwCreateWindow(winW, winH, "KAVL", nullptr, nullptr);
    if (!w) return 1;
    glfwSetWindowSizeLimits(w, 960, 600, GLFW_DONT_CARE, GLFW_DONT_CARE);   // below this the station no longer fits
    glfwMakeContextCurrent(w);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;   // Tab and the arrows move between controls, Space or Return presses
    applyTheme();
    App app;
    gApp = &app;
    app.win = w;
    if (!shot.empty() || hidden) app.cfg = Settings();   // checks start from the defaults
    else loadSettings(app);
    if (profile >= 0) app.profile = app.txProfile = profile;
    if (rxProfileArg >= -1) app.profile = rxProfileArg;
    if (rigModelArg > 0) {
        app.rigCfg.mode = 2;
        app.rigCfg.model = rigModelArg;
        app.rigCfg.follow = 1;
        app.rig.start(app.rigCfg);
        app.showRig = true;
    }
    if (tab == 1) app.showTx = true;   // --tab tx: the transmit column on show (both are, on the combined screen)
    app.hidden = hidden;
    if (textScaleArg > 0) app.textScale = std::clamp(textScaleArg, 0.9f, 1.3f);
    if (!callsignArg.empty()) app.cs.on = true, app.cs.text = callsignArg;
    for (const auto& [rx, v] : convArgs) {
        Converter& c = rx ? app.rxConv : app.txConv;
        c.on = true;
        c.loMHz = std::atof(v.c_str());
        c.invert = v.find(":mirror") != std::string::npos;
    }
    if (app.cs.font.empty()) app.cs.font = resourceDir() + "/Inter-SemiBold.ttf";
    app.wantPopout = popoutArg;
    app.player.setVolume(mute ? 0.f : app.volume);
    if (mute) app.volume = 0;
    const std::string fd = resourceDir();
    app.ui = io.Fonts->AddFontFromFileTTF((fd + "/Inter-Regular.ttf").c_str(), 15.f);
    app.bold = io.Fonts->AddFontFromFileTTF((fd + "/Inter-SemiBold.ttf").c_str(), 15.f);
    app.mono = io.Fonts->AddFontFromFileTTF((fd + "/JetBrainsMono-Regular.ttf").c_str(), 14.f);
    app.big = io.Fonts->AddFontFromFileTTF((fd + "/Inter-SemiBold.ttf").c_str(), 22.f);
    if (!app.ui) app.ui = io.Fonts->AddFontDefault();
    if (!app.bold) app.bold = app.ui;
    if (!app.mono) app.mono = app.ui;
    if (!app.big) app.big = app.bold;
    io.FontDefault = app.ui;
    ImGui_ImplGlfw_InitForOpenGL(w, true);
    ImGui_ImplOpenGL3_Init("#version 150");
    glfwSetDropCallback(w, [](GLFWwindow*, int n, const char** paths) {
        if (!gApp || n < 1) return;
        const std::string p = paths[0];
        const std::string ext = std::filesystem::path(p).extension().string();
        if (ext != ".wav" && ext != ".WAV") {   // a video to transmit; a .wav is IQ to receive
            gApp->videoPath = p;
            gApp->txSource = 0;
        } else {
            gApp->iqPath = p;
            gApp->srcKind = 0;
            if (const double f = frequencyFromName(p)) gApp->freqMHz = f / 1e6;
        }
    });
    if (app.srcKind > 0) refreshDevices(app);
    if (!file.empty()) {
        app.iqPath = file;
        app.srcKind = 0;
        if (const double f = frequencyFromName(file)) app.freqMHz = f / 1e6;
        startRx(app);
    }
    if (fecArg >= 0) app.txFec = fecArg;
    if (loopback) {
        app.txSource = 2;
        app.txDest = 2;
        app.srcKind = 3;
        app.txFreqMHz = app.freqMHz;
        startTx(app);
        startRx(app);
    }
    const double t0 = glfwGetTime();
    double nextReport = 1.0;
    double lastFrame = 0;
    while (!glfwWindowShouldClose(w)) {
        // Draw only as often as something can change: idle (nothing received, sent or encoded), sleep until input, at least 4 times a second;
        // running, at most 60 frames a second (the picture is never faster; a 120 Hz screen would double the drawing for nothing).
        const bool busy = app.rx.running() || app.tx.running() || app.tx.preparing() || app.popout.isOpen() || app.txStopping || app.listingCams ||
                          !shot.empty() || report || ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (busy) glfwPollEvents();
        else glfwWaitEventsTimeout(0.25);
        const double since = glfwGetTime() - lastFrame;
        if (since < 1.0 / 60.0) std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>((1.0 / 60.0 - since) * 1e6)));
        lastFrame = glfwGetTime();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        frame(app);
        ImGui::Render();
        int fw, fh;
        glfwGetFramebufferSize(w, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0.055f, 0.058f, 0.062f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (app.wantPopout != app.popout.isOpen()) {
            if (app.wantPopout) app.wantPopout = app.popout.open(w, !app.hidden);
            else app.popout.close();
        }
        app.popout.draw(w, app.havePic && app.rx.running() ? app.video.texture() : 0, app.video.width(), app.video.height());
        app.wantPopout = app.popout.isOpen();
        if (report && glfwGetTime() - t0 > nextReport) {
            nextReport += 1.0;
            printReport(app, glfwGetTime() - t0);
        }
        if (report) {   // events, to the frame: lock gained or lost, the picture starting or stopping (how long a retune takes to show)
            static bool locked = false, moving = false;
            static long lastShown = 0;
            static double lastMove = 0;
            const auto st = app.rx.stats();
            const auto ps = app.player.status();
            const double t = glfwGetTime() - t0;
            const bool lk = app.rx.running() && lockedNow(st);
            if (lk != locked) std::printf("EVENT t=%.2f %s\n", t, lk ? "locked" : "lock lost"), locked = lk;
            if (ps.shown != lastShown) lastShown = ps.shown, lastMove = t;
            const bool mv = t - lastMove < 0.4;
            if (mv != moving) std::printf("EVENT t=%.2f %s\n", t, mv ? "picture" : "picture stopped"), moving = mv;
            std::fflush(stdout);
        }
        if (runFor > 0 && glfwGetTime() - t0 > runFor) break;
        for (auto& rt : retunes)
            if (rt.first > 0 && glfwGetTime() - t0 > rt.first) {
                setRxFrequency(app, rt.second * 1e6, Tune::Plot);   // as a click on the spectrum
                if (report) std::printf("EVENT t=%.2f retune %.4f MHz\n", glfwGetTime() - t0, rt.second);
                rt.first = 0;
            }
        if (gapStop > 0 && app.tx.running() && glfwGetTime() - t0 > gapStop && glfwGetTime() - t0 < gapStart) stopTxAsync(app);
        if (gapStart > 0 && !app.tx.running() && !app.txStopping && glfwGetTime() - t0 > gapStart) {
            if (gapProfile >= 0) app.txProfile = gapProfile;
            startTx(app);
            gapStart = 0;
        }
        if (!shot.empty() && app.popout.isOpen() && glfwGetTime() - t0 > shotAfter - 0.2) app.popout.shotNext(shot.substr(0, shot.size() - 4) + "_popout.png");
        if (!shot.empty() && glfwGetTime() - t0 > shotAfter) {
            writeShot(w, shot);
            break;
        }
        glfwSwapBuffers(w);
    }
    app.rx.stop();
    waitTxStop(app);
    app.tx.stop();
    app.rig.stop();   // PTT released, rigctld stopped if KAVL started it
    if (app.camTh.joinable()) app.camTh.join();
    if (shot.empty() && !hidden) saveSettings(app);
    if (app.csTex) glDeleteTextures(1, &app.csTex);
    app.video.release();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(w);
    glfwTerminate();
    return 0;
}
