// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app loopback (loopback.h).
#include "loopback.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

LoopbackBus& LoopbackBus::get() {
    static LoopbackBus bus;
    return bus;
}

void LoopbackBus::push(const kavl::cf* x, size_t n, double txHz) {
    std::lock_guard<std::mutex> lk(m_);
    txHz_ = txHz;
    q_.insert(q_.end(), x, x + n);
    const size_t cap = static_cast<size_t>(kRate * 0.5);   // nobody listening: keep half a second at most
    if (q_.size() > cap) q_.erase(q_.begin(), q_.begin() + static_cast<long>(q_.size() - cap));
}

size_t LoopbackBus::pull(kavl::cf* out, size_t n, double& txHz) {
    std::lock_guard<std::mutex> lk(m_);
    const size_t k = std::min(n, q_.size());
    std::copy(q_.begin(), q_.begin() + static_cast<long>(k), out);
    q_.erase(q_.begin(), q_.begin() + static_cast<long>(k));
    txHz = txHz_;
    return k;
}

void LoopbackBus::clear() {
    std::lock_guard<std::mutex> lk(m_);
    q_.clear();
}

namespace {

class LoopbackSource : public IqSource {
public:
    explicit LoopbackSource(double f) : freq_(f) {}
    ~LoopbackSource() override { stop(); }
    std::string name() const override { return "Loopback"; }
    double sampleRate() const override { return LoopbackBus::kRate; }
    void setFrequency(double hz) override { freq_ = hz; }
    bool start(Sink sink) override {
        stop();
        LoopbackBus::get().clear();
        quit_ = false;
        th_ = std::thread([this, sink] { run(sink); });
        return true;
    }
    void stop() override {
        quit_ = true;
        if (th_.joinable()) th_.join();
    }

private:
    void run(const Sink& sink) {
        const size_t n = static_cast<size_t>(LoopbackBus::kRate / 100.0);   // 10 ms
        std::vector<kavl::cf> sig(n), out(n);
        std::mt19937 rng(2026);
        std::normal_distribution<float> noise(0.f, 0.006f);
        kavl::cf ph(1, 0);
        auto next = std::chrono::steady_clock::now();
        while (!quit_) {
            double txHz = 0;
            const size_t got = LoopbackBus::get().pull(sig.data(), n, txHz);
            const double shift = txHz - freq_.load();
            const bool inBand = std::abs(shift) < LoopbackBus::kRate / 2.0;
            const double w = 2.0 * 3.14159265358979 * shift / LoopbackBus::kRate;
            const kavl::cf step(static_cast<float>(std::cos(w)), static_cast<float>(std::sin(w)));
            for (size_t i = 0; i < n; i++) {
                kavl::cf s(noise(rng), noise(rng));
                if (i < got && inBand) {
                    s += sig[i] * ph;
                    ph *= step;
                }
                out[i] = s;
            }
            ph /= std::abs(ph);
            sink(out.data(), n);
            next += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(next);
        }
    }
    std::atomic<double> freq_;
    std::thread th_;
    std::atomic<bool> quit_{false};
};

}  // namespace

std::unique_ptr<IqSource> makeLoopbackSource(double freqHz) { return std::make_unique<LoopbackSource>(freqHz); }
