// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL resampler (kavl/include/kavl/kavl_resampler.h).
#include "kavl/kavl_resampler.h"
#include <algorithm>
#include <cmath>

namespace kavl {

namespace {
constexpr double kPi = 3.141592653589793;
double besselI0(double x) {
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 40; k++) {
        t *= (x / (2.0 * k)) * (x / (2.0 * k));
        s += t;
    }
    return s;
}
}  // namespace

bool Resampler::configure(double inRate, double outRate, int halfTaps) {
    if (inRate <= 0 || outRate <= 0 || outRate > 4.0 * inRate) return false;
    step_ = inRate / outRate;
    const double scale = std::min(1.0, outRate / inRate);   // < 1 when decimating: the filter must cut at the output's band edge
    half_ = std::max(4, static_cast<int>(std::ceil(halfTaps / scale)));
    const double fc = 0.5 * scale * 0.92;                   // cut-off in cycles per input sample
    const double beta = 8.0, i0b = besselI0(beta);
    const int taps = 2 * half_;
    tab_.assign(static_cast<size_t>((kPhases + 1) * taps), 0.f);
    for (int p = 0; p <= kPhases; p++) {
        const double mu = static_cast<double>(p) / kPhases;   // output at i + mu uses x[i - half_ + 1 + k]
        double sum = 0;
        std::vector<double> w(static_cast<size_t>(taps));
        for (int k = 0; k < taps; k++) {
            const double x = static_cast<double>(k - half_ + 1) - mu;
            const double sx = std::fabs(x) < 1e-12 ? 1.0 : std::sin(2.0 * kPi * fc * x) / (2.0 * kPi * fc * x);
            const double r = x / (half_ + 1.0);
            const double win = std::fabs(r) >= 1.0 ? 0.0 : besselI0(beta * std::sqrt(1.0 - r * r)) / i0b;
            w[static_cast<size_t>(k)] = 2.0 * fc * sx * win;
            sum += w[static_cast<size_t>(k)];
        }
        for (int k = 0; k < taps; k++) tab_[static_cast<size_t>(p * taps + k)] = static_cast<float>(w[static_cast<size_t>(k)] / sum);
    }
    reset();
    return true;
}

void Resampler::reset() {
    buf_.assign(static_cast<size_t>(half_), cf(0, 0));   // history of zeros
    pos_ = static_cast<double>(half_) - 1.0;
}

void Resampler::process(const cf* in, size_t n, std::vector<cf>& out) {
    buf_.insert(buf_.end(), in, in + n);
    const int taps = 2 * half_;
    for (;;) {
        const long i = static_cast<long>(std::floor(pos_));
        if (i + half_ >= static_cast<long>(buf_.size())) break;
        const double frac = (pos_ - static_cast<double>(i)) * kPhases;
        const int p = std::min(kPhases - 1, static_cast<int>(frac));
        const float u = static_cast<float>(frac - p);
        const float* a = &tab_[static_cast<size_t>(p * taps)];
        const float* b = a + taps;
        cf acc(0, 0);
        const cf* x = &buf_[static_cast<size_t>(i - half_ + 1)];
        for (int k = 0; k < taps; k++) acc += x[k] * (a[k] + u * (b[k] - a[k]));
        out.push_back(acc);
        pos_ += step_;
    }
    // keep the history the next outputs need
    const long keep0 = static_cast<long>(std::floor(pos_)) - half_ + 1;
    if (keep0 > 4096) {
        buf_.erase(buf_.begin(), buf_.begin() + keep0);
        pos_ -= static_cast<double>(keep0);
    }
}

}  // namespace kavl
