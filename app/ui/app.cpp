// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app state and actions (app.h).
#include "app.h"
#include <OpenGL/gl3.h>
#include "loopback.h"
#include <mach-o/dyld.h>
#include "kavl/kavl_fec.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

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

// A layout length that sits next to text grows with the text size (View > Text size).
float px(float v) { return v * ImGui::GetStyle().FontScaleMain; }

// The macOS file dialog (osascript), on a thread; the result is picked up by poll().

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
    a.textScale = std::clamp(static_cast<float>(c.num("view.textScale", 1.0)), 0.9f, 1.3f);
    a.cs.on = c.flag("tx.cs.on", false);
    a.cs.text = c.str("tx.cs.text");
    a.cs.font = c.str("tx.cs.font", resourceDir() + "/Inter-SemiBold.ttf");
    a.cs.x = static_cast<float>(c.num("tx.cs.x", 0.70));
    a.cs.y = static_cast<float>(c.num("tx.cs.y", 0.86));
    a.cs.height = std::clamp(static_cast<float>(c.num("tx.cs.h", 0.06)), 0.02f, 0.3f);
    for (int i = 0; i < 4; i++) a.cs.rgba[i] = static_cast<float>(c.num("tx.cs.c" + std::to_string(i), 1.0));
    a.cs.outline = c.flag("tx.cs.outline", true);
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
    c.set("view.textScale", static_cast<double>(a.textScale));
    c.set("tx.cs.on", a.cs.on);
    c.set("tx.cs.text", a.cs.text);
    c.set("tx.cs.font", a.cs.font);
    c.set("tx.cs.x", static_cast<double>(a.cs.x));
    c.set("tx.cs.y", static_cast<double>(a.cs.y));
    c.set("tx.cs.h", static_cast<double>(a.cs.height));
    for (int i = 0; i < 4; i++) c.set("tx.cs.c" + std::to_string(i), static_cast<double>(a.cs.rgba[i]));
    c.set("tx.cs.outline", a.cs.outline);
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

void takeSnapshot(App& a) {
    a.now.rx = a.rx.stats();
    a.now.play = a.player.status();
    a.now.tx = a.tx.status();
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
    if (a.rx.running() && a.rx.source()) {
        a.rx.source()->setFrequency(a.freqMHz * 1e6);
        a.rx.retuned();
    }
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

// the callsign image for the current profile, or empty when it is off
std::string callsignImage(const App& a) {
    if (!a.cs.on || a.cs.text.empty()) return "";
    const auto& vp = kavl::videoProfile(a.txProfile);
    return writeCallsignImage(a.cs, vp.width, vp.height);
}
std::string callsignSig(const App& a) { return a.cs.on && !a.cs.text.empty() ? a.cs.signature() : std::string(); }

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
        a.tx.start(d);   // the callsign is in the encoded file already (Encode for this profile)
    } else {
        const int cam = a.txSource == 2 || a.cameras.empty() ? -1 : a.cameras[static_cast<size_t>(std::clamp(a.camera, 0, static_cast<int>(a.cameras.size()) - 1))].index;
        const int mic = a.txSource == 2 || a.microphone < 0 || a.microphone >= static_cast<int>(a.microphones.size())
                            ? -1
                            : a.microphones[static_cast<size_t>(a.microphone)].index;
        a.tx.startLive(d, a.txProfile, a.txFec, cam, mic, a.liveFps ? 60 : 30, callsignImage(a));
    }
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
