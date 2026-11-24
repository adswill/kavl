// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app: the state of the station (App) and everything the screen is made of. The engines (app/engine) do the radio work; the files here
// draw and steer them:
//   app.cpp              settings, devices, the per-frame data, tuning, start/stop actions
//   widgets.cpp          small shared pieces: headings, value rows, lamps, action buttons, the ON AIR band, the constellation
//   picture.cpp          the received picture, its empty and waiting states, the spectrum hookup
//   receive_panel.cpp    the Receive column      transmit_panel.cpp  the Transmit column     callsign_editor.cpp  the callsign on the picture
//   station.cpp          the whole screen: menus, columns, status line, About windows (frame())
#pragma once
#include "imgui.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "callsign.h"
#include "live_feed.h"
#include "player.h"
#include "rig.h"
#include "rx_engine.h"
#include "settings.h"
#include "sources.h"
#include "spectrum_view.h"
#include "theme.h"
#include "tx_engine.h"
#include "video_gl.h"
#include "video_popout.h"

#include <atomic>
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

// A converter between a radio and the antenna (LNB, transverter, up-converter): the radio tunes the IF, the screen shows the air.
struct Converter {
    bool on = false;
    double loMHz = 0;
    bool invert = false;   // the LO above the signal: IF = LO - air, and the spectrum is mirrored (turned back in the engines)
    double radioMHz(double airMHz) const { return !on ? airMHz : invert ? loMHz - airMHz : airMHz - loMHz; }
    double airMHz(double radioMHz) const { return !on ? radioMHz : invert ? loMHz - radioMHz : radioMHz + loMHz; }
};

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
    float textScale = 1.f;    // View > Text size: 0.9, 1, 1.15, 1.3
    // the callsign on the transmitted picture, and its preview (a texture of the same drawing)
    // the status of the receiver, the player and the transmitter, read once at the start of each frame (each read locks and copies)
    struct Snapshot {
        RxEngine::Stats rx;
        Player::Status play;
        TxEngine::Status tx;
    } now;
    CallsignStyle cs;
    unsigned csTex = 0;
    std::string csTexKey;
    float csBox[4] = {0, 0, 0, 0};
    int csDrag = 0;   // 0 none, 1 moving, 2 resizing
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
    Converter rxConv, txConv;   // an LNB or transverter between each radio and the antenna (on-air frequencies everywhere on screen)
    std::string iqPath;
    double freqMHz = 433.0;
    int profile = -1;   // -1 = Auto
    std::string rxError;
    // rig control (CAT through rigctld): PTT keyed around transmitting, the rig's frequency following ours
    RigSettings rigCfg;
    RigControl rig;
    bool showRig = false;
    bool txPending = false;      // Start pressed, waiting for the rig to key (PTT) and the lead time
    double txPendingSince = 0, pttOnAt = -1;
    std::atomic<bool> txKeyed{false};   // KAVL keyed the rig for this transmission (released after the tail time)
    std::string txNote;          // why a start did not happen
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

extern App* gApp;

enum class Tune { Typed, Plot, Key, Recent, Undo };   // how a retune came (a click on the spectrum shows "Back to ...")
enum class Action { Primary, OnAir };                 // the start actions (Signal Teal) and the on-air stops (Transmit Red)

// ---- app.cpp ----
double thresholdDb(const kavl::Modcod* m);   // the Es/N0 a modcod needs
// A frame came within the last base seconds (1.5 frame times on the narrow profiles, whose frames last 1.4-2.9 s): locked
inline bool lockedNow(const RxEngine::Stats& st, double base = 0.5) {
    const double win = st.profile >= 0 ? std::max(base, 1.5 * kavl::frameSeconds(kavl::videoProfile(st.profile).symRate)) : base;
    return st.lastFrameSec >= 0 && st.streamSec - st.lastFrameSec < win;
}
float px(float v);                           // a length next to text, grown with View > Text size
std::string resourceDir();                   // the fonts: inside KAVL.app, else the source tree
void loadSettings(App& a);
void saveSettings(App& a);
int pickDevice(const std::vector<CaptureDevice>& devs, const std::string& saved, std::initializer_list<const char*> prefer);
void listDevices(App& a);
void takeDevices(App& a);
void refreshDevices(App& a);
void takeSnapshot(App& a);                   // the receiver, player and transmitter status, read once a frame
void uploadPicture(App& a);
void updateSpectrum(App& a);
void rxRange(const App& a, double& lo, double& hi, const char*& name);
double rxRadioHz(const App& a);   // what the receive radio tunes for a.freqMHz (through the converter when there is one)
double txRadioHz(const App& a);   // what the HackRF tunes for a.txFreqMHz
void converterUi(App& a, Converter& c, const char* id, bool locked, double onAirMHz);
void clampRx(App& a);
void rememberRecent(App& a, double mhz);
void setRxFrequency(App& a, double hz, Tune how);
void undoTune(App& a);
void stopTxAsync(App& a);
void waitTxStop(App& a);
void startRx(App& a);
std::string callsignImage(const App& a);
std::string callsignSig(const App& a);
void startTx(App& a);           // keys the rig first when rig PTT is on, then starts (pollTxStart)
void pollTxStart(App& a);       // every frame: starts the transmitter once the rig has keyed and the lead time passed
void rigFollow(App& a);         // the rig to the receive or transmit frequency, if it follows one
void rigWindow(App& a);
void tryLoopback(App& a);
// ---- widgets.cpp ----
void lamp(ImVec4 col);
void heading(App& a, const char* t);
void valueRow(App& a, const char* label, const char* fmt, ...);
bool actionButton(App& a, const char* label, Action kind, bool enabled = true);
void onAirBand(App& a);
bool chipButton(const char* label);
void drawConstellation(const std::vector<kavl::cf>& pts, float size);
// ---- picture.cpp ----
void pictureEmptyState(App& a, ImVec2 p0, ImVec2 size);
void drawVideo(App& a, ImVec2 size, bool hint);
void drawSpectrum(App& a, ImVec2 size);
// ---- panels ----
void radioSettings(App& a);
void receivePanel(App& a, float w, float h);
void transmitPanel(App& a, float w, float h);
void callsignEditor(App& a, bool locked);
void statusBar(App& a);
void aboutWindows(App& a);
void frame(App& a);
