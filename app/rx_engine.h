// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app receive engine: source -> (frequency shift) -> KAVL receiver -> demuxer -> player, on a worker thread fed through a queue (2 s at most;
// older samples are dropped and counted, the source is never held up). Keeps the status, the last constellation and a running spectrum of the input.
#pragma once
#include "player.h"
#include "sources.h"
#include "kavl/kavl_av.h"
#include "kavl/kavl_rx.h"

#include <condition_variable>
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
    bool start(std::unique_ptr<IqSource> src, int profile);
    void stop();
    bool running() const { return running_; }
    IqSource* source() { return src_.get(); }
    const std::string& error() const { return err_; }

    struct Stats {
        long frames = 0, codeWords = 0, codeWordsOk = 0, lost = 0;
        double esn0 = 0, mer = 0, cfo = 0, ppm = 0;
        double streamSec = 0, lastFrameSec = -1, droppedSec = 0, speed = 0;
        const kavl::Modcod* mc = nullptr;
        int lastOk = 0, lastBlocks = 0;
    };
    Stats stats();
    std::vector<kavl::cf> constellation();
    // Power spectrum of the input (dB, kSpecBins bins from -fs/2 to +fs/2), its sample rate and the offset of the signal.
    static constexpr int kSpecBins = 1024;
    bool spectrum(std::vector<float>& db, double& fs, double& offsetHz);

private:
    void onSamples(const kavl::cf* x, size_t n);
    void worker(int profile, double fs, double offset);

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
    std::vector<kavl::cf> specBuf_;
    double specFill_ = 0;
};
