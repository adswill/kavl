// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app: the spectrum and the waterfall of the receiver's input. A frequency axis (MHz) and a dB scale; click to tune, drag to pan, the mouse
// wheel zooms (double-click: the whole span again); the band of the profile can be dragged to retune; a hover readout of frequency and level.
// Display: averaging, peak hold, an automatic or a set dB range, the split between spectrum and waterfall can be dragged. Draws with Dear ImGui;
// the waterfall is a 1024 x 256 texture of the whole span (zoom shows a part of it).
#pragma once
#include "imgui.h"

#include <string>
#include <vector>

class Settings;

class SpectrumView {
public:
    struct Input {
        const std::vector<float>* db = nullptr;   // power per bin, -fs/2 .. +fs/2 of the radio's centre; nullptr or empty: no data
        double fs = 0, radioHz = 0;                // sample rate, the frequency the radio is tuned to (receive frequency + its DC offset)
        double rxHz = 0, bandHalfHz = 0;           // where the receiver looks and half the profile's width (0: unknown)
        double txHz = 0;                           // a transmitter of this Mac on the air (0: none)
        bool fresh = false;                        // db holds a new spectrum (the waterfall moves on)
    };
    ~SpectrumView();
    // Draws into size at the cursor. Returns true and sets tuneHz when the user picked a new receive frequency.
    bool draw(const Input& in, ImVec2 size, ImFont* small, double& tuneHz);
    // the option row (averaging, peak hold, range, waterfall)
    void options();
    void clear();   // a new receiver: forget the history
    void load(const Settings& s);
    void save(Settings& s) const;
    bool waterfall = true;

private:
    void addRow(const std::vector<float>& db);
    float lo_ = 0.f, hi_ = 1.f;   // the part of the span on show (0..1)
    bool peak_ = false, autoRange_ = true;
    float avg_ = 0.5f, minDb_ = -100.f, maxDb_ = -30.f, split_ = 0.45f;
    float autoLo_ = -100.f, autoHi_ = -40.f;
    bool rangeSet_ = false;
    std::vector<float> disp_, peakDb_;
    unsigned tex_ = 0;
    static constexpr int kW = 1024, kH = 256;
    std::vector<uint32_t> rows_ = std::vector<uint32_t>(static_cast<size_t>(kW) * kH, 0xFF000000u);
    // mouse
    bool dragging_ = false, bandDrag_ = false;
    float pressX_ = 0, bandShiftHz_ = 0;
    double pendingTuneHz_ = 0, pendingAt_ = -1;   // a click waits for the double-click time (a double-click only resets the zoom)
    double lastRxHz_ = 0;
};
