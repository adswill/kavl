// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app: the edges of a signal in a power spectrum (dB per bin), shared by the receiver (which profile, how far off) and the signal finder.
#pragma once
#include <algorithm>
#include <vector>

// Narrows a run of bins [l, r] (inclusive) that stands out of the noise to the signal's own edges: the outermost bins within 4 dB of its
// in-band level (the 70th percentile of the run). A strong narrow signal spreads a few bins into the noise through the FFT window, the
// transmitter's filter skirts and the averaging (a 12 kHz signal 45 dB up measured 33 kHz above the noise); measured against its own level,
// the width holds at any strength.
inline void signalEdges(const std::vector<float>& db, int& l, int& r) {
    if (r - l < 2) return;
    std::vector<float> run(db.begin() + l, db.begin() + r + 1);
    const size_t k = run.size() * 7 / 10;
    std::nth_element(run.begin(), run.begin() + static_cast<long>(k), run.end());
    const float thr = run[k] - 4.f;
    int a = l, b = r;
    while (a < b && db[static_cast<size_t>(a)] < thr) a++;
    while (b > a && db[static_cast<size_t>(b)] < thr) b--;
    l = a, r = b;
}
