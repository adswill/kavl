// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app receive engine (rx_engine.h).
#include "rx_engine.h"
#include "kavl/kavl_fec.h"

#include <chrono>
#include <thread>
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
        lostBefore_ = rebuiltBefore_ = 0;
    }
    {
        std::lock_guard<std::mutex> sp(specMtx_);
        spec_.assign(kSpecBins, -120.f);
        specBuf_.clear();
        specCount_ = 0;
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
                    // the first measurement as it is (an average starting from -120 dB would climb for a dozen spectra)
                    spec_[static_cast<size_t>(b)] = specCount_ == 0 ? db : spec_[static_cast<size_t>(b)] * 0.7f + db * 0.3f;
                }
                specCount_++;
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

// The signal near where it should be: its occupied width, and how far its middle sits from where it should (power-weighted over its bins, finer
// than one bin). It is looked for within 20 kHz, because a narrow signal from radios whose crystals are 10-20 ppm off can sit beside the spot
// altogether. False while nothing stands out of the noise there.
bool RxEngine::signalBand(double& widthHz, double& shiftHz) {
    std::vector<float> s;
    double fs, off;
    {
        std::lock_guard<std::mutex> sp(specMtx_);
        if (specCount_ < 10) return false;   // the average needs a moment
        s = spec_;
        fs = fs_;
        off = offset_;
    }
    const int n = static_cast<int>(s.size());
    std::vector<float> sorted(s);
    std::nth_element(sorted.begin(), sorted.begin() + n / 5, sorted.end());
    const float floorDb = sorted[static_cast<size_t>(n / 5)];
    auto at = [&](int i) {   // 3 bins smoothed
        auto v = [&](int k) { return s[static_cast<size_t>(std::clamp(k, 0, n - 1))]; };
        return (v(i - 1) + v(i) + v(i + 1)) / 3.f;
    };
    const double cExact = n / 2 - off / fs * n;   // bin n/2 is 0 Hz
    const int c = std::clamp(static_cast<int>(std::lround(cExact)), 0, n - 1);
    const int reach = std::max(1, static_cast<int>(std::lround(20e3 / fs * n)));
    int p = -1;
    for (int d = 0; d <= reach && p < 0; d++)
        for (int k : {c - d, c + d})
            if (k >= 0 && k < n && at(k) >= floorDb + 8.f) {
                p = k;
                break;
            }
    if (p < 0) return false;
    int l = p, r = p;
    while (l > 0 && at(l - 1) > floorDb + 6.f) l--;
    while (r < n - 1 && at(r + 1) > floorDb + 6.f) r++;
    double sw = 0, sx = 0;
    for (int i = l; i <= r; i++) {
        const double w = std::pow(10.0, (s[static_cast<size_t>(i)] - floorDb) / 10.0);
        sw += w;
        sx += w * i;
    }
    widthHz = (r - l + 1) * fs / n;
    shiftHz = (sx / sw - cExact) * fs / n;
    return true;
}

double RxEngine::signalWidthHz() {
    double w = 0, sh = 0;
    return signalBand(w, sh) ? w : 0;
}

int RxEngine::detect(double fs, double offset) {
    // the distinct symbol rates of the profiles
    std::vector<double> rates;
    for (int i = 0; i < kavl::kNumVideoProfiles; i++) {
        const double r = kavl::videoProfile(i).symRate;
        if (std::find(rates.begin(), rates.end(), r) == rates.end()) rates.push_back(r);
    }
    // A header search on every rate at once is far slower than real time. The width of the signal in the spectrum names the likely rates: only
    // the two closest run, and none while nothing stands out of the noise. After 1.5 s without a header (2.2 frame times on the narrow profiles,
    // whose frames last 1.4-2.9 s) the choice is made again.
    std::vector<double> cand;
    std::vector<std::unique_ptr<kavl::Receiver>> rx;
    double candSec = 0, candLimit = 1.5;
    auto choose = [&]() {
        cand.clear();
        rx.clear();
        candSec = 0;
        candLimit = 1.5;
        const double w = signalWidthHz();
        if (w <= 0) return;
        std::vector<double> byFit(rates);
        std::sort(byFit.begin(), byFit.end(), [&](double a, double b) { return std::abs(kavl::occupiedHz(a) - w) < std::abs(kavl::occupiedHz(b) - w); });
        for (double r : byFit) {
            if (cand.size() == 2 || std::abs(kavl::occupiedHz(r) - w) > 0.3 * kavl::occupiedHz(r)) break;
            cand.push_back(r);
            candLimit = std::max(candLimit, 2.2 * kavl::frameSeconds(r));
            kavl::Receiver::Options o;
            o.symRate = r;
            o.inRate = fs;
            o.headerOnly = true;
            rx.push_back(std::make_unique<kavl::Receiver>(o));
        }
    };
    {
        std::lock_guard<std::mutex> sl(sMtx_);
        st_.detecting = true;
    }
    // the mixer that brings the signal to the middle; on the narrow profiles also by how far the spectrum shows it off (their receivers find
    // offsets of up to 0.4 symbol rates only: 4.7 kHz at 12 kHz wide)
    kavl::cf step(1, 0), ph(1, 0);
    double shift = 0;
    auto setMixer = [&](double hz) {
        const double w = 2.0 * 3.14159265358979 * hz / fs;
        step = kavl::cf(static_cast<float>(std::cos(w)), static_cast<float>(std::sin(w)));
    };
    setMixer(offset);
    for (;;) {
        std::vector<kavl::cf> chunk;
        {
            std::unique_lock<std::mutex> lk(qMtx_);
            qCv_.wait(lk, [&] { return quit_ || !q_.empty(); });
            if (quit_) return -1;
            if (retune_.exchange(false)) {   // retuned while looking: start again on the new frequency
                q_.clear();
                queued_ = 0;
                rx.clear();
                cand.clear();
                continue;
            }
            while (queued_ > static_cast<long>(0.3 * fs) && q_.size() > 1) {   // keep to the newest samples while looking
                queued_ -= static_cast<long>(q_.front().size());
                q_.pop_front();
            }
            chunk = std::move(q_.front());
            q_.pop_front();
            queued_ -= static_cast<long>(chunk.size());
        }
        if (rx.empty() || candSec > candLimit) {
            choose();
            double bw = 0, sh = 0;
            const double nextShift = !cand.empty() && kavl::occupiedHz(cand.back()) < 100e3 && signalBand(bw, sh) ? sh : 0.0;
            if (nextShift != shift) setMixer(offset - (shift = nextShift));
            if (rx.empty()) continue;
        }
        if (offset - shift != 0.0) {
            for (size_t i = 0; i < chunk.size(); i++) {
                chunk[i] *= ph;
                ph *= step;
                if ((i & 4095) == 0) ph /= std::abs(ph);
            }
        }
        candSec += static_cast<double>(chunk.size()) / fs;
        std::vector<int> found(cand.size(), -1);   // the modcod each detector saw
        std::vector<std::thread> th;
        for (size_t k = 0; k < cand.size(); k++)
            th.emplace_back([&, k] {
                for (const auto& f : rx[k]->push(chunk.data(), chunk.size()))
                    if (f.mc) found[k] = f.mc->id;
            });
        for (auto& t : th) t.join();
        for (size_t k = 0; k < cand.size(); k++) {
            if (found[k] < 0) continue;
            int best = -1;
            for (int i = 0; i < kavl::kNumVideoProfiles; i++) {
                const auto& vp = kavl::videoProfile(i);
                if (vp.symRate != cand[k]) continue;
                if (best < 0 || vp.modcod == found[k]) best = i;   // the profile with that modcod; else any with the rate
            }
            std::lock_guard<std::mutex> sl(sMtx_);
            st_.detecting = false;
            return best;
        }
    }
}

// Runs until stopped. A transmission that stops and starts again is picked up by itself: after a gap of more than 1.5 s without a frame (2.5
// frame times on the narrow profiles) the demuxer starts afresh (protection groups and frame numbers restart) and so does the player's time line;
// with Auto, twice that without a frame goes back to finding the profile (the next transmission may use another one).
void RxEngine::worker(int fixedProfile, double fs, double offset) {
    long long n0 = 0;
    const auto wall0 = std::chrono::steady_clock::now();
    for (;;) {
        const int profile = fixedProfile >= 0 ? fixedProfile : detect(fs, offset);
        if (profile < 0) return;
        {
            std::lock_guard<std::mutex> sl(sMtx_);
            st_.profile = profile;
        }
        if (!receive(profile, fixedProfile < 0, fs, offset, n0, wall0)) return;
        player_.restart();   // the next transmission starts a new time line
        std::lock_guard<std::mutex> sl(sMtx_);
        st_.profile = -1;
    }
}

bool RxEngine::receive(int profile, bool autoProfile, double fs, double offset, long long& n0, std::chrono::steady_clock::time_point wall0) {
    const kavl::VideoProfile& vp = kavl::videoProfile(profile);
    kavl::Receiver::Options o;
    o.symRate = vp.symRate;
    o.inRate = fs;
    kavl::Receiver rx(o);
    auto demux = std::make_unique<kavl::av::Demuxer>();
    // the radios put the signal at -offset: shift it back to the middle; on a narrow profile also by how far the spectrum shows it off, measured
    // again each second while no frame comes (the receiver alone finds offsets of up to 0.4 symbol rates: 4.7 kHz at 12 kHz wide)
    kavl::cf step(1, 0), ph(1, 0);
    auto setMixer = [&](double hz) {
        const double w = 2.0 * 3.14159265358979 * hz / fs;
        step = kavl::cf(static_cast<float>(std::cos(w)), static_cast<float>(std::sin(w)));
    };
    setMixer(offset);
    const bool narrow = kavl::occupiedHz(vp.symRate) < 100e3;
    double shift = 0;
    auto lastMeasure = std::chrono::steady_clock::now() - std::chrono::hours(1);
    int bufFec = -1;
    using SteadyClock = std::chrono::steady_clock;
    const auto tStart = SteadyClock::now();
    auto lastFrame = SteadyClock::time_point();   // when the last frame came (wall clock: the queue may drop samples)
    bool haveFrame = false;
    const double gapSec = std::max(1.5, 2.5 * kavl::frameSeconds(vp.symRate));   // longer than any wait between two frames
    auto sinceFrame = [&] { return std::chrono::duration<double>(SteadyClock::now() - (haveFrame ? lastFrame : tStart)).count(); };
    for (;;) {
        std::vector<kavl::cf> chunk;
        {
            std::unique_lock<std::mutex> lk(qMtx_);
            qCv_.wait(lk, [&] { return quit_ || !q_.empty(); });
            if (quit_) return false;
            if (retune_.exchange(false)) {   // retuned: what is queued is from the old frequency
                q_.clear();
                queued_ = 0;
                lk.unlock();
                lostBefore_ += demux->stats().lost;
                rebuiltBefore_ += demux->stats().recovered;
                if (autoProfile) return true;   // another signal may use another profile
                rx.reset();
                demux = std::make_unique<kavl::av::Demuxer>();
                player_.restart();
                bufFec = -1;
                haveFrame = false;
                continue;
            }
            // nothing received lately: skip a backlog rather than search stale noise (and come back late when the signal returns)
            while (sinceFrame() > gapSec && queued_ > static_cast<long>(0.3 * fs) && q_.size() > 1) {
                queued_ -= static_cast<long>(q_.front().size());
                n0 += static_cast<long long>(q_.front().size());
                q_.pop_front();
            }
            chunk = std::move(q_.front());
            q_.pop_front();
            queued_ -= static_cast<long>(chunk.size());
        }
        if (narrow && (!haveFrame || sinceFrame() > gapSec) && SteadyClock::now() - lastMeasure > std::chrono::seconds(1)) {
            lastMeasure = SteadyClock::now();
            double bw = 0, sh = 0;
            if (signalBand(bw, sh) && std::abs(sh - shift) > 0.2 * vp.symRate) {   // the receiver copes with the rest (0.4)
                setMixer(offset - (shift = sh));
                rx.reset();   // what it holds was mixed with the old shift
            }
        }
        if (offset - shift != 0.0) {
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
        if (!frames.empty()) {
            if (haveFrame && sinceFrame() > gapSec) {   // the transmission stopped and came back: a new one
                lostBefore_ += demux->stats().lost;
                rebuiltBefore_ += demux->stats().recovered;
                demux = std::make_unique<kavl::av::Demuxer>();
                player_.restart();
                bufFec = -1;
                std::lock_guard<std::mutex> sl(sMtx_);
                st_.restarts++;
            }
            lastFrame = SteadyClock::now();
            haveFrame = true;
        } else if (autoProfile && sinceFrame() > 2.0 * gapSec) {
            lostBefore_ += demux->stats().lost;
            rebuiltBefore_ += demux->stats().recovered;
            return true;   // nothing for a while (3 s on the wide profiles): find the profile again
        }
        for (const auto& f : frames) {
            if (f.hdr.fec != bufFec && f.mc) {   // packets come a protection group at a time: the player waits one group more before it starts
                bufFec = f.hdr.fec;
                const auto fp = kavl::fec::profile(bufFec, f.mc->blocks);
                // a frame comes whole every frame time: wait 1.5 of them and 0.1 s (0.23 s at 1440p, 0.42 s at 720p), plus a group with protection
                const double frameMs = kavl::frameSeconds(vp.symRate) * 1000.0;
                player_.setStartBufferMs(100.0 + 1.5 * frameMs + (bufFec > 0 ? fp.frames * frameMs : 0.0));
            }
            demux->pushFrame(f);
        }
        player_.push(demux->take());
        std::lock_guard<std::mutex> sl(sMtx_);
        st_.streamSec = t;
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
        if (wall > 1.0) st_.speed = t / wall;
        st_.lost = lostBefore_ + demux->stats().lost;
        st_.rebuilt = rebuiltBefore_ + demux->stats().recovered;
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
