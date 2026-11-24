// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app receive engine (rx_engine.h).
#include "rx_engine.h"

#include <chrono>
#include <algorithm>
#include <cmath>

bool RxEngine::start(std::unique_ptr<IqSource> src, int profile) {
    stop();
    err_.clear();
    src_ = std::move(src);
    {
        std::lock_guard<std::mutex> lk(qMtx_);
        quit_ = false;
        q_.clear();
        queued_ = 0;
    }
    {
        std::lock_guard<std::mutex> sl(sMtx_);
        st_ = Stats();
        const_.clear();
    }
    {
        std::lock_guard<std::mutex> sp(specMtx_);
        spec_.assign(kSpecBins, -120.f);
        specBuf_.clear();
    }
    fs_ = src_->sampleRate();
    offset_ = src_->offsetHz();
    if (fs_ <= 0) {
        err_ = src_->error().empty() ? "the source has no sample rate" : src_->error();
        src_.reset();
        return false;
    }
    player_.start();
    th_ = std::thread([this, profile] { worker(profile, fs_, offset_); });
    if (!src_->start([this](const kavl::cf* x, size_t n) { onSamples(x, n); })) {
        err_ = src_->error();
        {
            std::lock_guard<std::mutex> lk(qMtx_);
            quit_ = true;
        }
        qCv_.notify_all();
        th_.join();
        player_.stop();
        src_.reset();
        return false;
    }
    running_ = true;
    return true;
}

void RxEngine::stop() {
    stopRecording();
    if (src_) src_->stop();
    {
        std::lock_guard<std::mutex> lk(qMtx_);
        quit_ = true;
    }
    qCv_.notify_all();
    if (th_.joinable()) th_.join();
    player_.stop();
    src_.reset();
    running_ = false;
}

void RxEngine::onSamples(const kavl::cf* x, size_t n) {
    // spectrum: 1024-point FFTs, about 30 per second, averaged
    {
        std::lock_guard<std::mutex> sp(specMtx_);
        const double every = std::max(1.0, fs_ / 30.0);
        for (size_t i = 0; i < n; i++) {
            if (specBuf_.size() < static_cast<size_t>(kSpecBins)) {
                if (specFill_ <= 0) specBuf_.push_back(x[i]);
                else specFill_ -= 1;
            }
            if (specBuf_.size() == static_cast<size_t>(kSpecBins)) {
                std::vector<kavl::cf> f(specBuf_);
                for (int k = 0; k < kSpecBins; k++) f[static_cast<size_t>(k)] *= static_cast<float>(0.5 - 0.5 * std::cos(2 * 3.14159265358979 * k / kSpecBins));
                kavl::fft(f.data(), kSpecBins, false);
                for (int k = 0; k < kSpecBins; k++) {
                    const int b = (k + kSpecBins / 2) % kSpecBins;
                    const float db = 10.f * std::log10(std::norm(f[static_cast<size_t>(k)]) / kSpecBins + 1e-20f);
                    spec_[static_cast<size_t>(b)] = spec_[static_cast<size_t>(b)] * 0.7f + db * 0.3f;
                }
                specBuf_.clear();
                specFill_ = every - kSpecBins;
            }
        }
    }
    std::vector<kavl::cf> v(x, x + n);
    {
        std::lock_guard<std::mutex> lk(qMtx_);
        queued_ += static_cast<long>(n);
        q_.push_back(std::move(v));
        while (queued_ > static_cast<long>(2.0 * fs_) && q_.size() > 1) {
            queued_ -= static_cast<long>(q_.front().size());
            std::lock_guard<std::mutex> sl(sMtx_);
            st_.droppedSec += static_cast<double>(q_.front().size()) / fs_;
            q_.pop_front();
        }
    }
    qCv_.notify_one();
}

void RxEngine::worker(int profile, double fs, double offset) {
    const kavl::VideoProfile& vp = kavl::videoProfile(profile);
    kavl::Receiver::Options o;
    o.symRate = vp.symRate;
    o.inRate = fs;
    kavl::Receiver rx(o);
    kavl::av::Demuxer demux;
    // the radios put the signal at -offset: shift it back to the middle
    const double w = 2.0 * 3.14159265358979 * offset / fs;
    const kavl::cf step(static_cast<float>(std::cos(w)), static_cast<float>(std::sin(w)));
    kavl::cf ph(1, 0);
    long long n0 = 0;
    const auto wall0 = std::chrono::steady_clock::now();
    for (;;) {
        std::vector<kavl::cf> chunk;
        {
            std::unique_lock<std::mutex> lk(qMtx_);
            qCv_.wait(lk, [&] { return quit_ || !q_.empty(); });
            if (quit_) return;
            chunk = std::move(q_.front());
            q_.pop_front();
            queued_ -= static_cast<long>(chunk.size());
        }
        if (offset != 0.0) {
            for (size_t i = 0; i < chunk.size(); i++) {
                chunk[i] *= ph;
                ph *= step;
                if ((i & 4095) == 0) ph /= std::abs(ph);
            }
        }
        n0 += static_cast<long long>(chunk.size());
        const double t = static_cast<double>(n0) / fs;
        {
            std::lock_guard<std::mutex> rl(recMtx_);
            if (rec_) {
                std::vector<int16_t> s(chunk.size() * 2);
                for (size_t i = 0; i < chunk.size(); i++) {
                    s[2 * i] = static_cast<int16_t>(std::lround(std::clamp(chunk[i].real() * 32767.f, -32768.f, 32767.f)));
                    s[2 * i + 1] = static_cast<int16_t>(std::lround(std::clamp(chunk[i].imag() * 32767.f, -32768.f, 32767.f)));
                }
                std::fwrite(s.data(), 2, s.size(), rec_);
                recSamples_ += static_cast<long long>(chunk.size());
            }
        }
        const auto frames = rx.push(chunk.data(), chunk.size());
        for (const auto& f : frames) demux.pushFrame(f);
        player_.push(demux.take());
        std::lock_guard<std::mutex> sl(sMtx_);
        st_.streamSec = t;
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
        if (wall > 1.0) st_.speed = t / wall;
        st_.lost = demux.stats().lost;
        st_.rebuilt = demux.stats().recovered;
        for (const auto& f : frames) {
            int ok = 0;
            for (const auto& b : f.blocks) ok += b.ok ? 1 : 0;
            st_.frames++;
            st_.codeWords += static_cast<long>(f.blocks.size());
            st_.codeWordsOk += ok;
            st_.lastOk = ok;
            st_.lastBlocks = static_cast<int>(f.blocks.size());
            st_.esn0 = f.esn0Db;
            st_.mer = f.merDb;
            st_.cfo = f.cfoHz;
            st_.ppm = f.clockPpm;
            st_.mc = f.mc;
            st_.fec = f.hdr.fec;
            st_.lastFrameSec = t;
            const_ = f.dataSyms;
        }
    }
}

RxEngine::Stats RxEngine::stats() {
    std::lock_guard<std::mutex> sl(sMtx_);
    return st_;
}

std::vector<kavl::cf> RxEngine::constellation() {
    std::lock_guard<std::mutex> sl(sMtx_);
    return const_;
}

bool RxEngine::spectrum(std::vector<float>& db, double& fs, double& offsetHz) {
    std::lock_guard<std::mutex> sp(specMtx_);
    if (spec_.empty()) return false;
    db = spec_;
    fs = fs_;
    offsetHz = offset_;
    return true;
}

namespace {
void put32(FILE* f, uint32_t v) {
    const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
    std::fwrite(b, 1, 4, f);
}
void put16(FILE* f, uint16_t v) {
    const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    std::fwrite(b, 1, 2, f);
}
void wavHeader(FILE* f, uint32_t rate, uint32_t dataBytes) {
    std::fseek(f, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, f);
    put32(f, 36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, rate);
    put32(f, rate * 4);
    put16(f, 4);
    put16(f, 16);
    std::fwrite("data", 1, 4, f);
    put32(f, dataBytes);
}
}  // namespace

bool RxEngine::startRecording(const std::string& path) {
    std::lock_guard<std::mutex> rl(recMtx_);
    if (rec_ || fs_ <= 0) return false;
    rec_ = std::fopen(path.c_str(), "wb");
    if (!rec_) return false;
    wavHeader(rec_, static_cast<uint32_t>(std::lround(fs_)), 0);
    recPath_ = path;
    recSamples_ = 0;
    return true;
}

void RxEngine::stopRecording() {
    std::lock_guard<std::mutex> rl(recMtx_);
    if (!rec_) return;
    const long long n = recSamples_;
    wavHeader(rec_, static_cast<uint32_t>(std::lround(fs_)), static_cast<uint32_t>(std::min<long long>(n * 4, 0xFFFFFFF0LL)));
    std::fclose(rec_);
    rec_ = nullptr;
}
