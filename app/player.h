// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app: plays the audio/video packets (kavl_av.h). AV1 video with dav1d into a YUV 4:2:0 picture (the GUI turns it into RGB on the GPU,
// video_gl.h); Opus sound with libopus into a ring that the audio device (miniaudio) drains on its own thread, with silence when it is empty
// (nothing here ever waits for the sound card). Video and sound run on one clock that starts 1 s after the first key frame and restarts when the
// time stamps jump (a looping file, a new transmission).
#pragma once
#include "kavl/kavl_av.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// The latest picture: 8-bit Y, U, V planes (U and V at half size), tightly packed.
struct Picture {
    std::mutex m;
    std::vector<uint8_t> y, u, v;
    int w = 0, h = 0;
    bool fresh = false;
};

class Player {
public:
    Player();
    ~Player();
    void start();
    void stop();
    void push(std::vector<kavl::av::Packet>&& packets);
    void setVolume(float v) { volume_ = v; }
    // How long the first key frame waits before playing starts (default 0.4 s). With erasure protection the packets come a whole group at a
    // time: 0.4 s plus one group. Afterwards the delay is trimmed to what the arrivals need (catch-up) and grows only when packets come late.
    void setStartBufferMs(double ms) { startBufMs_ = ms; }
    Picture& picture() { return pic_; }

    struct Status {
        bool playing = false, soundDevice = false;
        long shown = 0, late = 0, audioPackets = 0;
        int srcW = 0, srcH = 0;
        double fps = 0, bufferSec = 0, videoKbps = 0;
        double delaySec = -1;   // from the encoder of the transmitter to the screen (clock packets, kavl_av.h); -1 = not known
    };
    Status status();

private:
    using Clock = std::chrono::steady_clock;
    void videoLoop();
    void audioLoop();
    double nowPts() const;
    static void audioCallback(void* user, float* out, unsigned frames);

    Picture pic_;
    std::thread vth_, ath_;
    std::atomic<bool> quit_{false};
    std::mutex mtx_;
    std::deque<kavl::av::Packet> vq_, aq_;
    std::vector<uint8_t> opusHead_;
    uint32_t lastPts_ = 0;
    bool haveLast_ = false;
    std::deque<std::pair<uint32_t, size_t>> rateWin_;
    std::atomic<int> epoch_{0};
    std::atomic<bool> playing_{false};
    std::atomic<long long> t0ns_{0};
    std::atomic<double> basePts_{0};
    std::atomic<bool> haveClock_{false};
    std::atomic<double> startBufMs_{400.0};
    // catch-up: the least time to spare (pts - now at arrival) over a window; the window covers one protection group's arrival at least
    double minSlack_ = 1e9;
    Clock::time_point slackT0_ = Clock::now(), lastBackoff_ = Clock::now();
    void trackSlack(double slackMs);
    std::atomic<long long> clockOffMs_{0};   // the transmitter's wall clock minus the pts
    std::mutex stMtx_;
    Status st_;
    Clock::time_point fpsT0_ = Clock::now();
    long fpsN_ = 0;

    std::mutex ringMtx_;
    std::deque<float> ring_;   // 48 kHz stereo, interleaved
    std::atomic<float> volume_{1.f};
    struct Device;
    std::unique_ptr<Device> dev_;
};
