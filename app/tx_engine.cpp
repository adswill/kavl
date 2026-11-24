// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app transmit engine (tx_engine.h).
#include "tx_engine.h"
#include "kavl/kavl_media.h"
#include "kavl/kavl_resampler.h"
#include "loopback.h"

#include <libhackrf/hackrf.h>

#include <chrono>
#include <deque>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sys/wait.h>

namespace {

std::string shellQuote(const std::string& s) {
    std::string o = "'";
    for (char c : s) o += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return o + "'";
}

double probeDuration(const std::string& path) {
    const std::string cmd = shellQuote(findTool("ffprobe")) + " -v error -show_entries format=duration -of default=nw=1:nk=1 " + shellQuote(path);
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return 0;
    char buf[64] = {};
    const bool ok = std::fgets(buf, sizeof(buf), p) != nullptr;
    pclose(p);
    return ok ? std::atof(buf) : 0;
}

// the output sample rate: 4 x the symbol rate for a file, 2.4 Msps for the loopback, else a whole multiple of it of at least 2 Msps (HackRF)
double txRate(const TxEngine::Dest& d, double symRate) {
    const double r4 = kavl::kSps * symRate;
    if (d.toFile) return r4;
    if (d.loopback) return LoopbackBus::kRate;
    return r4 * std::max(1, static_cast<int>(std::ceil(2.0e6 / r4)));
}

}  // namespace

TxEngine::~TxEngine() {
    stop();
    quit_ = true;
    if (prepTh_.joinable()) prepTh_.join();
}

std::string TxEngine::prepareMessage() {
    std::lock_guard<std::mutex> lk(msgMtx_);
    return prepMsg_;
}

void TxEngine::prepare(const std::string& videoPath, int profile, int fecIdx) {
    if (preparing_ || running_) return;
    if (prepTh_.joinable()) prepTh_.join();
    ready_ = false;
    progress_ = 0;
    preparing_ = true;
    profile_ = profile;
    fec_ = fecIdx;
    prepTh_ = std::thread([this, videoPath, profile, fecIdx] { encodeJob(videoPath, profile, fecIdx); });
}

void TxEngine::encodeJob(std::string path, int profile, int fecIdx) {
    auto say = [&](const std::string& m) {
        std::lock_guard<std::mutex> lk(msgMtx_);
        prepMsg_ = m;
    };
    const kavl::VideoProfile& vp = kavl::videoProfile(profile);
    const int kbps = kavl::videoKbps(vp, fecIdx);
    const std::string dir = (std::filesystem::temp_directory_path() / "kavl").string();
    std::filesystem::create_directories(dir);
    const std::string mkv = dir + "/programme.mkv", ivf = dir + "/programme.ivf", ogg = dir + "/programme.ogg";
    const double dur = probeDuration(path);
    const int preset = vp.width > 2560 ? 5 : 2;
    char vf[256];
    std::snprintf(vf, sizeof(vf), "scale=%d:%d:force_original_aspect_ratio=decrease:flags=lanczos,pad=%d:%d:(ow-iw)/2:(oh-ih)/2,fps=%d,format=yuv420p", vp.width,
                  vp.height, vp.width, vp.height, vp.fps);
    const std::string ff = shellQuote(findTool("ffmpeg"));
    const std::string cmd = ff + " -hide_banner -nostats -loglevel error -y -i " + shellQuote(path) + " -vf " + shellQuote(vf) + " -c:v libsvtav1 -preset " +
                            std::to_string(preset) + " -b:v " + std::to_string(kbps) + "k -bufsize " + std::to_string(kbps * 3 / 2) + "k -g " +
                            std::to_string(vp.fps * 4) + " -svtav1-params lp=6:rc=1:tune=0 -c:a libopus -b:a 112k -ac 2 -ar 48000 -progress pipe:1 " +
                            shellQuote(mkv) + " 2>&1";
    say("Encoding " + std::filesystem::path(path).filename().string() + " for " + vp.name + " (AV1 " + std::to_string(kbps) + " kbit/s)...");
    FILE* p = popen(cmd.c_str(), "r");
    std::string errors;
    if (p) {
        char line[512];
        while (std::fgets(line, sizeof(line), p)) {
            if (quit_) break;
            if (!std::strncmp(line, "out_time_us=", 12) && dur > 0) progress_ = std::min(0.99f, static_cast<float>(std::atof(line + 12) / 1e6 / dur));
            else if (std::strncmp(line, "frame=", 6) && std::strncmp(line, "fps=", 4) && std::strchr(line, '=') == nullptr) errors += line;
        }
        const int rc = pclose(p);
        if (rc != 0) {
            say("Encoding failed: " + (errors.empty() ? std::string("ffmpeg returned an error") : errors.substr(0, 300)));
            preparing_ = false;
            return;
        }
    } else {
        say("Cannot run ffmpeg (it comes inside KAVL.app; for a build from source run tools/build_ffmpeg.sh, or brew install ffmpeg)");
        preparing_ = false;
        return;
    }
    const std::string split1 = ff + " -hide_banner -loglevel error -y -i " + shellQuote(mkv) + " -map 0:v -c copy -f ivf " + shellQuote(ivf);
    const std::string split2 = ff + " -hide_banner -loglevel error -y -i " + shellQuote(mkv) + " -map 0:a? -c copy -f ogg " + shellQuote(ogg);
    std::system(split1.c_str());
    std::system(split2.c_str());
    vids_.clear();
    auds_.clear();
    opusHead_.clear();
    if (!kavl::media::readIvf(ivf, vids_) || vids_.empty()) {
        say("The encoded video could not be read");
        preparing_ = false;
        return;
    }
    kavl::media::readOggOpus(ogg, opusHead_, auds_);   // no sound is fine
    programmeMs_ = vids_.back().ptsMs + 1000.0 / vp.fps;
    double vBytes = 0;
    for (const auto& v : vids_) vBytes += static_cast<double>(v.data.size());
    char m[256];
    std::snprintf(m, sizeof(m), "Ready: %s, %.1f s, video %.2f Mbit/s, %s", vp.name, programmeMs_ / 1000.0, vBytes * 8 / (programmeMs_ / 1000.0) / 1e6,
                  auds_.empty() ? "no sound" : "Opus stereo 112 kbit/s");
    say(m);
    progress_ = 1;
    ready_ = true;
    preparing_ = false;
}

int TxEngine::fill(uint8_t* buf, int len) {
    std::lock_guard<std::mutex> lk(ringMtx_);
    int i = 0;
    for (; i < len && !ring_.empty(); i++) {
        buf[i] = static_cast<uint8_t>(ring_.front());
        ring_.pop_front();
    }
    if (i < len) {
        std::memset(buf + i, 0, static_cast<size_t>(len - i));
        std::lock_guard<std::mutex> sl(stMtx_);
        if (running_) st_.underruns++;
    }
    return 0;
}

bool TxEngine::startLive(const Dest& d, int profile, int fecIdx, int camera, int mic, int fps) {
    stop();
    profile_ = profile;
    fec_ = fecIdx;
    const kavl::VideoProfile& vp = kavl::videoProfile(profile);
    // real-time AV1 is less efficient than the slow file encodes: the same budget, a softer picture
    if (!feed_.start(camera, mic, vp.width, vp.height, std::min(vp.fps, fps), kavl::videoKbps(vp, fecIdx))) {
        std::lock_guard<std::mutex> sl(stMtx_);
        st_.message = feed_.error();
        return false;
    }
    live_ = true;
    programmeMs_ = 0;
    opusHead_.clear();
    if (!start(d)) {
        feed_.stop();
        live_ = false;
        return false;
    }
    return true;
}

bool TxEngine::start(const Dest& d) {
    if (!live_) stop();
    if (!ready_ && !live_) return false;
    quit_ = false;
    {
        std::lock_guard<std::mutex> sl(stMtx_);
        st_ = Status();
    }
    {
        std::lock_guard<std::mutex> lk(ringMtx_);
        ring_.clear();
    }
    const kavl::VideoProfile& vp = kavl::videoProfile(profile_);
    const double rate = txRate(d, vp.symRate);
    freqHz_ = d.freqHz;
    if (!d.toFile && !d.loopback) {
        hackrf_device* dev = nullptr;
        if (hackrf_init() != HACKRF_SUCCESS || (d.serial.empty() ? hackrf_open(&dev) : hackrf_open_by_serial(d.serial.c_str(), &dev)) != HACKRF_SUCCESS) {
            std::lock_guard<std::mutex> sl(stMtx_);
            st_.message = "No HackRF found (or it is in use)";
            return false;
        }
        hackrf_set_sample_rate(dev, rate);
        hackrf_set_baseband_filter_bandwidth(dev, hackrf_compute_baseband_filter_bw(static_cast<uint32_t>(std::max(1.75e6, kavl::occupiedHz(vp.symRate) * 1.5))));
        hackrf_set_freq(dev, static_cast<uint64_t>(d.freqHz));
        hackrf_set_txvga_gain(dev, static_cast<uint32_t>(std::clamp(d.txGainDb, 0, 47)));
        hackrf_set_amp_enable(dev, d.amp ? 1 : 0);
        dev_ = dev;
    }
    {
        std::lock_guard<std::mutex> sl(stMtx_);
        st_.sampleRate = rate;
        st_.payloadMbps = kavl::netBitRate(*kavl::modcod(vp.modcod), vp.symRate) / 1e6;
        st_.programmeSec = programmeMs_ / 1000.0;
        st_.message = std::string(live_ ? "Live: " : "") + (d.toFile ? "writing " + d.path : "transmitting");
    }
    running_ = true;
    txTh_ = std::thread([this, d] { modulateLoop(d); });
    if (d.loopback) {   // drain the ring in real time, as a HackRF would, into this Mac's receiver
        lbTh_ = std::thread([this, rate] {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            const int len = static_cast<int>(rate / 100.0) * 2;   // 10 ms of int8 I/Q
            std::vector<uint8_t> b(static_cast<size_t>(len));
            std::vector<kavl::cf> x(static_cast<size_t>(len / 2));
            auto next = std::chrono::steady_clock::now();
            while (!quit_) {
                fill(b.data(), len);
                for (size_t i = 0; i < x.size(); i++) x[i] = kavl::cf(static_cast<int8_t>(b[2 * i]) / 128.f, static_cast<int8_t>(b[2 * i + 1]) / 128.f);
                LoopbackBus::get().push(x.data(), x.size(), freqHz_.load());
                next += std::chrono::milliseconds(10);
                std::this_thread::sleep_until(next);
            }
        });
    }
    if (dev_) {
        // let the modulator fill a little first, then start the radio
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (hackrf_start_tx(static_cast<hackrf_device*>(dev_), [](hackrf_transfer* t) { return static_cast<TxEngine*>(t->tx_ctx)->fill(t->buffer, t->valid_length); },
                            this) != HACKRF_SUCCESS) {
            stop();
            std::lock_guard<std::mutex> sl(stMtx_);
            st_.message = "The HackRF did not start transmitting";
            return false;
        }
    }
    return true;
}

void TxEngine::stop() {
    quit_ = true;
    if (txTh_.joinable()) txTh_.join();
    if (lbTh_.joinable()) lbTh_.join();
    feed_.stop();
    live_ = false;
    if (dev_) {
        hackrf_stop_tx(static_cast<hackrf_device*>(dev_));
        hackrf_close(static_cast<hackrf_device*>(dev_));
        hackrf_exit();
        dev_ = nullptr;
    }
    running_ = false;
    quit_ = false;
}

TxEngine::Status TxEngine::status() {
    std::lock_guard<std::mutex> sl(stMtx_);
    return st_;
}

void TxEngine::modulateLoop(Dest d) {
    const kavl::VideoProfile& vp = kavl::videoProfile(profile_);
    const kavl::Modcod& mc = *kavl::modcod(vp.modcod);
    const double r4 = kavl::kSps * vp.symRate;
    const double rate = txRate(d, vp.symRate);
    const bool resample = std::abs(rate - r4) > 0.5;
    kavl::Resampler up;
    if (resample) up.configure(r4, rate);
    FILE* wav = nullptr;
    long long wavSamples = 0;
    if (d.toFile) {
        wav = std::fopen(d.path.c_str(), "wb");
        if (!wav) {
            std::lock_guard<std::mutex> sl(stMtx_);
            st_.message = "Cannot write " + d.path;
            running_ = false;
            return;
        }
        std::vector<uint8_t> hdr(44, 0);
        std::fwrite(hdr.data(), 1, hdr.size(), wav);   // written properly at the end
    }
    const double frameMs = kavl::frameSeconds(vp.symRate) * 1000.0, leadMs = 3000.0;   // 3 s ahead: a protection group must arrive whole
    const int H = kavl::kRrcSpan / 2 + 2;   // symbols of overlap on each side for the pulse shaping
    kavl::av::Muxer mux(mc, fec_);
    kavl::Transmitter tx(mc.id);
    std::vector<kavl::cf> prev(static_cast<size_t>(H), kavl::cf(0, 0)), cur, next;
    size_t vi = 0, ai = 0;
    double base = 0;   // time line offset of the current loop of the programme
    uint32_t nextCfg = 0;
    long n = 0;
    int seq = 0;
    long loops = 0;
    int tailFrames = 0;
    std::deque<kavl::av::Packet> liveV, liveA;
    auto makeFrame = [&]() {
        if (live_) {   // everything that arrived goes out at once, in time order; padding when nothing is waiting
            std::vector<kavl::av::Packet> v, a;
            feed_.take(v, a, opusHead_);
            for (auto& p : v) liveV.push_back(std::move(p));
            for (auto& p : a) liveA.push_back(std::move(p));
            while (!liveV.empty() || !liveA.empty()) {
                const bool tv = liveA.empty() || (!liveV.empty() && liveV.front().ptsMs <= liveA.front().ptsMs);
                kavl::av::Packet p = std::move(tv ? liveV.front() : liveA.front());
                (tv ? liveV : liveA).pop_front();
                if (!opusHead_.empty() && p.ptsMs >= nextCfg) {
                    kavl::av::Packet c;
                    c.type = kavl::av::kAudioConfig;
                    c.ptsMs = p.ptsMs;
                    c.data = opusHead_;
                    mux.push(c);
                    nextCfg = p.ptsMs + 1000;
                }
                mux.push(p);
            }
            const auto pay = mux.takeFrame(!mux.frameReady());
            kavl::Header h;
            h.modcod = mc.id;
            h.fec = fec_;
            h.seq = seq++ & 0xFFFF;
            n++;
            return kavl::frameSymbols(mc, h, pay.data());
        }
        const double due = n * frameMs + leadMs;
        while (!mux.frameReady()) {
            if (vi >= vids_.size() && ai >= auds_.size()) {   // loop: the programme starts again on a continuing time line (a file holds it once)
                if (d.toFile) break;
                vi = ai = 0;
                base += programmeMs_;
                loops++;
            }
            const bool takeVideo = ai >= auds_.size() || (vi < vids_.size() && vids_[vi].ptsMs <= auds_[ai].ptsMs);
            kavl::av::Packet p = takeVideo ? vids_[vi] : auds_[ai];
            p.ptsMs = static_cast<uint32_t>(p.ptsMs + base);
            if (p.ptsMs > due) break;
            if (!opusHead_.empty() && p.ptsMs >= nextCfg) {
                kavl::av::Packet c;
                c.type = kavl::av::kAudioConfig;
                c.ptsMs = p.ptsMs;
                c.data = opusHead_;
                mux.push(c);
                nextCfg = p.ptsMs + 1000;
            }
            mux.push(p);
            (takeVideo ? vi : ai)++;
        }
        const auto pay = mux.takeFrame(!mux.frameReady());
        kavl::Header h;
        h.modcod = mc.id;
        h.fec = fec_;
        h.seq = seq++ & 0xFFFF;
        n++;
        return kavl::frameSymbols(mc, h, pay.data());
    };
    const auto& taps = kavl::rrc();
    (void)taps;
    cur = makeFrame();
    const auto t0 = std::chrono::steady_clock::now();
    while (!quit_) {
        next = makeFrame();
        // pulse shaping of the current frame with H symbols of the previous and the next one around it
        std::vector<kavl::cf> win;
        win.reserve(cur.size() + 2 * static_cast<size_t>(H));
        win.insert(win.end(), prev.end() - H, prev.end());
        win.insert(win.end(), cur.begin(), cur.end());
        win.insert(win.end(), next.begin(), next.begin() + H);
        const auto shaped = kavl::Transmitter::shape(win);
        std::vector<kavl::cf> iq(shaped.begin() + static_cast<long>(H) * kavl::kSps, shaped.begin() + static_cast<long>(H + cur.size()) * kavl::kSps);
        std::vector<kavl::cf> out;
        if (resample) up.process(iq.data(), iq.size(), out);
        else out.swap(iq);
        if (wav) {
            std::vector<int16_t> s(out.size() * 2);
            for (size_t i = 0; i < out.size(); i++) {
                s[2 * i] = static_cast<int16_t>(std::lround(std::clamp(out[i].real() * 0.18f * 32767.f, -32768.f, 32767.f)));
                s[2 * i + 1] = static_cast<int16_t>(std::lround(std::clamp(out[i].imag() * 0.18f * 32767.f, -32768.f, 32767.f)));
            }
            std::fwrite(s.data(), 2, s.size(), wav);
            wavSamples += static_cast<long long>(out.size());
            if (live_) std::this_thread::sleep_until(t0 + std::chrono::microseconds(static_cast<long long>(n * frameMs * 1000.0)));   // live: real time
            else if (vi >= vids_.size() && ai >= auds_.size() && !mux.pending() && ++tailFrames > 2) quit_ = true;   // all sent, plus the frames in flight
        } else {
            // the HackRF wants int8; peaks of the shaped signal stay inside with the data at a quarter of full scale (peaks about 3.5 x)
            std::vector<int8_t> b(out.size() * 2);
            for (size_t i = 0; i < out.size(); i++) {
                b[2 * i] = static_cast<int8_t>(std::lround(std::clamp(out[i].real() * 32.f, -127.f, 127.f)));
                b[2 * i + 1] = static_cast<int8_t>(std::lround(std::clamp(out[i].imag() * 32.f, -127.f, 127.f)));
            }
            // keep about 0.3 s queued: wait while the radio drains it
            for (;;) {
                {
                    std::lock_guard<std::mutex> lk(ringMtx_);
                    if (ring_.size() < static_cast<size_t>(rate * 2 * 0.3) || quit_) {
                        ring_.insert(ring_.end(), b.begin(), b.end());
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        prev.swap(cur);
        cur.swap(next);
        std::lock_guard<std::mutex> sl(stMtx_);
        st_.frames = n;
        st_.loops = loops;
        st_.seconds = d.toFile ? static_cast<double>(wavSamples) / rate : std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    if (wav) {   // the WAV header
        auto put32 = [&](long pos, uint32_t v) {
            std::fseek(wav, pos, SEEK_SET);
            const uint8_t x[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
            std::fwrite(x, 1, 4, wav);
        };
        const uint32_t dataBytes = static_cast<uint32_t>(wavSamples * 4);
        std::fseek(wav, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, wav);
        put32(4, 36 + dataBytes);
        std::fseek(wav, 8, SEEK_SET);
        std::fwrite("WAVEfmt ", 1, 8, wav);
        put32(16, 16);
        const uint8_t fmt[4] = {1, 0, 2, 0};
        std::fseek(wav, 20, SEEK_SET);
        std::fwrite(fmt, 1, 4, wav);
        put32(24, static_cast<uint32_t>(std::lround(rate)));
        put32(28, static_cast<uint32_t>(std::lround(rate)) * 4);
        const uint8_t ba[4] = {4, 0, 16, 0};
        std::fseek(wav, 32, SEEK_SET);
        std::fwrite(ba, 1, 4, wav);
        std::fseek(wav, 36, SEEK_SET);
        std::fwrite("data", 1, 4, wav);
        put32(40, dataBytes);
        std::fclose(wav);
        std::lock_guard<std::mutex> sl(stMtx_);
        st_.message = "Written: " + d.path;
    }
    running_ = false;
}
