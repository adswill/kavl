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
// With erasure protection (kavl_fec.h, the header's fec field) each group of frames ends with parity frames; the receiver rebuilds up to that
// many lost frames (or the same number of lost code words) of each group before it reads the packets.
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
    // fecIdx: erasure protection profile (kavl_fec.h, 0 = none); the transmitter puts the same value in the frame header.
    explicit Muxer(const Modcod& m, int fecIdx = 0) : m_(m), fec_(fecIdx) {}
    void push(const Packet& p);
    // Bytes and code words waiting, and whether a whole frame of them is ready.
    size_t pending() const { return q_.size() + cwq_.size(); }
    bool frameReady();
    // The next frame payload (framePayloadBytes); padded data code words when not enough is waiting.
    std::vector<uint8_t> takeFrame(bool flush = false);
    int fec() const { return fec_; }
private:
    std::vector<uint8_t> dataCell(bool flush);
    void addData(std::vector<uint8_t> cw);
    void fill();
    const Modcod& m_;
    int fec_;
    std::deque<uint8_t> q_;
    std::deque<size_t> starts_;   // stream positions of packet starts
    size_t pos_ = 0;              // stream position of q_.front()
    std::deque<std::vector<uint8_t>> cwq_;      // code words ready to go out (data and parity)
    std::vector<std::vector<uint8_t>> group_;   // data code words of the open protection group
};

class Demuxer {
public:
    struct Stats { long codeWords = 0, lost = 0, recovered = 0, packets = 0, badPackets = 0, resyncs = 0; };
    void pushFrame(const RxFrame& f);
    void flush();   // the last protection group (end of a recording)
    std::vector<Packet> take();
    const Stats& stats() const { return st_; }
private:
    void codeWord(const uint8_t* d, size_t n);
    void parse();
    void finishGroup(bool atEnd = false);
    long group_ = -1;
    int lastPos_ = -1;   // the last place of the open group that arrived
    int groupFec_ = 0, groupBlocks_ = 1;
    size_t cellSize_ = 0;
    std::vector<std::vector<uint8_t>> cells_;
    bool sync_ = false;
    int lastSeq_ = -1;
    std::vector<uint8_t> cur_;
    std::vector<Packet> out_;
    Stats st_;
};

}  // namespace kavl::av
