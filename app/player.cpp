// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app player (player.h).
#include "player.h"

#include <dav1d/dav1d.h>
#include <opus.h>

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#include "miniaudio.h"

#include <algorithm>
#include <cstring>

namespace {
constexpr double kStartBufferMs = 2000.0;   // covers a protection group (the transmitter sends 3 s ahead)
constexpr size_t kMaxQueued = 6000;
constexpr size_t kRingMax = 48000 * 2;   // 1 s of stereo
}  // namespace

struct Player::Device {
    ma_device dev{};
    bool ok = false;
};

void Player::audioCallback(void* user, float* out, unsigned frames) {
    auto* self = static_cast<Player*>(user);
    const float vol = self->volume_.load();
    std::lock_guard<std::mutex> lk(self->ringMtx_);
    for (unsigned i = 0; i < frames * 2; i++) {
        if (!self->ring_.empty()) {
            out[i] = self->ring_.front() * vol;
            self->ring_.pop_front();
        } else {
            out[i] = 0.f;
        }
    }
}

Player::Player() : dev_(std::make_unique<Device>()) {
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = 48000;
    cfg.dataCallback = [](ma_device* d, void* out, const void*, ma_uint32 n) { audioCallback(d->pUserData, static_cast<float*>(out), n); };
    cfg.pUserData = this;
    if (ma_device_init(nullptr, &cfg, &dev_->dev) == MA_SUCCESS) dev_->ok = ma_device_start(&dev_->dev) == MA_SUCCESS;
}

Player::~Player() {
    stop();
    if (dev_->ok) ma_device_uninit(&dev_->dev);
}

void Player::start() {
    stop();
    quit_ = false;
    playing_ = false;
    vth_ = std::thread([this] { videoLoop(); });
    ath_ = std::thread([this] { audioLoop(); });
}

void Player::stop() {
    quit_ = true;
    if (vth_.joinable()) vth_.join();
    if (ath_.joinable()) ath_.join();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        vq_.clear();
        aq_.clear();
        haveLast_ = false;
        rateWin_.clear();
        playing_ = false;
        epoch_++;
    }
    std::lock_guard<std::mutex> rl(ringMtx_);
    ring_.clear();
}

void Player::push(std::vector<kavl::av::Packet>&& packets) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& p : packets) {
        if (p.type == kavl::av::kAudioConfig) {
            opusHead_ = p.data;
            continue;
        }
        if (p.type == kavl::av::kVideo) {
            if (haveLast_ && (p.ptsMs + 1000 < lastPts_ || p.ptsMs > lastPts_ + 10000)) {   // a jump: a new time line
                vq_.clear();
                aq_.clear();
                rateWin_.clear();
                playing_ = false;
                epoch_++;
            }
            lastPts_ = p.ptsMs;
            haveLast_ = true;
            rateWin_.emplace_back(p.ptsMs, p.data.size());
            while (!rateWin_.empty() && rateWin_.front().first + 3000 < p.ptsMs) rateWin_.pop_front();
            vq_.push_back(std::move(p));
            if (vq_.size() > kMaxQueued) vq_.pop_front();
        } else if (p.type == kavl::av::kAudio) {
            aq_.push_back(std::move(p));
            if (aq_.size() > kMaxQueued) aq_.pop_front();
        }
    }
}

double Player::nowPts() const {
    return basePts_.load() + (std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count() - t0ns_.load()) / 1e6;
}

Player::Status Player::status() {
    Status s;
    {
        std::lock_guard<std::mutex> lk(stMtx_);
        s = st_;
    }
    s.playing = playing_;
    s.soundDevice = dev_->ok;
    std::lock_guard<std::mutex> lk(mtx_);
    if (rateWin_.size() > 1) {
        size_t b = 0;
        for (const auto& r : rateWin_) b += r.second;
        const double span = (rateWin_.back().first - rateWin_.front().first) / 1000.0;
        if (span > 0.5) s.videoKbps = b * 8.0 / span / 1000.0;
    }
    if (s.playing && haveLast_) s.bufferSec = std::max(0.0, (lastPts_ - nowPts()) / 1000.0);
    return s;
}

void Player::videoLoop() {
    struct Pic {
        double pts;
        int w, h;
        std::vector<uint8_t> y, u, v;
    };
    std::deque<Pic> pics;
    std::deque<kavl::av::Packet> local;
    Dav1dContext* dec = nullptr;
    int epoch = -1;
    bool keySeen = false;
    Clock::time_point firstKeyAt;
    auto openDecoder = [&]() {
        if (dec) dav1d_close(&dec);
        Dav1dSettings s;
        dav1d_default_settings(&s);
        s.n_threads = 6;
        s.max_frame_delay = 2;
        if (dav1d_open(&dec, &s) != 0) dec = nullptr;
    };
    // a decoded picture -> 8-bit packed planes
    auto take = [&](const Dav1dPicture& p) {
        Pic pic;
        pic.pts = static_cast<double>(p.m.timestamp);
        pic.w = p.p.w;
        pic.h = p.p.h;
        const int cw = (p.p.w + 1) / 2, ch = (p.p.h + 1) / 2;
        pic.y.resize(static_cast<size_t>(pic.w) * pic.h);
        pic.u.resize(static_cast<size_t>(cw) * ch);
        pic.v.resize(static_cast<size_t>(cw) * ch);
        const bool hi = p.p.bpc > 8;
        auto plane = [&](const void* src, ptrdiff_t stride, int w, int h, std::vector<uint8_t>& dst) {
            for (int r = 0; r < h; r++) {
                if (!hi) {
                    std::memcpy(dst.data() + static_cast<size_t>(r) * w, static_cast<const uint8_t*>(src) + r * stride, static_cast<size_t>(w));
                } else {
                    const auto* s16 = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(src) + r * stride);
                    for (int c = 0; c < w; c++) dst[static_cast<size_t>(r) * w + c] = static_cast<uint8_t>(s16[c] >> (p.p.bpc - 8));
                }
            }
        };
        plane(p.data[0], p.stride[0], pic.w, pic.h, pic.y);
        if (p.p.layout == DAV1D_PIXEL_LAYOUT_I420) {
            plane(p.data[1], p.stride[1], cw, ch, pic.u);
            plane(p.data[2], p.stride[1], cw, ch, pic.v);
        } else {   // monochrome or other layouts: grey colour planes
            std::fill(pic.u.begin(), pic.u.end(), 128);
            std::fill(pic.v.begin(), pic.v.end(), 128);
        }
        {
            std::lock_guard<std::mutex> sl(stMtx_);
            st_.srcW = pic.w;
            st_.srcH = pic.h;
        }
        pics.push_back(std::move(pic));
    };
    while (!quit_) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (epoch != epoch_) {
                epoch = epoch_;
                local.clear();
                pics.clear();
                keySeen = false;
                openDecoder();
            }
            while (!vq_.empty() && local.size() < 1200) {
                local.push_back(std::move(vq_.front()));
                vq_.pop_front();
            }
        }
        if (!keySeen) {
            while (!local.empty() && !local.front().key) local.pop_front();
            if (!local.empty()) {
                keySeen = true;
                firstKeyAt = Clock::now();
            }
        }
        if (keySeen && !playing_ && !local.empty() && Clock::now() - firstKeyAt > std::chrono::milliseconds(static_cast<int>(kStartBufferMs))) {
            basePts_ = static_cast<double>(local.front().ptsMs);
            t0ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
            playing_ = true;
        }
        while (dec && keySeen && pics.size() < 3 && !local.empty()) {
            const auto& pk = local.front();
            Dav1dData data{};
            uint8_t* buf = dav1d_data_create(&data, pk.data.size());
            if (buf) {
                std::memcpy(buf, pk.data.data(), pk.data.size());
                data.m.timestamp = pk.ptsMs;
                int r;
                do {
                    r = dav1d_send_data(dec, &data);
                    Dav1dPicture p{};
                    while (dav1d_get_picture(dec, &p) == 0) {
                        take(p);
                        dav1d_picture_unref(&p);
                    }
                } while (r == DAV1D_ERR(EAGAIN) && data.sz > 0 && !quit_);
                if (data.sz > 0) dav1d_data_unref(&data);
            }
            local.pop_front();
        }
        if (dec && pics.empty()) {   // pictures that came out late
            Dav1dPicture p{};
            while (dav1d_get_picture(dec, &p) == 0) {
                take(p);
                dav1d_picture_unref(&p);
            }
        }
        if (playing_ && !pics.empty()) {
            const double now = nowPts();
            while (pics.size() > 1 && pics[1].pts <= now) {
                pics.pop_front();
                std::lock_guard<std::mutex> sl(stMtx_);
                st_.late++;
            }
            if (pics.front().pts <= now) {
                {
                    std::lock_guard<std::mutex> vl(pic_.m);
                    pic_.y.swap(pics.front().y);
                    pic_.u.swap(pics.front().u);
                    pic_.v.swap(pics.front().v);
                    pic_.w = pics.front().w;
                    pic_.h = pics.front().h;
                    pic_.fresh = true;
                }
                pics.pop_front();
                std::lock_guard<std::mutex> sl(stMtx_);
                st_.shown++;
                fpsN_++;
                const double dt = std::chrono::duration<double>(Clock::now() - fpsT0_).count();
                if (dt >= 1.0) {
                    st_.fps = fpsN_ / dt;
                    fpsN_ = 0;
                    fpsT0_ = Clock::now();
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (dec) dav1d_close(&dec);
}

void Player::audioLoop() {
    OpusDecoder* dec = nullptr;
    std::vector<uint8_t> head;
    int epoch = -1, skip = 0;
    std::vector<float> pcm(5760 * 2);
    while (!quit_) {
        kavl::av::Packet p;
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (epoch != epoch_) {
                epoch = epoch_;
                if (dec) opus_decoder_destroy(dec);
                dec = nullptr;
                head.clear();
                std::lock_guard<std::mutex> rl(ringMtx_);
                ring_.clear();
            }
            if (head.empty() && !opusHead_.empty()) head = opusHead_;
            if (playing_ && !aq_.empty()) {
                const double now = nowPts();
                while (!aq_.empty() && aq_.front().ptsMs + 300.0 < now) aq_.pop_front();
                if (!aq_.empty() && aq_.front().ptsMs <= now + 60.0) {
                    p = std::move(aq_.front());
                    aq_.pop_front();
                    have = true;
                }
            }
        }
        if (!dec && head.size() >= 19) {
            int err = 0;
            dec = opus_decoder_create(48000, 2, &err);
            if (err != OPUS_OK) dec = nullptr;
            skip = head[10] | (head[11] << 8);   // the pre-skip of the OpusHead
        }
        if (!have || !dec) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;
        }
        const int n = opus_decode_float(dec, p.data.data(), static_cast<opus_int32>(p.data.size()), pcm.data(), 5760, 0);
        if (n <= 0) continue;
        {
            std::lock_guard<std::mutex> sl(stMtx_);
            st_.audioPackets++;
        }
        std::lock_guard<std::mutex> rl(ringMtx_);
        for (int i = 0; i < n; i++) {
            if (skip > 0) {
                skip--;
                continue;
            }
            ring_.push_back(pcm[static_cast<size_t>(2 * i)]);
            ring_.push_back(pcm[static_cast<size_t>(2 * i + 1)]);
        }
        while (ring_.size() > kRingMax) ring_.pop_front();
    }
    if (dec) opus_decoder_destroy(dec);
}
