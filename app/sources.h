// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app: where the IQ comes from.
//   IQ file:  a .wav (16-bit or float), played at its own rate in real time, looped.
//   RTL-SDR:  through rtl_tcp (the program that comes with librtlsdr, started by the app for a local dongle, or a remote rtl_tcp server); the
//             dongles are listed with libusb. 2.4 Msps.
//   HackRF:   through libhackrf, chosen by serial number. 4 Msps.
// The radios tune offsetHz() above the wanted frequency when the sample rate leaves room, so their DC spike sits outside the KAVL signal; the
// receive engine shifts the signal back.
#pragma once
#include "kavl/kavl_common.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class IqSource {
public:
    using Sink = std::function<void(const kavl::cf*, size_t)>;
    virtual ~IqSource() = default;
    virtual bool start(Sink sink) = 0;   // false: see error()
    virtual void stop() = 0;
    virtual double sampleRate() const = 0;
    virtual double offsetHz() const { return 0.0; }
    virtual void setFrequency(double /*hz*/) {}
    virtual std::string error() const { return err_; }
    virtual std::string name() const = 0;
protected:
    std::string err_;
};

struct RadioDevice {
    std::string label;    // shown in the device list
    std::string serial;   // HackRF serial / RTL-SDR serial string
    int index = 0;        // RTL-SDR: the device index rtl_tcp uses
};
std::vector<RadioDevice> listRtlSdr();    // RTL2832U dongles on USB
std::vector<RadioDevice> listHackRf();    // HackRF One / Pro on USB
// The rtl_tcp program (Homebrew, /usr/local, PATH), empty if not found.
std::string findRtlTcp();

struct RtlSettings {
    bool remote = false;            // connect to host:port instead of starting rtl_tcp
    std::string host = "127.0.0.1";
    int port = 1234;
    int index = 0;                  // local dongle
    int gainTenthsDb = -1;          // -1 = automatic
    int ppm = 0;
    bool biasTee = false;
    bool agc = false;               // the RTL2832U's own AGC
};
struct HackRfSettings {
    std::string serial;             // empty = the first one
    int lna = 24;                   // 0..40 dB in 8 dB steps
    int vga = 20;                   // 0..62 dB in 2 dB steps
    bool amp = false;               // +14 dB RF amplifier
    bool antennaPower = false;      // bias on the antenna port
};

std::unique_ptr<IqSource> makeFileSource(const std::string& path);
double frequencyFromName(const std::string& path);   // "..._433000000Hz..." -> Hz, 0 if none
// signalWidthHz: the occupied width of the profile (chooses the DC offset)
std::unique_ptr<IqSource> makeRtlSource(const RtlSettings& s, double freqHz, double signalWidthHz);
std::unique_ptr<IqSource> makeHackRfSource(const HackRfSettings& s, double freqHz, double signalWidthHz);
