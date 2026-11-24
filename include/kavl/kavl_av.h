// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL audio/video layer: codec units (AV1 temporal units, Opus packets) as packets in the byte stream the PHY code words carry. No codec lives here.
//
// Packet: type 1 | stream 1 | flags 1 | pts (ms) 4 | length 3 | data | CRC-32 4 (over everything before it), big endian. 14 bytes of overhead.
// Types: 1 video (one AV1 temporal unit; the encoder repeats the sequence header in every key frame, so a receiver can join at any key frame),
//        2 audio (one Opus packet), 3 audio config (the OpusHead of the stream, sent at the start and about every second). Type 0 = padding to the end of
//        the code word. Flags: bit 0 = key frame.
// Code word payload: a 2-byte big-endian offset of the first packet that starts in it (0xFFFF = none), then stream bytes; packets run on from code word
// to code word. After a lost code word the receiver drops the packet in progress and resumes at the next offset.
#pragma once
#include "kavl/kavl_rx.h"
#include <cstdint>
#include <deque>
#include <vector>

namespace kavl::av {

enum : uint8_t { kVideo = 1, kAudio = 2, kAudioConfig = 3 };
constexpr size_t kOverhead = 14;
constexpr size_t kMaxPacket = 4u << 20;

struct Packet {
    uint8_t type = 0, stream = 0;
    bool key = false;
    uint32_t ptsMs = 0;
    std::vector<uint8_t> data;
};
std::vector<uint8_t> serialize(const Packet& p);

class Muxer {
public:
    explicit Muxer(const Modcod& m) : m_(m) {}
    void push(const Packet& p);
    // Bytes waiting, and whether a whole frame of them is ready.
    size_t pending() const { return q_.size(); }
    bool frameReady() const { return q_.size() >= static_cast<size_t>((m_.payloadBytes - 2) * m_.blocks); }
    // The next frame payload (framePayloadBytes); with flush the last bytes are padded.
    std::vector<uint8_t> takeFrame(bool flush = false);
private:
    const Modcod& m_;
    std::deque<uint8_t> q_;
    std::deque<size_t> starts_;   // stream positions of packet starts
    size_t pos_ = 0;              // stream position of q_.front()
};

class Demuxer {
public:
    struct Stats { long codeWords = 0, lost = 0, packets = 0, badPackets = 0, resyncs = 0; };
    void pushFrame(const RxFrame& f);
    std::vector<Packet> take();
    const Stats& stats() const { return st_; }
private:
    void codeWord(const uint8_t* d, size_t n);
    void parse();
    bool sync_ = false;
    int lastSeq_ = -1;
    std::vector<uint8_t> cur_;
    std::vector<Packet> out_;
    Stats st_;
};

}  // namespace kavl::av
