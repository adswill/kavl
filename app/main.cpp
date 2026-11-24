// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL: receiver and transmitter of the KAVL narrow-band video mode.
//   Receive:  IQ file / RTL-SDR (rtl_tcp) / HackRF -> KAVL receiver -> video + sound; signal state, link margin, constellation, spectrum, waterfall.
//   Transmit: a video file -> AV1 + Opus for a profile (ffmpeg) -> KAVL signal -> HackRF or IQ .wav.
// Options: --file IQ.wav (start receiving it), --profile N, --tab rx|tx, --mute, --hidden, --shot OUT.png [--shot-after S] (a picture of the
// window after S seconds, then quit). Checks without radios: --loopback (test pattern sent to this Mac's receiver), --fec N, --report (a status
// line every second on stdout), --popout (the video window too; hidden with --hidden; --shot also saves it as OUT_popout.png), --run-for S (quit after S seconds), --tx-gap A:B[:P] (the transmitter stops at A s and starts again at B s, with
// profile P), --retune T:MHz (the receive frequency changes at T s; more than one may be given).
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
#include "spectrum_view.h"
#include "theme.h"
#include "live_feed.h"
#include "loopback.h"
#include "tx_engine.h"
#include "video_gl.h"
#include "video_popout.h"
#include "kavl/kavl_fec.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
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
    bool showRx = true, showTx = true;   // the columns
    VideoPopout popout;
    bool wantPopout = false, hidden = false;
    std::string narrowNote;   // which column the window is too narrow for
    // tuning: where it was (Cmd+Z), the last frequencies used (Recent), the undo chip after a click on the spectrum, a range note
    std::vector<double> tuneHistory, recent;   // MHz
    double chipUntil = 0, chipFromMHz = 0, freqBeforeEdit = 0, noteUntil = 0;
    std::string freqNote;
    // slow work off the drawing thread: listing cameras (ffmpeg), stopping the transmitter (waits for ffmpeg and the radio)
    std::thread camTh, txStopTh;
    std::atomic<bool> listingCams{false}, camsReady{false}, txStopping{false};
    std::mutex camMtx;
    std::vector<CaptureDevice> newCams, newMics;
    // receive
    int srcKind = 0;   // 0 IQ file, 1 RTL-SDR, 2 HackRF, 3 loopback (this Mac's transmitter)
    std::string iqPath;
    double freqMHz = 433.0;
    int profile = -1;   // -1 = Auto
    std::string rxError;
    float volume = 1.f;
    RtlSettings rtl;
    HackRfSettings hrf;
    std::vector<RadioDevice> rtlDevs, hrfDevs;
    int rtlDev = 0, hrfDev = 0;
    char rtlHost[128] = "127.0.0.1";
    // transmit
    std::string videoPath, txFilePath;
    int txProfile = 4, txDest = 0, txGain = 20, txHrfDev = 0, txFec = 0;
    int txSource = 0;   // 0 video file, 1 camera (live), 2 test pattern (live)
    int camera = 0, microphone = 0, liveFps = 0;   // liveFps: 0 = 30, 1 = 60
    std::vector<CaptureDevice> cameras, microphones;
    bool devicesListed = false;
    std::string cameraName, microphoneName;   // saved by name: the device numbers change when an iPhone or a virtual device comes and goes
    double txFreqMHz = 433.0;
    bool txAmp = false;
    // view
    bool videoOnly = false, fullscreen = false, showAbout = false, showLicences = false;
    SpectrumView specView;
    std::vector<float> spec;
    double specFs = 0, specOff = 0;
    bool specFresh = false;
    long specCount = -1;
    bool havePic = false;
    GLFWwindow* win = nullptr;
    int winX = 100, winY = 100, winW = 1600, winH = 950;
};

App* gApp = nullptr;

void loadSettings(App& a) {
    Settings& c = a.cfg;
    c.load();
    a.srcKind = std::clamp(static_cast<int>(c.num("rx.source", 0)), 0, 3);
    a.iqPath = c.str("rx.file");
    a.freqMHz = c.num("rx.freqMHz", 433.0);
    {   // Recent: "433.0000,432.5000,..."
        std::string s = c.str("rx.recent");
        for (size_t i = 0; i < s.size();) {
            const size_t j = s.find(',', i);
            const double f = std::atof(s.substr(i, j == std::string::npos ? std::string::npos : j - i).c_str());
            if (f > 0 && a.recent.size() < 8) a.recent.push_back(f);
            if (j == std::string::npos) break;
            i = j + 1;
        }
    }
    a.profile = std::clamp(static_cast<int>(c.num("rx.profile", -1)), -1, kavl::kNumVideoProfiles - 1);   // -1 = Auto
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
    a.txDest = std::clamp(static_cast<int>(c.num("tx.dest", 0)), 0, 2);
    a.txFec = std::clamp(static_cast<int>(c.num("tx.fec", 0)), 0, kavl::fec::kNumProfiles - 1);
    a.txSource = std::clamp(static_cast<int>(c.num("tx.source", 0)), 0, 2);
    a.liveFps = static_cast<int>(c.num("tx.liveFps", 0));
    a.cameraName = c.str("tx.cameraName");
    a.microphoneName = c.str("tx.microphoneName");
    a.specView.load(c);
    a.showRx = c.flag("view.showRx", true);
    a.showTx = c.flag("view.showTx", true);
}

void saveSettings(App& a) {
    Settings& c = a.cfg;
    c.set("rx.source", a.srcKind);
    c.set("rx.file", a.iqPath);
    c.set("rx.freqMHz", a.freqMHz);
    {
        std::string s;
        char t[32];
        for (double r : a.recent) {
            std::snprintf(t, sizeof(t), "%s%.4f", s.empty() ? "" : ",", r);
            s += t;
        }
        c.set("rx.recent", s);
    }
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
    c.set("tx.fec", a.txFec);
    c.set("tx.source", a.txSource);
    c.set("tx.liveFps", a.liveFps);
    if (a.devicesListed) {   // else keep what was saved
        a.cameraName = a.camera >= 0 && a.camera < static_cast<int>(a.cameras.size()) ? a.cameras[static_cast<size_t>(a.camera)].name : a.cameraName;
        a.microphoneName = a.microphone < 0 ? "none" : a.microphone < static_cast<int>(a.microphones.size()) ? a.microphones[static_cast<size_t>(a.microphone)].name : a.microphoneName;
    }
    c.set("tx.cameraName", a.cameraName);
    c.set("tx.microphoneName", a.microphoneName);
    a.specView.save(c);
    c.set("view.showRx", a.showRx);
    c.set("view.showTx", a.showTx);
    c.save();
}

// the saved device by name; else the first whose name has one of the preferred words (the Mac's own camera and microphone); else the first
int pickDevice(const std::vector<CaptureDevice>& devs, const std::string& saved, std::initializer_list<const char*> prefer) {
    for (size_t i = 0; i < devs.size(); i++)
        if (!saved.empty() && devs[i].name == saved) return static_cast<int>(i);
    for (const char* w : prefer)
        for (size_t i = 0; i < devs.size(); i++)
            if (devs[i].name.find(w) != std::string::npos) return static_cast<int>(i);
    return 0;
}

// Asking ffmpeg for the cameras takes a moment: on a thread; the lists arrive in takeDevices().
void listDevices(App& a) {
    if (a.listingCams) return;
    if (a.camTh.joinable()) a.camTh.join();
    a.listingCams = true;
    a.camTh = std::thread([&a] {
        std::vector<CaptureDevice> cams, mics;
        listCaptureDevices(cams, mics);
        std::lock_guard<std::mutex> lk(a.camMtx);
        a.newCams = std::move(cams);
        a.newMics = std::move(mics);
        a.camsReady = true;
        a.listingCams = false;
    });
}

void takeDevices(App& a) {
    if (!a.camsReady) return;
    std::lock_guard<std::mutex> lk(a.camMtx);
    a.cameras = std::move(a.newCams);
    a.microphones = std::move(a.newMics);
    a.camsReady = false;
    a.devicesListed = true;
    a.camera = pickDevice(a.cameras, a.cameraName, {"FaceTime", "Built-in"});
    a.microphone = a.microphoneName == "none" ? -1 : a.microphones.empty() ? -1 : pickDevice(a.microphones, a.microphoneName, {"MacBook", "Built-in"});
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

// A new spectrum only when the receiver measured one (about 30 a second): the waterfall's time axis no longer depends on the screen's refresh.
void updateSpectrum(App& a) {
    a.specFresh = false;
    if (!a.rx.running()) {
        a.spec.clear();
        return;
    }
    const long n = a.rx.spectrumCount();
    if (n != a.specCount && a.rx.spectrum(a.spec, a.specFs, a.specOff)) a.specFresh = true;
    a.specCount = n;
}

// what the chosen receive radio can tune (MHz)
void rxRange(const App& a, double& lo, double& hi, const char*& name) {
    if (a.srcKind == 1) lo = 24, hi = 1766, name = "The RTL-SDR";
    else lo = 1, hi = 6000, name = a.srcKind == 2 ? "The HackRF" : "KAVL";
}

// Keeps a.freqMHz inside the radio's range; says so when it had to move it.
void clampRx(App& a) {
    double lo, hi;
    const char* name;
    rxRange(a, lo, hi, name);
    const double f = std::clamp(a.freqMHz, lo, hi);
    if (f != a.freqMHz) {
        char t[128];
        std::snprintf(t, sizeof(t), "%s tunes %.0f to %.0f MHz: set to %.4f MHz.", name, lo, hi, f);
        a.freqNote = t;
        a.noteUntil = ImGui::GetTime() + 5.0;
        a.freqMHz = f;
    }
}

void rememberRecent(App& a, double mhz) {
    a.recent.erase(std::remove_if(a.recent.begin(), a.recent.end(), [&](double r) { return std::abs(r - mhz) < 0.0005; }), a.recent.end());
    a.recent.insert(a.recent.begin(), mhz);
    if (a.recent.size() > 8) a.recent.resize(8);
}

// Every retune goes through here: clamped to the radio, the old frequency kept for Cmd+Z, the radio told at once. A click or drag on the
// spectrum also shows "Back to ..." for 8 s (one stray click must not cost the signal).
enum class Tune { Typed, Plot, Key, Recent, Undo };
void setRxFrequency(App& a, double hz, Tune how) {
    const double before = a.freqMHz;
    a.freqMHz = hz / 1e6;
    clampRx(a);
    if (std::abs(a.freqMHz - before) < 1e-7) return;
    if (how != Tune::Undo) {
        a.tuneHistory.push_back(before);
        if (a.tuneHistory.size() > 32) a.tuneHistory.erase(a.tuneHistory.begin());
    }
    if (how == Tune::Plot) {
        a.chipUntil = ImGui::GetTime() + 8.0;
        a.chipFromMHz = before;
    } else {
        a.chipUntil = 0;
    }
    if (a.rx.running()) rememberRecent(a, a.freqMHz);
    if (a.rx.running() && a.rx.source()) a.rx.source()->setFrequency(a.freqMHz * 1e6);
}

void undoTune(App& a) {
    if (a.tuneHistory.empty()) return;
    const double f = a.tuneHistory.back();
    a.tuneHistory.pop_back();
    setRxFrequency(a, f * 1e6, Tune::Undo);
}

// Stopping the transmitter waits for ffmpeg and the radio (up to 2 s): on a thread, so the window keeps drawing.
void stopTxAsync(App& a) {
    if (a.txStopping) return;
    if (a.txStopTh.joinable()) a.txStopTh.join();
    a.txStopping = true;
    a.txStopTh = std::thread([&a] {
        a.tx.stop();
        a.txStopping = false;
    });
}

void waitTxStop(App& a) {
    if (a.txStopTh.joinable()) a.txStopTh.join();
}

void startRx(App& a) {
    a.rxError.clear();
    clampRx(a);
    rememberRecent(a, a.freqMHz);
    // the width only places the radio's DC spike: with Auto, that of the 400 kHz profiles
    const double width = a.profile >= 0 ? kavl::occupiedHz(kavl::videoProfile(a.profile).symRate) : 400e3;
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
    } else if (a.srcKind == 3) {
        src = makeLoopbackSource(a.freqMHz * 1e6);
    } else {
        if (a.hrfDevs.empty()) refreshDevices(a);
        HackRfSettings s = a.hrf;
        if (!a.hrfDevs.empty()) s.serial = a.hrfDevs[static_cast<size_t>(a.hrfDev)].serial;
        src = makeHackRfSource(s, a.freqMHz * 1e6, width);
    }
    if (!a.rx.start(std::move(src), a.profile)) a.rxError = a.rx.error();
    a.havePic = false;
    a.specView.clear();
}

void startTx(App& a) {
    TxEngine::Dest d;
    d.toFile = a.txDest == 1;
    d.loopback = a.txDest == 2;
    d.path = a.txFilePath;
    d.freqHz = a.txFreqMHz * 1e6;
    d.txGainDb = a.txGain;
    d.amp = a.txAmp;
    if (!a.hrfDevs.empty() && a.txHrfDev < static_cast<int>(a.hrfDevs.size())) d.serial = a.hrfDevs[static_cast<size_t>(a.txHrfDev)].serial;
    if (a.txSource == 0) {
        a.tx.start(d);
    } else {
        const int cam = a.txSource == 2 || a.cameras.empty() ? -1 : a.cameras[static_cast<size_t>(std::clamp(a.camera, 0, static_cast<int>(a.cameras.size()) - 1))].index;
        const int mic = a.txSource == 2 || a.microphone < 0 || a.microphone >= static_cast<int>(a.microphones.size())
                            ? -1
                            : a.microphones[static_cast<size_t>(a.microphone)].index;
        a.tx.startLive(d, a.txProfile, a.txFec, cam, mic, a.liveFps ? 60 : 30);
    }
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
    ImGui::SameLine(110);
    ImGui::PushFont(a.mono);
    ImGui::TextUnformatted(buf);
    ImGui::PopFont();
}

// The action buttons: primary (Start: Signal Teal, dark bold label) and on-air (Stop transmitting / recording: Transmit Red, dark bold label).
// Taller than the other controls, so the actions that start or stop something are the first thing found. A disabled primary falls back to the
// plain button, so a greyed teal never looks like an available action.
enum class Action { Primary, OnAir };
bool actionButton(App& a, const char* label, Action kind, bool enabled = true) {
    const ImVec2 size(-1, ImGui::GetFrameHeight() + 8);
    if (!enabled) {
        ImGui::BeginDisabled();
        ImGui::Button(label, size);
        ImGui::EndDisabled();
        return false;
    }
    const bool primary = kind == Action::Primary;
    ImGui::PushStyleColor(ImGuiCol_Button, primary ? pal::accent() : pal::badRed());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, primary ? pal::accentHover() : ImVec4(0.93f, 0.47f, 0.43f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, primary ? pal::accentActive() : ImVec4(0.80f, 0.31f, 0.28f, 1));
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
    const float w = ImGui::GetContentRegionAvail().x, h = onAir ? 36.f : 26.f;
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

// Send this Mac's test pattern to its own receiver: a picture within seconds, no radio needed (the quickest way to see what KAVL does).
void tryLoopback(App& a) {
    a.rx.stop();
    waitTxStop(a);
    a.tx.stop();
    a.srcKind = 3;
    a.txSource = 2;
    a.txDest = 2;
    a.txFreqMHz = a.freqMHz;
    startTx(a);
    startRx(a);
}

// What the picture area says while there is no picture: first use (what will be here, the steps, a one-click try), looking for a signal (on
// which frequency, what to check), and the short waits after a signal is found.
void pictureEmptyState(App& a, ImVec2 p0, ImVec2 size) {
    auto* dl = ImGui::GetWindowDrawList();
    const ImU32 strong = ImGui::GetColorU32(ImGuiCol_Text), muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float colW = std::min(440.f, size.x - 40.f);
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
    const auto st = a.rx.stats();
    char buf[160];
    if (a.rx.running()) {
        const bool found = st.lastFrameSec >= 0 && st.streamSec - st.lastFrameSec < 1.5;   // a frame lately, not ever
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
            text(a.big, strong, y, a.player.status().srcW ? "Starting the picture" : "Signal found");
            text(a.ui, muted, y, a.player.status().srcW ? "a moment's buffer" : "waiting for a key frame (every 2 to 4 s)");
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
    const int shown = a.profile >= 0 ? a.profile : a.rx.stats().profile;
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

// ---- receive --------------------------------------------------------------------------------------------------------------------------------------------
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
        ImGui::SetNextItemWidth(-72);
        const double shown = a.freqMHz;
        if (ImGui::InputDouble("##freq", &a.freqMHz, 0.1, 1.0, "%.4f MHz")) {
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
    const auto st = a.rx.stats();
    const auto ps = a.player.status();
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

// ---- transmit -------------------------------------------------------------------------------------------------------------------------------------------
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
        ImGui::TextWrapped("%dx%d at %d fps: AV1 %d kbit/s + Opus 112 kbit/s, %s, %.0f kHz wide, needs about %.1f dB", vp.width, vp.height,
                           a.txSource == 0 ? vp.fps : std::min(vp.fps, a.liveFps ? 60 : 30), kavl::videoKbps(vp, a.txFec), kavl::modcod(vp.modcod)->name,
                           kavl::occupiedHz(vp.symRate) / 1e3, thresholdDb(kavl::modcod(vp.modcod)));
        ImGui::PopStyleColor();
        if (a.txSource != 0 && vp.width > 2560) ImGui::TextColored(pal::warnAmber(), "Live 4K: the real-time encoder gives a softer picture than an encoded file.");
        if (a.txSource == 0) {
            if (ImGui::Button("Encode for this profile", ImVec2(-1, 0)) && !a.videoPath.empty()) a.tx.prepare(a.videoPath, a.txProfile, a.txFec);
            ImGui::SetItemTooltip("AV1 + Opus for this profile with ffmpeg (slow, best quality); once per video and profile");
        }
        if (txBusy) ImGui::EndDisabled();
        if (a.txSource == 0 && (a.tx.preparing() || a.tx.ready() || !a.tx.prepareMessage().empty())) {
            ImGui::ProgressBar(a.tx.progress(), ImVec2(-1, 0));
            ImGui::TextWrapped("%s", a.tx.prepareMessage().c_str());
        }
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
        const bool fileReady = a.tx.ready() && a.tx.profile() == a.txProfile && a.tx.fec() == a.txFec;
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
            if (a.txSource == 0 && a.tx.ready() && (a.tx.profile() != a.txProfile || a.tx.fec() != a.txFec))
                ImGui::TextColored(pal::warnAmber(), "The profile or the protection changed: encode again.");
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
    const auto ts = a.tx.status();
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

// the line at the bottom: receiver, transmitter, delay, decoder speed
void statusBar(App& a) {
    const auto st = a.rx.stats();
    const auto ps = a.player.status();
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
        const bool locked = st.lastFrameSec >= 0 && st.streamSec - st.lastFrameSec < 0.5;
        const int prof = a.profile >= 0 ? a.profile : st.profile;
        part(locked ? pal::okGreen() : pal::warnAmber(), "RX %.4f MHz  %s%s", a.freqMHz, locked ? (ps.playing ? "playing" : "locked") : "searching",
             prof >= 0 ? (std::string("  ") + kavl::videoProfile(prof).name).c_str() : "");
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
                "Separate programs the app runs, not part of it: rtl_tcp (librtlsdr, GPL 2) for RTL-SDR dongles, and FFmpeg 8.0 (LGPL 2.1, bundled; built by tools/build_ffmpeg.sh from the ffmpeg.org source) for encoding.\n\n"
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
    uploadPicture(a);
    updateSpectrum(a);
    takeDevices(a);

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
        if (ImGui::IsKeyPressed(ImGuiKey_F) && !io.KeySuper) a.videoOnly = !a.videoOnly;
        if (ImGui::IsKeyPressed(ImGuiKey_P) && !io.KeySuper) a.wantPopout = !a.popout.isOpen();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) a.videoOnly = false;
        if (io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_O)) a.chooser.open(1, "Choose a KAVL IQ .wav", false);
        if (io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_1)) a.showRx = !a.showRx;
        if (io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_2)) a.showTx = !a.showTx;
        if (io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_Z, false)) undoTune(a);
        // tuning steps on [ and ] (the arrows belong to keyboard navigation)
        const double step = io.KeyShift ? 0.05 : 0.005;
        if (!io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) setRxFrequency(a, (a.freqMHz - step) * 1e6, Tune::Key);
        if (!io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_RightBracket)) setRxFrequency(a, (a.freqMHz + step) * 1e6, Tune::Key);
    }
    if (a.videoOnly) {   // the picture alone: no columns, no spectrum
        drawVideo(a, ImGui::GetContentRegionAvail(), false);
        ImGui::End();
        aboutWindows(a);
        return;
    }
    // the station: Receive on the left, the picture and the spectrum in the middle, Transmit on the right, the status line at the bottom
    const float statusH = ImGui::GetFrameHeightWithSpacing() + 4;
    const float h = ImGui::GetContentRegionAvail().y - statusH;
    // The columns scale with the window; when the picture would get narrower than 520 px, a column that is not in use gives way (Transmit
    // first). A running column never hides: it holds its Stop. The status line says what was hidden.
    const float total = ImGui::GetContentRegionAvail().x, gap = ImGui::GetStyle().ItemSpacing.x, minMid = 520.f;
    const float leftW = std::clamp(total * 0.2f, 250.f, 300.f), rightW = std::clamp(total * 0.22f, 270.f, 330.f);
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
    const float specH = std::clamp(av.y * (a.specView.waterfall ? 0.36f : 0.2f), 140.f, 460.f);
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
}

void printReport(App& a, double t) {
    const auto st = a.rx.stats();
    const auto ps = a.player.status();
    const auto ts = a.tx.status();
    std::printf("t=%5.1f rx frames=%ld lost=%ld profile=%d play=%d shown=%ld late=%ld buf=%.2f delay=%.2f snd=%ld restarts=%ld dropped=%.1f | tx frames=%ld underruns=%ld\n", t, st.frames,
                st.lost, st.profile, ps.playing ? 1 : 0, ps.shown, ps.late, ps.bufferSec, ps.delaySec, ps.audioPackets, st.restarts, st.droppedSec, ts.frames, ts.underruns);
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
    double runFor = 0, gapStop = 0, gapStart = 0;
    int winW = 1600, winH = 950;   // --size WxH (checks at other window sizes)
    int gapProfile = -1;
    std::vector<std::pair<double, double>> retunes;   // --retune T:MHz: the receive frequency at T s   // --tx-gap A:B[:P]: the loopback transmitter stops at A s and starts again at B s (with profile P)
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
        else if (k == "--loopback") loopback = true;
        else if (k == "--fec") fecArg = std::clamp(std::atoi(val().c_str()), 0, kavl::fec::kNumProfiles - 1);
        else if (k == "--report") report = true;
        else if (k == "--popout") popoutArg = true;
        else if (k == "--size") std::sscanf(val().c_str(), "%dx%d", &winW, &winH);
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
    if (tab == 1) app.showTx = true;   // --tab tx: the transmit column on show (both are, on the combined screen)
    app.hidden = hidden;
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
        if (runFor > 0 && glfwGetTime() - t0 > runFor) break;
        for (auto& rt : retunes)
            if (rt.first > 0 && glfwGetTime() - t0 > rt.first) {
                setRxFrequency(app, rt.second * 1e6, Tune::Plot);   // as a click on the spectrum
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
    if (app.camTh.joinable()) app.camTh.join();
    if (shot.empty() && !hidden) saveSettings(app);
    app.video.release();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(w);
    glfwTerminate();
    return 0;
}
