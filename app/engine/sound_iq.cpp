// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app: IQ from a sound card (sound_iq.h), through miniaudio (its implementation is compiled in player.cpp).
#include "sound_iq.h"
#include "miniaudio.h"
#include <cstring>

void soundIqToIq(const float* lr, size_t frames, bool swap, std::vector<kavl::cf>& out) {
    out.resize(frames);
    for (size_t i = 0; i < frames; i++) out[i] = swap ? kavl::cf(lr[2 * i + 1], lr[2 * i]) : kavl::cf(lr[2 * i], lr[2 * i + 1]);
}

std::vector<std::string> listSoundInputs() {
    std::vector<std::string> v;
    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return v;
    ma_device_info* play = nullptr;
    ma_device_info* cap = nullptr;
    ma_uint32 np = 0, nc = 0;
    if (ma_context_get_devices(&ctx, &play, &np, &cap, &nc) == MA_SUCCESS)
        for (ma_uint32 i = 0; i < nc; i++) v.emplace_back(cap[i].name);
    ma_context_uninit(&ctx);
    return v;
}

namespace {
class SoundIqSource : public IqSource {
public:
    SoundIqSource(const SoundIqSettings& s, double freqHz, std::function<void(double)> tune) : s_(s), freq_(freqHz), tune_(std::move(tune)) {}
    ~SoundIqSource() override { stop(); }
    bool start(Sink sink) override {
        sink_ = std::move(sink);
        if (ma_context_init(nullptr, 0, nullptr, &ctx_) != MA_SUCCESS) {
            err_ = "No audio system";
            return false;
        }
        haveCtx_ = true;
        ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
        cfg.capture.format = ma_format_f32;
        cfg.capture.channels = 2;
        cfg.sampleRate = static_cast<ma_uint32>(s_.rate);
        cfg.periodSizeInMilliseconds = 20;
        cfg.dataCallback = [](ma_device* d, void*, const void* in, ma_uint32 n) { static_cast<SoundIqSource*>(d->pUserData)->onAudio(static_cast<const float*>(in), n); };
        cfg.pUserData = this;
        ma_device_id id{};
        if (!s_.device.empty()) {
            ma_device_info* play = nullptr;
            ma_device_info* cap = nullptr;
            ma_uint32 np = 0, nc = 0;
            bool found = false;
            if (ma_context_get_devices(&ctx_, &play, &np, &cap, &nc) == MA_SUCCESS)
                for (ma_uint32 i = 0; i < nc && !found; i++)
                    if (s_.device == cap[i].name) id = cap[i].id, found = true;
            if (!found) {
                err_ = "The audio input \"" + s_.device + "\" is not there";
                return false;
            }
            cfg.capture.pDeviceID = &id;
        }
        if (ma_device_init(&ctx_, &cfg, &dev_) != MA_SUCCESS) {
            err_ = "Cannot open the audio input (allow KAVL the microphone in System Settings > Privacy & Security)";
            return false;
        }
        haveDev_ = true;
        if (dev_.sampleRate != static_cast<ma_uint32>(s_.rate)) {   // the input runs at another rate: say so rather than mis-tune
            err_ = "The audio input runs at " + std::to_string(dev_.sampleRate) + " Hz, not " + std::to_string(s_.rate) + ": choose that rate";
            return false;
        }
        if (tune_) tune_(freq_);
        if (ma_device_start(&dev_) != MA_SUCCESS) {
            err_ = "Cannot start the audio input";
            return false;
        }
        return true;
    }
    void stop() override {
        if (haveDev_) ma_device_uninit(&dev_);
        if (haveCtx_) ma_context_uninit(&ctx_);
        haveDev_ = haveCtx_ = false;
    }
    double sampleRate() const override { return s_.rate; }
    double offsetHz() const override { return -s_.centreHz; }   // the signal sits at centreHz in the IQ; the engine shifts it to the middle
    void setFrequency(double hz) override {
        freq_ = hz;
        if (tune_) tune_(hz);
    }
    std::string name() const override { return "Sound card IQ"; }

private:
    void onAudio(const float* lr, ma_uint32 n) {
        soundIqToIq(lr, n, s_.swapIq, buf_);
        if (sink_) sink_(buf_.data(), buf_.size());
    }
    SoundIqSettings s_;
    double freq_;
    std::function<void(double)> tune_;
    Sink sink_;
    ma_context ctx_{};
    ma_device dev_{};
    bool haveCtx_ = false, haveDev_ = false;
    std::vector<kavl::cf> buf_;
};
}  // namespace

std::unique_ptr<IqSource> makeSoundIqSource(const SoundIqSettings& s, double freqHz, std::function<void(double)> tune) {
    return std::make_unique<SoundIqSource>(s, freqHz, std::move(tune));
}
