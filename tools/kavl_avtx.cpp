// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// kavl_avtx: video + sound -> KAVL IQ .wav. Reads AV1 video as IVF and Opus sound as Ogg (both made by FFmpeg, e.g.
//   ffmpeg -i in.mkv -map 0:v -c copy -f ivf v.ivf && ffmpeg -i in.mkv -map 0:a -c copy -f ogg a.ogg),
// interleaves the codec units by time, packs them into frames (kavl_av.h) and writes the transmitted signal as a 2-channel 16-bit .wav at 4 samples per
// symbol, which SDR++ (File source) and kavl_decwav open.
//   kavl_avtx --video v.ivf [--audio a.ogg] [--profile N] [--fec F] [--modcod N] [--symrate R] [--snr DB] [--freq HZ] [--out FILE]
// --fec F: erasure protection across code words (kavl_fec.h: 1 = 30 + 2, 2 = 60 + 4, 3 = 28 + 4; 0 = none, the default).
// --profile N sets the symbol rate and the modcod of video profile N (kavl_phy.h, 0 .. 5; 3 = 1080p60 at 800 kHz); --symrate / --modcod override.
// Without --video it lists the profiles with their AV1 budgets.
#include "kavl/kavl_av.h"
#include "kavl/kavl_fec.h"
#include "kavl/kavl_media.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

void put32(FILE* f, uint32_t v) {
    const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
    std::fwrite(b, 1, 4, f);
}
void put16(FILE* f, uint16_t v) {
    const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    std::fwrite(b, 1, 2, f);
}

}  // namespace

int main(int argc, char** argv) {
    std::string video, audio, out;
    int mc = kavl::kMainModcod;
    int fecIdx = 0;
    double snr = 100.0, freq = 433000000.0, symRate = kavl::kDefaultSymRate;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        const char* v = argv[i + 1];
        if (k == "--video") video = v;
        else if (k == "--audio") audio = v;
        else if (k == "--modcod") mc = std::atoi(v);
        else if (k == "--snr") snr = std::atof(v);
        else if (k == "--symrate") symRate = std::atof(v);
        else if (k == "--fec") fecIdx = std::clamp(std::atoi(v), 0, kavl::fec::kNumProfiles - 1);
        else if (k == "--profile") {
            const kavl::VideoProfile& vp = kavl::videoProfile(std::atoi(v));
            symRate = vp.symRate;
            mc = vp.modcod;
        }
        else if (k == "--freq") freq = std::atof(v);
        else if (k == "--out") out = v;
        else {
            std::fprintf(stderr, "unknown option %s\n", k.c_str());
            return 2;
        }
    }
    const kavl::Modcod* m = kavl::modcod(mc);
    std::vector<kavl::av::Packet> vids, auds;
    std::vector<uint8_t> opusHead;
    if (video.empty()) {
        std::printf("profiles (AV1 budget = payload with 10%% headroom, less sound and overhead):\n");
        for (int i = 0; i < kavl::kNumVideoProfiles; i++) {
            const auto& vp = kavl::videoProfile(i);
            std::printf("  %d  %-26s %4dx%-4d %3d fps  %7.1f ksym/s  %-15s  payload %.3f Mbit/s  video budget %d kbit/s (%d with --fec 1)\n", i, vp.name, vp.width,
                        vp.height, vp.fps, vp.symRate / 1e3, kavl::modcod(vp.modcod)->name, kavl::netBitRate(*kavl::modcod(vp.modcod), vp.symRate) / 1e6, kavl::videoKbps(vp),
                        kavl::videoKbps(vp, 1));
        }
        return 0;
    }
    if (!m || !kavl::media::readIvf(video, vids)) {
        std::fprintf(stderr, "usage: kavl_avtx --video v.ivf [--audio a.ogg] [--modcod N] [--symrate R] [--snr DB] [--freq HZ] [--out FILE]\n");
        return 2;
    }
    if (!audio.empty() && !kavl::media::readOggOpus(audio, opusHead, auds)) {
        std::fprintf(stderr, "cannot read Ogg Opus %s\n", audio.c_str());
        return 1;
    }
    if (out.empty()) out = "kavl_av_" + std::to_string(static_cast<long long>(std::llround(freq))) + "Hz.wav";

    // interleave by time; the audio config goes in front and about every second. Frames are paced to real time: frame n (on air at n x frame time)
    // carries the units due by then plus a lead, and padding when there is nothing more, so the signal lasts as long as the programme (a receiver
    // plays it at the right speed and a looping player sees the whole of it).
    const double leadMs = 3000.0, frameMs = kavl::frameSeconds(symRate) * 1000.0;   // 3 s ahead: a protection group (up to 32 frames) must arrive whole
    kavl::av::Muxer mux(*m, fecIdx);
    size_t vi = 0, ai = 0;
    uint32_t nextCfg = 0;
    long vBytes = 0, aBytes = 0;
    std::vector<std::vector<uint8_t>> frames;
    for (long n = 0; vi < vids.size() || ai < auds.size() || mux.pending(); n++) {
        const double due = n * frameMs + leadMs;
        while ((vi < vids.size() || ai < auds.size()) && !mux.frameReady()) {
            const bool takeVideo = ai >= auds.size() || (vi < vids.size() && vids[vi].ptsMs <= auds[ai].ptsMs);
            const kavl::av::Packet& p = takeVideo ? vids[vi] : auds[ai];
            if (p.ptsMs > due) break;
            if (!opusHead.empty() && p.ptsMs >= nextCfg) {
                kavl::av::Packet c;
                c.type = kavl::av::kAudioConfig;
                c.ptsMs = p.ptsMs;
                c.data = opusHead;
                mux.push(c);
                nextCfg = p.ptsMs + 1000;
            }
            mux.push(p);
            (takeVideo ? vBytes : aBytes) += static_cast<long>(p.data.size());
            (takeVideo ? vi : ai)++;
        }
        frames.push_back(mux.takeFrame(!mux.frameReady()));
    }
    // a little idle time at the end so a looping player resyncs cleanly
    for (int i = 0; i < 4; i++) frames.push_back(std::vector<uint8_t>(static_cast<size_t>(kavl::framePayloadBytes(*m)), 0));

    kavl::Transmitter tx(mc);
    auto iq = tx.modulate(frames, fecIdx, 0, 0, 256, 256);
    if (snr < 99.0) {
        const double sd = std::sqrt(kavl::kSps / std::pow(10.0, snr / 10.0) / 2.0);
        kavl::XorShift64 nr{777};
        auto g = [&]() {
            const double u1 = std::max(1e-300, static_cast<double>(nr.next() >> 11) / 9007199254740992.0);
            const double u2 = static_cast<double>(nr.next() >> 11) / 9007199254740992.0;
            return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
        };
        for (auto& v : iq) v += kavl::cf(static_cast<float>(sd * g()), static_cast<float>(sd * g()));
    }
    const uint32_t rate = static_cast<uint32_t>(std::llround(kavl::kSps * symRate));
    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", out.c_str());
        return 1;
    }
    const uint32_t dataBytes = static_cast<uint32_t>(iq.size() * 4);
    std::fwrite("RIFF", 1, 4, f);
    put32(f, 36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, rate);
    put32(f, rate * 4);
    put16(f, 4);
    put16(f, 16);
    std::fwrite("data", 1, 4, f);
    put32(f, dataBytes);
    const float scale = 0.18f * 32767.f;
    std::vector<int16_t> s(iq.size() * 2);
    for (size_t i = 0; i < iq.size(); i++) {
        s[2 * i] = static_cast<int16_t>(std::lround(std::fmax(-32768.f, std::fmin(32767.f, iq[i].real() * scale))));
        s[2 * i + 1] = static_cast<int16_t>(std::lround(std::fmax(-32768.f, std::fmin(32767.f, iq[i].imag() * scale))));
    }
    std::fwrite(s.data(), 2, s.size(), f);
    std::fclose(f);
    const double dur = vids.empty() ? 0 : vids.back().ptsMs / 1000.0;
    std::printf("%s: %zu video units (%.2f Mbit/s), %zu audio packets (%.0f kbit/s), %zu frames of %s, %.1f s of signal, %u samples/s, %.0f kHz wide\n",
                out.c_str(), vids.size(), dur > 0 ? vBytes * 8.0 / dur / 1e6 : 0.0, auds.size(), dur > 0 ? aBytes * 8.0 / dur / 1e3 : 0.0, frames.size(), m->name,
                static_cast<double>(iq.size()) / rate, rate, kavl::occupiedHz(symRate) / 1e3);
    return 0;
}
