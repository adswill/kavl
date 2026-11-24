// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL: receiver and transmitter of the KAVL narrow-band video mode.
//   Receive:  IQ file / RTL-SDR (rtl_tcp) / HackRF -> KAVL receiver -> video + sound; signal state, link margin, constellation, spectrum, waterfall.
//   Transmit: a video file -> AV1 + Opus for a profile (ffmpeg) -> KAVL signal -> HackRF or IQ .wav.
// Options: --file IQ.wav (start receiving it), --profile N, --tab rx|tx, --mute, --hidden, --shot OUT.png [--shot-after S] (a picture of the
// window after S seconds, then quit).
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>
#include <OpenGL/gl3.h>
#include <mach-o/dyld.h>

#include "player.h"
#include "rx_engine.h"
#include "settings.h"
#include "theme.h"
#include "tx_engine.h"
#include "video_gl.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef KAVL_FONT_DIR
#define KAVL_FONT_DIR "."
#endif
#ifndef KAVL_VERSION
#define KAVL_VERSION "1.0"
#endif

namespace {

// Es/N0 a modcod needs (measured where known, else the Shannon limit of its bits per symbol + 2.5 dB)
double thresholdDb(const kavl::Modcod* m) {
    if (!m) return 0;
    if (m->id == 2) return 13.5;
    if (m->id == 3) return 18.0;
    if (m->id == 4) return 19.5;
    if (m->id == 5) return 22.0;
    const double bits = m->bitsPerSym * m->rate15 / 15.0;
    return 10.0 * std::log10(std::pow(2.0, bits) - 1.0) + 2.5;
}

// The macOS file dialog (osascript), on a thread; the result is picked up by poll().
struct FileChooser {
    std::mutex m;
    std::string result;
    bool busy = false, done = false;
    int target = 0;
    std::thread th;
    void open(int which, const char* prompt, bool save, const std::string& defaultName = "") {
        if (busy) return;
        if (th.joinable()) th.join();
        busy = true;
        done = false;
        target = which;
        const std::string script = save ? std::string("POSIX path of (choose file name with prompt \"") + prompt + "\" default name \"" + defaultName + "\")"
                                        : std::string("POSIX path of (choose file with prompt \"") + prompt + "\")";
        th = std::thread([this, script] {
            const std::string cmd = "osascript -e '" + script + "' 2>/dev/null";
            std::string out;
            if (FILE* p = popen(cmd.c_str(), "r")) {
                char buf[2048];
                while (std::fgets(buf, sizeof(buf), p)) out += buf;
                pclose(p);
            }
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
            std::lock_guard<std::mutex> lk(m);
            result = out;
            done = true;
            busy = false;
        });
    }
    bool poll(int which, std::string& out) {
        std::lock_guard<std::mutex> lk(m);
        if (!done || target != which) return false;
        done = false;
        if (result.empty()) return false;
        out = result;
        return true;
    }
    ~FileChooser() {
        if (th.joinable()) th.join();
    }
};

uint32_t wfColour(float v) {
    v = std::clamp(v, 0.f, 1.f);
    const float r = std::clamp(2.6f * v - 1.6f, 0.f, 1.f), g = std::clamp(1.6f * v - 0.35f, 0.f, 1.f), b = std::clamp(0.10f + 1.3f * v, 0.f, 1.f);
    return 0xFF000000u | (static_cast<uint32_t>(b * 255) << 16) | (static_cast<uint32_t>(g * 255) << 8) | static_cast<uint32_t>(r * 255);
}

std::string resourceDir() {
    // inside KAVL.app: Contents/MacOS/kavl -> Contents/Resources; else the fonts of the source tree
    char buf[4096];
    uint32_t n = sizeof(buf);
    if (_NSGetExecutablePath(buf, &n) == 0) {
        const auto res = std::filesystem::path(buf).parent_path().parent_path() / "Resources";
        if (std::filesystem::exists(res / "Inter-Regular.ttf")) return res.string();
    }
    return KAVL_FONT_DIR;
}

struct App {
    Player player;
    RxEngine rx{player};
    TxEngine tx;
    FileChooser chooser;
    Settings cfg;
    VideoGl video;
    ImFont *ui = nullptr, *bold = nullptr, *mono = nullptr, *big = nullptr;
    int tab = 0;
    // receive
    int srcKind = 0;   // 0 IQ file, 1 RTL-SDR, 2 HackRF
    std::string iqPath;
    double freqMHz = 433.0;
    int profile = 4;
    std::string rxError;
    float volume = 1.f;
    RtlSettings rtl;
    HackRfSettings hrf;
    std::vector<RadioDevice> rtlDevs, hrfDevs;
    int rtlDev = 0, hrfDev = 0;
    char rtlHost[128] = "127.0.0.1";
    // transmit
    std::string videoPath, txFilePath;
    int txProfile = 4, txDest = 0, txGain = 20, txHrfDev = 0;
    double txFreqMHz = 433.0;
    bool txAmp = false;
    // view
    bool videoOnly = false, fullscreen = false, showWaterfall = true, showAbout = false, showLicences = false;
    unsigned wfTex = 0;
    static constexpr int kWfW = RxEngine::kSpecBins, kWfH = 256;
    std::vector<uint32_t> wf = std::vector<uint32_t>(static_cast<size_t>(kWfW) * kWfH, 0xFF000000u);
    std::vector<float> spec;
    double specFs = 0, specOff = 0;
    float dbLo = -100.f, dbHi = -40.f;
    bool havePic = false;
    GLFWwindow* win = nullptr;
    int winX = 100, winY = 100, winW = 1600, winH = 950;
};

App* gApp = nullptr;

void loadSettings(App& a) {
    Settings& c = a.cfg;
    c.load();
    a.srcKind = static_cast<int>(c.num("rx.source", 0));
    a.iqPath = c.str("rx.file");
    a.freqMHz = c.num("rx.freqMHz", 433.0);
    a.profile = std::clamp(static_cast<int>(c.num("rx.profile", 4)), 0, kavl::kNumVideoProfiles - 1);
    a.volume = static_cast<float>(c.num("rx.volume", 1.0));
    a.rtl.remote = c.flag("rtl.remote", false);
    std::snprintf(a.rtlHost, sizeof(a.rtlHost), "%s", c.str("rtl.host", "127.0.0.1").c_str());
    a.rtl.port = static_cast<int>(c.num("rtl.port", 1234));
    a.rtl.gainTenthsDb = static_cast<int>(c.num("rtl.gain", -1));
    a.rtl.ppm = static_cast<int>(c.num("rtl.ppm", 0));
    a.rtl.biasTee = c.flag("rtl.biasTee", false);
    a.rtl.agc = c.flag("rtl.agc", false);
    a.hrf.lna = static_cast<int>(c.num("hackrf.lna", 24));
    a.hrf.vga = static_cast<int>(c.num("hackrf.vga", 20));
    a.hrf.amp = c.flag("hackrf.amp", false);
    a.hrf.antennaPower = c.flag("hackrf.antennaPower", false);
    a.txProfile = std::clamp(static_cast<int>(c.num("tx.profile", 4)), 0, kavl::kNumVideoProfiles - 1);
    a.txFreqMHz = c.num("tx.freqMHz", 433.0);
    a.txGain = static_cast<int>(c.num("tx.gain", 20));
    a.txAmp = c.flag("tx.amp", false);
    a.txDest = static_cast<int>(c.num("tx.dest", 0));
    a.showWaterfall = c.flag("view.waterfall", true);
}

void saveSettings(App& a) {
    Settings& c = a.cfg;
    c.set("rx.source", a.srcKind);
    c.set("rx.file", a.iqPath);
    c.set("rx.freqMHz", a.freqMHz);
    c.set("rx.profile", a.profile);
    c.set("rx.volume", static_cast<double>(a.volume));
    c.set("rtl.remote", a.rtl.remote);
    c.set("rtl.host", std::string(a.rtlHost));
    c.set("rtl.port", a.rtl.port);
    c.set("rtl.gain", a.rtl.gainTenthsDb);
    c.set("rtl.ppm", a.rtl.ppm);
    c.set("rtl.biasTee", a.rtl.biasTee);
    c.set("rtl.agc", a.rtl.agc);
    c.set("hackrf.lna", a.hrf.lna);
    c.set("hackrf.vga", a.hrf.vga);
    c.set("hackrf.amp", a.hrf.amp);
    c.set("hackrf.antennaPower", a.hrf.antennaPower);
    c.set("tx.profile", a.txProfile);
    c.set("tx.freqMHz", a.txFreqMHz);
    c.set("tx.gain", a.txGain);
    c.set("tx.amp", a.txAmp);
    c.set("tx.dest", a.txDest);
    c.set("view.waterfall", a.showWaterfall);
    c.save();
}

void refreshDevices(App& a) {
    a.rtlDevs = listRtlSdr();
    a.hrfDevs = listHackRf();
    a.rtlDev = std::clamp(a.rtlDev, 0, std::max(0, static_cast<int>(a.rtlDevs.size()) - 1));
    a.hrfDev = std::clamp(a.hrfDev, 0, std::max(0, static_cast<int>(a.hrfDevs.size()) - 1));
}

void uploadPicture(App& a) {
    Picture& p = a.player.picture();
    std::lock_guard<std::mutex> lk(p.m);
    if (!p.fresh || p.w <= 0 || p.h <= 0) return;
    if (a.video.update(p.y.data(), p.u.data(), p.v.data(), p.w, p.h)) a.havePic = true;
    p.fresh = false;
}

void updateWaterfall(App& a) {
    if (!a.rx.running() || !a.rx.spectrum(a.spec, a.specFs, a.specOff)) return;
    std::vector<float> sorted(a.spec);
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<long>(sorted.size() / 5), sorted.end());
    const float floorDb = sorted[sorted.size() / 5];
    const float peak = *std::max_element(a.spec.begin(), a.spec.end());
    a.dbLo += ((floorDb - 8.f) - a.dbLo) * 0.05f;
    a.dbHi += (std::max(peak + 4.f, floorDb + 25.f) - a.dbHi) * 0.05f;
    std::memmove(a.wf.data() + App::kWfW, a.wf.data(), sizeof(uint32_t) * App::kWfW * (App::kWfH - 1));
    for (int x = 0; x < App::kWfW; x++) a.wf[static_cast<size_t>(x)] = wfColour((a.spec[static_cast<size_t>(x)] - a.dbLo) / (a.dbHi - a.dbLo));
    if (!a.wfTex) {
        glGenTextures(1, &a.wfTex);
        glBindTexture(GL_TEXTURE_2D, a.wfTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, App::kWfW, App::kWfH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    glBindTexture(GL_TEXTURE_2D, a.wfTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, App::kWfW, App::kWfH, GL_RGBA, GL_UNSIGNED_BYTE, a.wf.data());
}

void startRx(App& a) {
    a.rxError.clear();
    const auto& vp = kavl::videoProfile(a.profile);
    const double width = kavl::occupiedHz(vp.symRate);
    std::unique_ptr<IqSource> src;
    if (a.srcKind == 0) {
        if (a.iqPath.empty()) {
            a.rxError = "Choose an IQ file first (File > Open IQ file, or drop one on the window).";
            return;
        }
        src = makeFileSource(a.iqPath);
    } else if (a.srcKind == 1) {
        RtlSettings s = a.rtl;
        s.host = a.rtlHost;
        if (!s.remote) {
            if (a.rtlDevs.empty()) refreshDevices(a);
            if (a.rtlDevs.empty()) {
                a.rxError = "No RTL-SDR found on USB.";
                return;
            }
            s.index = a.rtlDevs[static_cast<size_t>(a.rtlDev)].index;
        }
        src = makeRtlSource(s, a.freqMHz * 1e6, width);
    } else {
        if (a.hrfDevs.empty()) refreshDevices(a);
        HackRfSettings s = a.hrf;
        if (!a.hrfDevs.empty()) s.serial = a.hrfDevs[static_cast<size_t>(a.hrfDev)].serial;
        src = makeHackRfSource(s, a.freqMHz * 1e6, width);
    }
    if (!a.rx.start(std::move(src), a.profile)) a.rxError = a.rx.error();
    a.havePic = false;
    std::fill(a.wf.begin(), a.wf.end(), 0xFF000000u);
}

// ---- drawing helpers ------------------------------------------------------------------------------------------------------------------------------------
void lamp(ImVec4 col) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + h * 0.4f, p.y + h * 0.5f), h * 0.32f, ImGui::GetColorU32(col));
    ImGui::Dummy(ImVec2(h * 0.9f, h));
    ImGui::SameLine();
}

void heading(App& a, const char* t) {
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
    ImGui::SameLine(110);
    ImGui::PushFont(a.mono);
    ImGui::TextUnformatted(buf);
    ImGui::PopFont();
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

void drawVideo(App& a, ImVec2 size, bool hint) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(0, 0, 0, 255));
    const float aspect = a.video.height() > 0 ? static_cast<float>(a.video.width()) / static_cast<float>(a.video.height()) : 16.f / 9.f;
    const float vw = std::min(size.x, size.y * aspect), vh = vw / aspect;
    const ImVec2 q0(p0.x + (size.x - vw) * 0.5f, p0.y + (size.y - vh) * 0.5f);
    const auto ps = a.player.status();
    if (a.havePic && a.rx.running()) {
        dl->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(a.video.texture())), q0, ImVec2(q0.x + vw, q0.y + vh));
    } else {
        const auto st = a.rx.stats();
        const char* t = !a.rx.running() ? "Not receiving" : st.lastFrameSec < 0 ? "Looking for a KAVL signal..." : ps.srcW ? "Buffering..." : "Waiting for a key frame...";
        ImGui::PushFont(a.big);
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(p0.x + (size.x - ts.x) * 0.5f, p0.y + (size.y - ts.y) * 0.5f), ImGui::GetColorU32(pal::grey()), t);
        ImGui::PopFont();
    }
    ImGui::InvisibleButton("##video", size);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) a.videoOnly = !a.videoOnly;
    if (hint && ImGui::IsItemHovered() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) ImGui::SetItemTooltip("Double-click or F: video only. Esc: back. F11: full screen.");
}

void drawSpectrum(App& a, ImVec2 size) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    const float specH = a.showWaterfall ? size.y * 0.42f : size.y;
    const ImVec2 s1(p0.x + size.x, p0.y + specH);
    dl->AddRectFilled(p0, s1, ImGui::GetColorU32(pal::plotBg()));
    dl->AddRect(p0, s1, ImGui::GetColorU32(pal::plotBorder()));
    for (int g = 1; g < 6; g++) dl->AddLine(ImVec2(p0.x, p0.y + specH * g / 6.f), ImVec2(s1.x, p0.y + specH * g / 6.f), ImGui::GetColorU32(pal::plotGrid()));
    if (!a.spec.empty() && a.specFs > 0) {
        const auto& vp = kavl::videoProfile(a.profile);
        const double half = kavl::occupiedHz(vp.symRate) / 2.0;
        auto xOf = [&](double hz) { return p0.x + static_cast<float>((hz / a.specFs + 0.5) * size.x); };
        dl->AddRectFilled(ImVec2(xOf(-a.specOff - half), p0.y), ImVec2(xOf(-a.specOff + half), s1.y), ImGui::GetColorU32(pal::accent(0.10f)));
        std::vector<ImVec2> line(a.spec.size());
        for (size_t i = 0; i < a.spec.size(); i++) {
            const float v = std::clamp((a.spec[i] - a.dbLo) / (a.dbHi - a.dbLo), 0.f, 1.f);
            line[i] = ImVec2(p0.x + size.x * static_cast<float>(i) / static_cast<float>(a.spec.size() - 1), s1.y - v * specH);
        }
        dl->AddPolyline(line.data(), static_cast<int>(line.size()), ImGui::GetColorU32(pal::accent()), 0, 1.2f);
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), "%.3f MHz   span %.2f MHz", a.freqMHz, a.specFs / 1e6);
        dl->AddText(ImVec2(p0.x + 6, p0.y + 4), ImGui::GetColorU32(pal::grey()), lbl);
    }
    if (a.showWaterfall) {
        if (a.wfTex) dl->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(a.wfTex)), ImVec2(p0.x, s1.y + 2), ImVec2(s1.x, p0.y + size.y));
        else dl->AddRectFilled(ImVec2(p0.x, s1.y + 2), ImVec2(s1.x, p0.y + size.y), IM_COL32(0, 0, 0, 255));
    }
    ImGui::Dummy(size);
}

// ---- receive --------------------------------------------------------------------------------------------------------------------------------------------
void radioSettings(App& a) {
    const bool run = a.rx.running();
    if (a.srcKind == 0) {
        ImGui::TextDisabled("File");
        ImGui::TextWrapped("%s", a.iqPath.empty() ? "(none)" : std::filesystem::path(a.iqPath).filename().string().c_str());
        if (run) ImGui::BeginDisabled();
        if (ImGui::Button("Open IQ file...", ImVec2(-1, 0))) a.chooser.open(1, "Choose a KAVL IQ .wav", false);
        if (run) ImGui::EndDisabled();
        return;
    }
    if (run) ImGui::BeginDisabled();
    if (a.srcKind == 1) {
        ImGui::Checkbox("Remote rtl_tcp server", &a.rtl.remote);
        if (a.rtl.remote) {
            ImGui::SetNextItemWidth(-60);
            ImGui::InputText("##host", a.rtlHost, sizeof(a.rtlHost));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputInt("##port", &a.rtl.port, 0);
        } else {
            ImGui::SetNextItemWidth(-28);
            if (a.rtlDevs.empty()) ImGui::TextDisabled("No RTL-SDR found");
            else ImGui::Combo("##rtldev", &a.rtlDev, [](void* d, int i) { return (*static_cast<std::vector<RadioDevice>*>(d))[static_cast<size_t>(i)].label.c_str(); },
                              &a.rtlDevs, static_cast<int>(a.rtlDevs.size()));
            ImGui::SameLine();
            if (ImGui::Button("R##refr")) refreshDevices(a);
            ImGui::SetItemTooltip("Look for radios again");
            if (findRtlTcp().empty()) ImGui::TextColored(pal::warnAmber(), "rtl_tcp missing: brew install librtlsdr");
        }
    } else {
        ImGui::SetNextItemWidth(-28);
        if (a.hrfDevs.empty()) ImGui::TextDisabled("No HackRF found");
        else ImGui::Combo("##hrfdev", &a.hrfDev, [](void* d, int i) { return (*static_cast<std::vector<RadioDevice>*>(d))[static_cast<size_t>(i)].label.c_str(); },
                          &a.hrfDevs, static_cast<int>(a.hrfDevs.size()));
        ImGui::SameLine();
        if (ImGui::Button("R##refh")) refreshDevices(a);
        ImGui::SetItemTooltip("Look for radios again");
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputDouble("##freq", &a.freqMHz, 0.1, 1.0, "%.4f MHz");
    if (run) ImGui::EndDisabled();
    // gains can change while running only through a restart: keep them editable when stopped
    if (run) ImGui::BeginDisabled();
    if (a.srcKind == 1) {
        bool autoGain = a.rtl.gainTenthsDb < 0;
        if (ImGui::Checkbox("Automatic gain", &autoGain)) a.rtl.gainTenthsDb = autoGain ? -1 : 300;
        if (!autoGain) {
            float g = a.rtl.gainTenthsDb / 10.f;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##rtlg", &g, 0.f, 49.6f, "Gain %.1f dB")) a.rtl.gainTenthsDb = static_cast<int>(std::lround(g * 10));
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##ppm", &a.rtl.ppm, -100, 100, "Correction %d ppm");
        ImGui::Checkbox("Bias tee", &a.rtl.biasTee);
        ImGui::SameLine();
        ImGui::Checkbox("RTL AGC", &a.rtl.agc);
    } else {
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##lna", &a.hrf.lna, 0, 40, "LNA %d dB");
        a.hrf.lna = a.hrf.lna / 8 * 8;
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##vga", &a.hrf.vga, 0, 62, "VGA %d dB");
        a.hrf.vga = a.hrf.vga / 2 * 2;
        ImGui::Checkbox("Amplifier +14 dB", &a.hrf.amp);
        ImGui::SameLine();
        ImGui::Checkbox("Antenna power", &a.hrf.antennaPower);
    }
    if (run) ImGui::EndDisabled();
}

void receivePanel(App& a, float leftW, float contentH) {
    const auto st = a.rx.stats();
    const auto ps = a.player.status();
    ImGui::BeginChild("##left", ImVec2(leftW, contentH), ImGuiChildFlags_Borders);
    heading(a, "Radio");
    const bool run = a.rx.running();
    if (run) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##src", &a.srcKind, "IQ file\0RTL-SDR (rtl_tcp)\0HackRF\0") && a.srcKind > 0) refreshDevices(a);
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##prof", &a.profile, [](void*, int i) { return kavl::videoProfile(i).name; }, nullptr, kavl::kNumVideoProfiles);
    if (run) ImGui::EndDisabled();
    radioSettings(a);
    if (!run) {
        if (ImGui::Button("Start", ImVec2(-1, 0))) startRx(a);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.46f, 0.55f, 1));
        if (ImGui::Button("Stop", ImVec2(-1, 0))) a.rx.stop();
        ImGui::PopStyleColor();
    }
    if (!a.rxError.empty()) ImGui::TextColored(pal::badRed(), "%s", a.rxError.c_str());
    ImGui::Separator();
    heading(a, "Signal");
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
    std::snprintf(lbl, sizeof(lbl), locked ? "%+.1f dB margin" : "margin -", margin);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, margin < 1 ? pal::badRed() : margin < 3 ? pal::warnAmber() : pal::okGreen());
    ImGui::ProgressBar(locked ? static_cast<float>(std::clamp(margin / 10.0, 0.0, 1.0)) : 0.f, ImVec2(-1, 0), lbl);
    ImGui::PopStyleColor();
    drawConstellation(a.rx.constellation(), std::min(leftW - 18, 200.f));
    valueRow(a, "Mode", "%s", st.mc ? st.mc->name : "-");
    valueRow(a, "Es/N0", "%.1f dB", st.esn0);
    valueRow(a, "MER", "%.1f dB", st.mer);
    valueRow(a, "Offset", "%.0f Hz", st.cfo);
    valueRow(a, "Clock", "%.1f ppm", st.ppm);
    valueRow(a, "Code words", "%ld/%ld", st.codeWordsOk, st.codeWords);
    valueRow(a, "Lost", "%ld", st.lost);
    ImGui::Separator();
    heading(a, "Programme");
    valueRow(a, "Video", ps.srcW ? "%dx%d %.0f fps" : "-", ps.srcW, ps.srcH, ps.fps);
    valueRow(a, "Video rate", "%.2f Mbit/s", ps.videoKbps / 1e3);
    valueRow(a, "Buffer", "%.1f s", ps.bufferSec);
    valueRow(a, "Sound", ps.soundDevice ? "%ld packets" : "no sound device", ps.audioPackets);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##vol", &a.volume, 0.f, 1.5f, "Volume %.2f")) a.player.setVolume(a.volume);
    if (st.droppedSec > 0.01) ImGui::TextColored(pal::badRed(), "Decoder behind: %.1f s dropped", st.droppedSec);
    if (run && st.speed > 0) ImGui::TextDisabled("Decoder %.2fx real time", st.speed);
    ImGui::EndChild();
}

// ---- transmit -------------------------------------------------------------------------------------------------------------------------------------------
void transmitPage(App& a, float h) {
    ImGui::BeginChild("##tx", ImVec2(0, h));
    const float colW = std::min(820.f, ImGui::GetContentRegionAvail().x);
    ImGui::BeginChild("##txcol", ImVec2(colW, 0));
    heading(a, "1. Programme");
    ImGui::TextDisabled("Drop a video file on the window, or");
    ImGui::SameLine();
    if (ImGui::Button("Choose a video...")) a.chooser.open(2, "Choose a video to transmit", false);
    ImGui::TextUnformatted(a.videoPath.empty() ? "(no video chosen)" : a.videoPath.c_str());
    ImGui::Spacing();
    heading(a, "2. Profile");
    const bool busy = a.tx.preparing() || a.tx.running();
    if (busy) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(260);
    ImGui::Combo("##txprof", &a.txProfile, [](void*, int i) { return kavl::videoProfile(i).name; }, nullptr, kavl::kNumVideoProfiles);
    const auto& vp = kavl::videoProfile(a.txProfile);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%dx%d at %d fps: AV1 %d kbit/s + Opus 112 kbit/s, %s, %.0f kHz wide, needs about %.1f dB", vp.width, vp.height, vp.fps,
                       kavl::videoKbps(vp), kavl::modcod(vp.modcod)->name, kavl::occupiedHz(vp.symRate) / 1e3, thresholdDb(kavl::modcod(vp.modcod)));
    ImGui::PopStyleColor();
    if (ImGui::Button("Encode for this profile") && !a.videoPath.empty()) a.tx.prepare(a.videoPath, a.txProfile);
    if (busy) ImGui::EndDisabled();
    if (a.tx.preparing() || a.tx.ready() || !a.tx.prepareMessage().empty()) {
        ImGui::ProgressBar(a.tx.progress(), ImVec2(-1, 0));
        ImGui::TextWrapped("%s", a.tx.prepareMessage().c_str());
    }
    ImGui::Spacing();
    heading(a, "3. Send");
    if (a.tx.running()) ImGui::BeginDisabled();
    ImGui::RadioButton("HackRF", &a.txDest, 0);
    ImGui::SameLine();
    ImGui::RadioButton("IQ file (.wav)", &a.txDest, 1);
    if (a.txDest == 0) {
        if (a.hrfDevs.size() > 1) {
            ImGui::SetNextItemWidth(260);
            ImGui::Combo("##txdev", &a.txHrfDev, [](void* d, int i) { return (*static_cast<std::vector<RadioDevice>*>(d))[static_cast<size_t>(i)].label.c_str(); },
                         &a.hrfDevs, static_cast<int>(a.hrfDevs.size()));
        }
        ImGui::SetNextItemWidth(170);
        ImGui::InputDouble("##txf", &a.txFreqMHz, 0.1, 1.0, "%.4f MHz");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        ImGui::SliderInt("##txg", &a.txGain, 0, 47, "TX gain %d dB");
        ImGui::SameLine();
        ImGui::Checkbox("RF amplifier (+14 dB)", &a.txAmp);
        ImGui::TextColored(pal::warnAmber(), "Transmit only where your licence and the band plan allow it (%.0f kHz wide signal).", kavl::occupiedHz(vp.symRate) / 1e3);
    } else {
        if (ImGui::Button("Save as...")) a.chooser.open(3, "Save the KAVL IQ file", true, "kavl_programme.wav");
        ImGui::SameLine();
        ImGui::TextUnformatted(a.txFilePath.empty() ? "(no file chosen)" : a.txFilePath.c_str());
    }
    if (a.tx.running()) ImGui::EndDisabled();
    const bool canStart = a.tx.ready() && a.tx.profile() == a.txProfile && (a.txDest == 0 || !a.txFilePath.empty());
    if (!a.tx.running()) {
        if (!canStart) ImGui::BeginDisabled();
        if (ImGui::Button(a.txDest == 0 ? "  Start transmitting  " : "  Write the IQ file  ")) {
            TxEngine::Dest d;
            d.toFile = a.txDest == 1;
            d.path = a.txFilePath;
            d.freqHz = a.txFreqMHz * 1e6;
            d.txGainDb = a.txGain;
            d.amp = a.txAmp;
            if (!a.hrfDevs.empty() && a.txHrfDev < static_cast<int>(a.hrfDevs.size())) d.serial = a.hrfDevs[static_cast<size_t>(a.txHrfDev)].serial;
            a.tx.start(d);
        }
        if (!canStart) ImGui::EndDisabled();
        if (a.tx.ready() && a.tx.profile() != a.txProfile) ImGui::TextColored(pal::warnAmber(), "The profile changed: encode again.");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, pal::badRed());
        if (ImGui::Button("  Stop  ")) a.tx.stop();
        ImGui::PopStyleColor();
    }
    const auto ts = a.tx.status();
    if (!ts.message.empty()) ImGui::TextWrapped("%s", ts.message.c_str());
    if (a.tx.running() || ts.frames > 0) {
        valueRow(a, "On air", "%.1f s (programme %.1f s, loops %ld)", ts.seconds, ts.programmeSec, ts.loops);
        valueRow(a, "Frames", "%ld", ts.frames);
        valueRow(a, "Sample rate", "%.3f Msps", ts.sampleRate / 1e6);
        valueRow(a, "Payload", "%.3f Mbit/s", ts.payloadMbps);
        if (ts.underruns) ImGui::TextColored(pal::warnAmber(), "Radio waited for data %ld times", ts.underruns);
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Tip: receive at the same time with an RTL-SDR on the Receive tab.");
    ImGui::EndChild();
    ImGui::EndChild();
}

// ---- windows ------------------------------------------------------------------------------------------------------------------------------------------
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
                "Separate programs the app runs, not part of it: rtl_tcp (librtlsdr, GPL 2) for RTL-SDR dongles and ffmpeg for encoding.\n\n"
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
    if (a.chooser.poll(2, picked)) a.videoPath = picked;
    if (a.chooser.poll(3, picked)) a.txFilePath = picked;
    uploadPicture(a);
    updateWaterfall(a);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("KAVL", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
                     (a.videoOnly ? 0 : ImGuiWindowFlags_MenuBar));
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
                a.tab = 1;
                a.chooser.open(2, "Choose a video to transmit", false);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Video only", "F", a.videoOnly)) a.videoOnly = !a.videoOnly;
            if (ImGui::MenuItem("Full screen", "F11", a.fullscreen)) a.fullscreen = !a.fullscreen, glfwPostEmptyEvent();
            ImGui::MenuItem("Waterfall", nullptr, &a.showWaterfall);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Radio")) {
            if (ImGui::MenuItem("Look for radios again")) refreshDevices(a);
            ImGui::TextDisabled("RTL-SDR: %zu, HackRF: %zu", a.rtlDevs.size(), a.hrfDevs.size());
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
        if (ImGui::IsKeyPressed(ImGuiKey_F) && a.tab == 0) a.videoOnly = !a.videoOnly;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) a.videoOnly = false;
        if (io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_O)) a.chooser.open(1, "Choose a KAVL IQ .wav", false);
    }
    if (a.videoOnly && a.tab == 0) {
        drawVideo(a, ImGui::GetContentRegionAvail(), false);
        ImGui::End();
        aboutWindows(a);
        return;
    }
    if (ImGui::BeginTabBar("##tabs")) {
        static bool first = true;
        const int want = a.tab;
        if (ImGui::BeginTabItem("Receive", nullptr, first && want == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
            a.tab = 0;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Transmit", nullptr, first && want == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
            a.tab = 1;
            ImGui::EndTabItem();
        }
        first = false;
        ImGui::EndTabBar();
    }
    const float h = ImGui::GetContentRegionAvail().y;
    if (a.tab == 0) {
        receivePanel(a, 290, h);
        ImGui::SameLine();
        ImGui::BeginChild("##main", ImVec2(0, h));
        const ImVec2 av = ImGui::GetContentRegionAvail();
        const float specH = std::clamp(av.y * (a.showWaterfall ? 0.28f : 0.14f), 90.f, 300.f);
        drawVideo(a, ImVec2(av.x, av.y - specH - 6), true);
        ImGui::Dummy(ImVec2(0, 2));
        drawSpectrum(a, ImVec2(av.x, specH));
        ImGui::EndChild();
    } else {
        transmitPage(a, h);
    }
    ImGui::End();
    aboutWindows(a);
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
    bool hidden = false, mute = false;
    int profile = -1, tab = -1;
    for (int i = 1; i < argc; i++) {
        const std::string k = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (k == "--file") file = val();
        else if (k == "--profile") profile = std::clamp(std::atoi(val().c_str()), 0, kavl::kNumVideoProfiles - 1);
        else if (k == "--shot") shot = val();
        else if (k == "--shot-after") shotAfter = std::atof(val().c_str());
        else if (k == "--hidden") hidden = true;
        else if (k == "--mute") mute = true;
        else if (k == "--tab") tab = val() == "tx" ? 1 : 0;
    }
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    if (hidden) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* w = glfwCreateWindow(1600, 950, "KAVL", nullptr, nullptr);
    if (!w) return 1;
    glfwMakeContextCurrent(w);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    applyTheme();
    App app;
    gApp = &app;
    app.win = w;
    if (!shot.empty() || hidden) app.cfg = Settings();   // checks start from the defaults
    else loadSettings(app);
    if (profile >= 0) app.profile = app.txProfile = profile;
    if (tab >= 0) app.tab = tab;
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
        if (gApp->tab == 1) {
            gApp->videoPath = p;
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
    const double t0 = glfwGetTime();
    while (!glfwWindowShouldClose(w)) {
        glfwPollEvents();
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
        if (!shot.empty() && glfwGetTime() - t0 > shotAfter) {
            writeShot(w, shot);
            break;
        }
        glfwSwapBuffers(w);
    }
    app.rx.stop();
    app.tx.stop();
    if (shot.empty() && !hidden) saveSettings(app);
    if (app.wfTex) glDeleteTextures(1, &app.wfTex);
    app.video.release();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(w);
    glfwTerminate();
    return 0;
}
