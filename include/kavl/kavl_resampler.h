// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// Streaming resampler for any ratio (Kaiser-windowed sinc, 512 tabulated phases with linear interpolation between them), for receivers whose input rate is
// not 4 x the symbol rate (an RTL-SDR at 2.4 Msps). Standalone.
#pragma once
#include "kavl/kavl_common.h"
#include <vector>

namespace kavl {

class Resampler {
public:
    // halfTaps: half the filter length in input samples at equal rates (longer when decimating). False for a bad ratio.
    bool configure(double inRate, double outRate, int halfTaps = 16);
    void reset();
    // Appends the resampled samples of in[0..n) to out.
    void process(const cf* in, size_t n, std::vector<cf>& out);
private:
    double step_ = 1.0;     // input samples per output sample
    double pos_ = 0.0;      // position of the next output in buf_
    int half_ = 16;         // taps on each side
    static constexpr int kPhases = 512;
    std::vector<float> tab_;   // (kPhases + 1) x (2 half_) weights
    std::vector<cf> buf_;
};

}  // namespace kavl
