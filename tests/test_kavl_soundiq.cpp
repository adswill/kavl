// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app sound-card IQ (app/engine/sound_iq.h): left/right to I/Q, and swapped. Opens no audio device.
#include "sound_iq.h"
#include <cstdio>

int main() {
    const float lr[6] = {0.1f, -0.2f, 0.3f, 0.4f, -0.5f, 0.6f};
    std::vector<kavl::cf> iq;
    int fails = 0;
    soundIqToIq(lr, 3, false, iq);
    if (iq.size() != 3 || iq[0] != kavl::cf(0.1f, -0.2f) || iq[2] != kavl::cf(-0.5f, 0.6f)) fails++;
    soundIqToIq(lr, 3, true, iq);
    if (iq.size() != 3 || iq[0] != kavl::cf(-0.2f, 0.1f) || iq[1] != kavl::cf(0.4f, 0.3f)) fails++;
    std::printf(fails ? "test_kavl_soundiq: %d failures\n" : "test_kavl_soundiq: all passed\n", fails);
    return fails ? 1 : 0;
}
