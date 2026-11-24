// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app loopback: this Mac's transmitter straight into its receiver, no radios. The transmitter hands over the same int8 samples it would give a
// HackRF (at 2.4 Msps, drained in real time); the receive side runs its own real-time clock, adds a little noise and moves the signal by the
// difference between the transmit and the receive frequency (so tuning works as with a radio: off by more than 1.2 MHz and the signal is gone).
#pragma once
#include "sources.h"

#include <deque>
#include <mutex>

class LoopbackBus {
public:
    static constexpr double kRate = 2.4e6;
    static LoopbackBus& get();
    // transmit side: samples at kRate, sent on txHz
    void push(const kavl::cf* x, size_t n, double txHz);
    // receive side: up to n samples (fewer when the transmitter sent less), and the frequency they were sent on
    size_t pull(kavl::cf* out, size_t n, double& txHz);
    void clear();
private:
    std::mutex m_;
    std::deque<kavl::cf> q_;
    double txHz_ = 0;
};

std::unique_ptr<IqSource> makeLoopbackSource(double freqHz);
