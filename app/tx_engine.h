// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app transmit engine.
//   prepare(): encodes a video file for a profile with the ffmpeg command-line tool (picture size and frame rate of the profile, AV1 at the profile's
//              video budget: SVT-AV1 preset 2, preset 5 for 4K, key frame every 4 s; Opus 112 kbit/s stereo), in the background, with progress.
//   start():   modulates the programme as a stream (frames paced to real time, pulse shaping with one frame of overlap, the programme looped
//              seamlessly) to a HackRF (libhackrf; sample rate a whole multiple of 4 x the symbol rate, at least 2 Msps) or to an IQ .wav.
// Transmitting is never refused here: the app shows a reminder about licences and band plans, the operator decides.
#pragma once
#include "kavl/kavl_av.h"
#include "kavl/kavl_phy.h"
#include "live_feed.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class TxEngine {
public:
    ~TxEngine();
    // encoding
    void prepare(const std::string& videoPath, int profile, int fecIdx = 0);   // fecIdx: erasure protection (kavl_fec.h)
    bool preparing() const { return preparing_; }
    bool ready() const { return ready_; }
    float progress() const { return progress_; }
    std::string prepareMessage();

    // transmitting
    struct Dest {
        bool toFile = false;
        std::string path;          // IQ .wav when toFile
        double freqHz = 433e6;
        int txGainDb = 20;         // HackRF TX VGA, 0..47
        bool amp = false;          // HackRF RF amplifier (+14 dB)
        std::string serial;        // HackRF serial, empty = the first one
        bool loopback = false;     // to this Mac's receiver (loopback.h) instead of a HackRF
    };
    bool start(const Dest& d);
    // Live: camera (or the test pattern, camera < 0) and microphone (-1 = none) encoded in real time for the profile while transmitting.
    bool startLive(const Dest& d, int profile, int fecIdx, int camera, int mic, int fps);
    bool live() const { return live_; }
    LiveFeed::Status liveStatus() { return feed_.status(); }
    std::string liveError() { return feed_.error(); }
    void stop();
    bool running() const { return running_; }
    struct Status {
        double seconds = 0, programmeSec = 0, sampleRate = 0, payloadMbps = 0;
        long frames = 0, underruns = 0, loops = 0;
        std::string message;
    };
    Status status();
    int profile() const { return profile_; }
    int fec() const { return fec_; }

private:
    void encodeJob(std::string path, int profile, int fecIdx);
    void modulateLoop(Dest d);

    std::thread prepTh_, txTh_, lbTh_;
    std::atomic<double> freqHz_{433e6};
    std::atomic<bool> preparing_{false}, ready_{false}, running_{false}, quit_{false};
    std::atomic<float> progress_{0};
    std::mutex msgMtx_;
    std::string prepMsg_;
    int profile_ = 4, fec_ = 0;
    std::vector<kavl::av::Packet> vids_, auds_;
    std::vector<uint8_t> opusHead_;
    double programmeMs_ = 0;

    // HackRF output ring (int8 I/Q pairs)
    std::mutex ringMtx_;
    std::deque<int8_t> ring_;
    std::mutex stMtx_;
    Status st_;
    void* dev_ = nullptr;
    LiveFeed feed_;
    std::atomic<bool> live_{false};
public:
    // called from the HackRF thread
    int fill(uint8_t* buf, int len);
};
