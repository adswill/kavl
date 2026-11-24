// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app: control of a ham transceiver (CAT) through Hamlib's rigctld, a separate program (brew install hamlib) that KAVL either starts
// itself or finds already running, and talks to over TCP. No Hamlib code is in KAVL.
//   - PTT: keyed before KAVL transmits and released after (lead and tail times for a sequencer, a PA or a preamp relay)
//   - frequency: the rig can follow the receive or the transmit frequency, and its own frequency is read back once a second
// Everything goes through one worker thread, so a slow or missing rig never holds up the window.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>

struct RigSettings {
    int mode = 0;                 // 0 off, 1 a rigctld that is already running (host:port), 2 KAVL starts rigctld itself
    std::string host = "127.0.0.1";
    int port = 4532;
    int model = 1;                // Hamlib rig model (rigctl -l lists them; 1 = a simulated rig, for trying it out)
    std::string device;           // the rig's serial port, e.g. /dev/cu.usbserial-1420 (mode 2)
    int baud = 0;                 // 0 = the rig's default
    int pttType = 0;              // 0 the rig's own (CAT), 1 RTS, 2 DTR on the serial port (mode 2)
    bool ptt = true;              // key the rig while KAVL transmits
    int leadMs = 100, tailMs = 100;   // PTT on this long before the first sample, off this long after the last
    int follow = 0;               // the rig's frequency follows: 0 nothing, 1 the receive, 2 the transmit frequency (on the air)
};

class RigControl {
public:
    ~RigControl() { stop(); }
    void start(const RigSettings& s);   // (re)connects in the background; status() says how it went
    void stop();                        // PTT off first, then rigctld stopped if KAVL started it
    bool active() const { return running_; }
    void setFrequency(double hz);       // queued; only the newest counts
    void setPtt(bool on);               // queued in order, sent before any frequency change behind it
    // Blocks until the PTT command queued last has been answered (or about 1 s): the transmitter starts only once the rig has switched.
    bool waitPtt();
    struct Status {
        bool connected = false;
        bool ptt = false;
        double rigHz = 0;                // read back from the rig, 0 if unknown
        std::string error;               // why it is not connected, or the last refusal
    };
    Status status();
    static std::string findRigctld();   // the rigctld program, or "" when Hamlib is not installed

private:
    void worker(RigSettings s);
    bool connectTo(const std::string& host, int port);
    bool command(const std::string& line, std::string& reply);   // one line out, one line back (1 s timeout)
    void setError(const std::string& e);
    void disconnect();

    std::thread th_;
    std::atomic<bool> running_{false}, quit_{false};
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<int> ptt_;                // queued PTT states
    double freqHz_ = 0;                  // queued frequency (0 = none)
    long pttQueued_ = 0, pttDone_ = 0;
    std::condition_variable doneCv_;
    Status st_;
    int fd_ = -1;
    pid_t child_ = -1;
};
