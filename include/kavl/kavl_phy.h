// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL, the narrow-band digital video mode: physical layer, transmit side and the shared frame layout.
//
// Goal (owner, 2026-11-23): one 1440p60 video stream with 48 kHz stereo audio in as close to 400 kHz as possible (at most 500 kHz) at an Es/N0 of 20 dB
// or less, at the picture quality of the 500 kHz design (about 2.4 Mbit/s of payload).
//
// One frame = preamble (64 pseudo-random QPSK symbols) + header (64 info bits incl. CRC-16, K = 7 rate 1/2 convolutional code, BPSK turned by a quarter
// turn per symbol, 140 symbols) + data region (32400 data symbols with a known pseudo-random QPSK pilot in front of every 32 data symbols and one after the last = 33414 symbols). The 32400 data symbols hold exactly
// bitsPerSymbol / 2 LDPC code words of 64800 bits (ATSC 3.0 A/322 codes): QPSK 1, 16 points 2, 64 points 3, 256 points 4, 1024 points 5. The constellations
// are the A/322 non-uniform ones of the code rate (QPSK uniform). The coded bits of the frame are permuted together (one pseudo-random permutation per frame
// size) and scrambled. Each code word carries payloadBytes + CRC-32 (+ up to 7 zero bits).
//
// Single carrier, root-raised-cosine roll-off 0.02; at the default 392 ksym/s the signal occupies 400 kHz. The IQ is produced at 4 samples per symbol; the
// receiver accepts any input rate (it resamples to 4 samples per symbol).
#pragma once
#include "kavl/kavl_common.h"
#include <string>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kavl {

constexpr int kSps = 4;                       // samples per symbol of the IQ stream
constexpr double kDefaultSymRate = 392000.0;  // symbols per second (400 kHz occupied)
constexpr float kRrcBeta = 0.02f;
constexpr int kRrcSpan = 80;                  // symbols (a small roll-off needs a long filter)
constexpr int kLdpcN = 64800;
constexpr int kDataSyms = 32400;
constexpr int kPilotGroup = 32;               // data symbols per pilot
constexpr int kPilots = (kDataSyms + kPilotGroup - 1) / kPilotGroup + 1;   // 1014
constexpr int kRegionSyms = kDataSyms + kPilots;                          // 33414
constexpr int kPreambleSyms = 64;
constexpr int kHeaderInfoBits = 64;
constexpr int kHeaderCodedBits = (kHeaderInfoBits + 6) * 2;               // 140
constexpr int kHeaderStart = kPreambleSyms;                              // symbol index of the first header symbol
constexpr int kRegionStart = kPreambleSyms + kHeaderCodedBits;           // 204
constexpr int kFrameSyms = kRegionStart + kRegionSyms;                   // 33618
constexpr int kNumModcods = 13;
constexpr int kMainModcod = 3;                                            // NUC-256 10/15: 2.01 Mbit/s at 392 ksym/s (11/15: 19.5 dB measured, 2.22 Mbit/s)
// Profiles (the receiver must know the symbol rate; the header carries the modcod): 1440p60 = 392 ksym/s (400 kHz), 720p60 = 196 ksym/s (200 kHz).
constexpr double kSymRate1440 = 392000.0, kSymRate720 = 196000.0;

// Video profiles: picture size, frame rate, symbol rate (the width is 1.02 x the symbol rate) and modcod. The NUC-256 10/15 ones need about 18 dB,
// the robust NUC-64 10/15 ones 13.5 dB (measured). The AV1 budgets match the picture quality (VMAF) of the first 1440p60 demo at 13 % fewer bits
// (SVT-AV1 preset 2, key frame every 4 s; kavl/video/codec_search.sh). videoKbps() is the AV1 budget: the payload rate with 10 % headroom for the
// variable bit rate, less the profile's sound and packet overhead (112 + 18 kbit/s on the wide profiles).
struct VideoProfile {
    const char* name;
    int width, height, fps;
    double symRate;
    int modcod;
    // the sound and the packet overhead the video budget leaves room for, and how often a key frame comes
    int soundKbps = 112;     // Opus
    int soundChannels = 2;
    int soundFrameMs = 20;   // Opus frame (and Ogg page) length; 60 ms on the narrow profiles (fewer packets, less overhead)
    int overheadKbps = 18;   // packet headers (14 bytes each), the sound config and the clock
    int keySec = 1;          // a key frame this often live (twice as long in an encoded file)
};
constexpr int kNumVideoProfiles = 14;
const VideoProfile& videoProfile(int i);   // 0 .. kNumVideoProfiles - 1 (clamped)
// fecIdx: erasure protection (kavl_fec.h) takes its parity share off the budget.
int videoKbps(const VideoProfile& p, int fecIdx = 0);

struct Modcod {
    int id;
    const char* name;
    int bitsPerSym;
    int rate15;        // LDPC rate = rate15 / 15
    int blocks;        // code words per frame = bitsPerSym / 2
    int kBits;         // information bits of one code word
    int payloadBytes;  // payload bytes of one code word (without the CRC-32)
};
// 0 QPSK 8/15, 1 NUC-16 9/15, 2 NUC-64 10/15, 3 NUC-256 10/15, 4 NUC-256 11/15, 5 NUC-256 12/15, 6 NUC-256 13/15, 7 NUC-1024 10/15, 8 NUC-1024 11/15,
// 9 NUC-1024 12/15. nullptr for a bad id.
const Modcod* modcod(int id);
// The constellation of a modcod: 2^bitsPerSym points, unit average power, indexed by the label (first transmitted bit = most significant).
const std::vector<cf>& constellation(const Modcod& m);
// Bits (one per byte, first transmitted first) -> symbols; nBits a multiple of bitsPerSym.
std::vector<cf> mapBits(const Modcod& m, const uint8_t* bits, size_t nBits);
// Max-log LLRs (positive = bit 0), n x bitsPerSym values in the order of mapBits; n0 = complex noise variance per symbol.
void demapSoft(const Modcod& m, const cf* sym, size_t n, float n0, float* llr);
inline int framePayloadBytes(const Modcod& m) { return m.payloadBytes * m.blocks; }
inline double frameSeconds(double symRate) { return kFrameSyms / symRate; }
// Payload bits per second of the PHY (after LDPC, CRC, preamble, header and pilots).
inline double netBitRate(const Modcod& m, double symRate) { return framePayloadBytes(m) * 8.0 * symRate / kFrameSyms; }
// Occupied bandwidth in Hz.
inline double occupiedHz(double symRate) { return symRate * (1.0 + kRrcBeta); }

struct Header {
    int modcod = 0;     // 0..15 (0..7 defined)
    int fec = 0;        // erasure FEC group profile of the container layer, 3 bits
    int layer = 0;      // 0 = single layer (2 bits; reserved for the layered mode)
    int seq = 0;        // frame sequence number, 16 bits, wraps
    int linkQ = 0;      // reported Es/N0 in 0.25 dB steps (8 bits), 0 = unknown
    int id = 0;         // station ID slice, 13 bits (0 = none): bit 12 set, then two 6-bit characters of the callsign (stationIdSlice)
};
// 64 information bits (one per byte), MSB first: version 2 (=0), modcod 4, fec 3, layer 2, seq 16, linkQ 8, id 13, CRC-16 over the first 48.
// (The id bits were reserved as 0 before, so older transmitters simply send no ID.)
std::vector<uint8_t> headerBits(const Header& h);
bool parseHeader(const uint8_t* bits64, Header& h);   // false on a CRC or version failure
// Station ID in the headers: the callsign (up to kStationIdChars of A-Z, 0-9, / and -; lower case is raised; other characters dropped) goes
// two characters per frame, the pair chosen by seq % 8 (65536 frames are a whole number of rounds, so the wrap keeps the order). A reader has
// it once it holds the pairs from the first up to the one with the end: 3 frames for "M0ABC" once pair 0 came. No payload is used, and it
// decodes whenever the headers do.
constexpr int kStationIdChars = 16;
int stationIdSlice(const std::string& callsign, int seq);   // the header's id for this frame; 0 for an empty callsign
class StationIdReader {
public:
    void push(const Header& h);              // every received header, in order
    std::string callsign() const;            // once the pairs up to its end have come (and since then), else ""
    void reset();
private:
    int slice_[8] = {};
    int have_ = 0;                           // bit per slice seen
};
std::vector<cf> headerSymbols(const Header& h);       // 140 symbols: BPSK x j^i (a quarter turn per symbol, so the header looks like any other signal to the
                                                      // receiver's blind IQ-imbalance estimate)

// 64 pseudo-random QPSK symbols, unit magnitude. Not a Zadoff-Chu sequence: the symbol-to-symbol phase steps of a Zadoff-Chu sequence form a pure tone, so
// the receiver's offset-blind differential correlation would peak at every shift (each read as another carrier offset).
const std::vector<cf>& preambleSymbols();
cf pilotSymbol(int i);                                // pseudo-random QPSK, unit magnitude, i = 0 .. kPilots - 1

// Data region layout: the region index of every pilot and data symbol.
struct RegionMap {
    std::vector<int> pilot;   // kPilots entries
    std::vector<int> data;    // kDataSyms entries, in order
};
const RegionMap& regionMap();

// Frame bit permutation + scrambling (nBits = blocks x 16200). Code order -> transmitted order.
void interleaveScramble(const uint8_t* in, uint8_t* out, int nBits);
// LLRs in transmitted order -> code order (descrambling flips the sign).
void deinterleaveDescrambleLlr(const float* inTx, float* outCode, int nBits);

// Coded bits of a frame in transmitted order: payload = framePayloadBytes(m) bytes.
std::vector<uint8_t> frameCodedBits(const Modcod& m, const uint8_t* payload);
// Symbols of a whole frame (kFrameSyms).
std::vector<cf> frameSymbols(const Modcod& m, const Header& h, const uint8_t* payload);
// Symbols of a frame from already mapped data symbols (kDataSyms).
std::vector<cf> assembleFrame(const Header& h, const std::vector<cf>& dataSyms);

// Root-raised-cosine taps (unit energy) of the transmitter and the receiver's matched filter.
const std::vector<float>& rrc();

class Transmitter {
public:
    explicit Transmitter(int modcodId);
    const Modcod& modcod() const { return *m_; }
    // IQ of the frames back to back (4 samples per symbol, data power 1 per sample): leadSyms of silence, the frames, tailSyms of silence.
    // frames[i] = framePayloadBytes bytes; sequence numbers count from startSeq.
    std::vector<cf> modulate(const std::vector<std::vector<uint8_t>>& frames, int fec = 0, int linkQ = 0, int startSeq = 0, int leadSyms = 32,
                             int tailSyms = 32) const;
    // Pulse shaping of a symbol stream (4 samples per symbol, data power 1 per sample).
    static std::vector<cf> shape(const std::vector<cf>& syms);
private:
    const Modcod* m_;
};

}  // namespace kavl
