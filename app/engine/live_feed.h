// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app: live programme. The ffmpeg program captures a camera and a microphone (macOS AVFoundation), or makes a test pattern with a tone, and
// encodes AV1 (SVT-AV1, low delay, constant bit rate) and Opus in real time into two pipes; this reads them as they come (IVF and Ogg streams)
// and hands out packets with their times. ffmpeg is a separate program (LGPL, bundled in KAVL.app; it is not part of KAVL).
#pragma once
#include "kavl/kavl_av.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// a helper program: the copy bundled in KAVL.app first, then Homebrew; the bare name when none is found
std::string findTool(const char* name);

struct CaptureDevice {
    int index = 0;
    std::string name;
};
// The cameras and microphones AVFoundation offers (asks ffmpeg).
void listCaptureDevices(std::vector<CaptureDevice>& cameras, std::vector<CaptureDevice>& microphones);

class LiveFeed {
public:
    ~LiveFeed() { stop(); }
    // camera < 0: a test pattern with a 1 kHz tone (no camera needed). mic < 0: no sound from the camera side (the test pattern has its tone).
    // overlay: a raw RGBA image of width x height laid over the picture (the callsign, callsign.h); empty = none
    bool start(int camera, int mic, int width, int height, int fps, int videoKbps, const std::string& overlay = "");
    void stop();
    bool running() const { return running_; }
    std::string error();
    // the packets that arrived since the last call; head = the OpusHead once known; videoWallMs (optional): when each video unit came out of the
    // encoder (ms, Unix time)
    void take(std::vector<kavl::av::Packet>& video, std::vector<kavl::av::Packet>& audio, std::vector<uint8_t>& head,
              std::vector<int64_t>* videoWallMs = nullptr);
    struct Status {
        long videoUnits = 0, audioPackets = 0;
        double fps = 0, kbps = 0;
        int captureW = 0, captureH = 0;   // the camera mode in use (0 for the test pattern)
        double captureFps = 0;
    };
    Status status();

private:
    void readVideo(int fd);
    void readAudio(int fd);
    std::atomic<bool> running_{false};
    int pid_ = 0;
    std::thread vth_, ath_, eth_;
    std::mutex m_;
    std::deque<kavl::av::Packet> v_, a_;
    std::deque<int64_t> vWall_;
    std::vector<uint8_t> head_;
    std::string err_;
    Status st_;
    std::deque<std::pair<double, size_t>> win_;   // (seconds, bytes) of the recent video units
};
