// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app: IQ from a sound card: rigs and SDRs that give I and Q as the left and right channel of an audio input (Elecraft KX3/KX2 I/Q
// out, SoftRock, FUNcube Dongle Pro+ and the like), at 48, 96 or 192 kHz. Enough for the narrow profiles (12 and 24 kHz). The rig is tuned
// through rig control (rig.h): setFrequency() is handed on to it.
#pragma once
#include "sources.h"
#include <functional>

struct SoundIqSettings {
    std::string device;      // the input's name, as listSoundInputs() gives it ("" = the system's default input)
    int rate = 48000;
    bool swapIq = false;     // some rigs give Q on the left
    double centreHz = 0;     // where the dial frequency sits in the IQ (an IF offset; 0 for most)
};

std::vector<std::string> listSoundInputs();   // names of the audio inputs (lists them; opens none)
// tune: called with the frequency the rig should go to (at start and on every retune); may be empty
std::unique_ptr<IqSource> makeSoundIqSource(const SoundIqSettings& s, double freqHz, std::function<void(double)> tune);
// Stereo float frames (L, R, L, R, ...) to IQ: I = left, Q = right, or the other way round with swap.
void soundIqToIq(const float* lr, size_t frames, bool swap, std::vector<kavl::cf>& out);
