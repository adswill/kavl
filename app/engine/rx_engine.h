// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app receive engine: source -> (frequency shift) -> KAVL receiver -> demuxer -> player, on a worker thread fed through a queue (2 s at most;
// older samples are dropped and counted, the source is never held up). Keeps the status, the last constellation and a running spectrum of the input.
#pragma once
#include "player.h"
#include "sources.h"
#include "kavl/kavl_av.h"
#include "kavl/kavl_rx.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RxEngine {
public:
    explicit RxEngine(Player& player) : player_(player) {}
    ~RxEngine() { stop(); }
    // profile -1: find it (one header-only detector per symbol rate on the first seconds, then the profile with that rate and modcod)
    bool start(std::unique_ptr<IqSource> src, int profile);
    void stop();
    bool running() const { return running_; }
    // The radio was retuned: the receiver starts afresh (no stale tracking or samples) and, with Auto, finds the profile again.
    void retuned() { retune_ = true; }
    IqSource* source() { return src_.get(); }
    const std::string& error() const { return err_; }

    struct Stats {
        long frames = 0, codeWords = 0, codeWordsOk = 0, lost = 0, rebuilt = 0;
        int fec = 0;
        int profile = -1;          // the profile in use (-1 while still looking)
        bool detecting = false;
        double esn0 = 0, mer = 0, cfo = 0, ppm = 0;
        double streamSec = 0, lastFrameSec = -1, droppedSec = 0, speed = 0;
        const kavl::Modcod* mc = nullptr;
        int lastOk = 0, lastBlocks = 0;
        long restarts = 0;         // transmissions that stopped and came back
    };
    Stats stats();
    std::vector<kavl::cf> constellation();
    // Power spectrum of the input (dB, kSpecBins bins from -fs/2 to +fs/2), its sample rate and the offset of the signal.
    static constexpr int kSpecBins = 1024;
    bool spectrum(std::vector<float>& db, double& fs, double& offsetHz);
    long spectrumCount() {   // how many spectra so far: a new value means a new measurement
        std::lock_guard<std::mutex> sp(specMtx_);
        return specCount_;
    }
    // Recording what the receiver gets (after the shift back to the middle): a 16-bit IQ .wav at the source's rate.
    bool startRecording(const std::string& path);
    void stopRecording();
    bool recording() const { return rec_ != nullptr; }
    double recordedSec() const { return recSamples_ / (fs_ > 0 ? fs_ : 1.0); }
    const std::string& recordingPath() const { return recPath_; }

private:
    void onSamples(const kavl::cf* x, size_t n);
    void worker(int fixedProfile, double fs, double offset);
    // one profile until stopped (false) or, with autoProfile, until nothing came for 3 s (true)
    bool receive(int profile, bool autoProfile, double fs, double offset, long long& n0, std::chrono::steady_clock::time_point wall0);
    long lostBefore_ = 0, rebuiltBefore_ = 0;   // of the demuxers before the current one
    std::atomic<bool> retune_{false};
    int detect(double fs, double offset);   // the profile of the signal, -1 when stopped first
    double signalWidthHz();                 // the width of what stands out of the noise where the signal should be (spectrum), 0 if nothing

    Player& player_;
    std::unique_ptr<IqSource> src_;
    std::string err_;
    bool running_ = false;
    std::thread th_;
    std::mutex qMtx_, sMtx_, specMtx_;
    std::condition_variable qCv_;
    std::deque<std::vector<kavl::cf>> q_;
    long queued_ = 0;
    bool quit_ = false;
    double fs_ = 0, offset_ = 0;
    Stats st_;
    std::vector<kavl::cf> const_;
    std::vector<float> spec_;
    std::mutex recMtx_;
    FILE* rec_ = nullptr;
    std::string recPath_;
    std::atomic<long long> recSamples_{0};
    std::vector<kavl::cf> specBuf_;
    double specFill_ = 0;
    long specCount_ = 0;
};
