// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// kavl_decwav: decodes a KAVL IQ .wav (2 channels, 16-bit PCM or 32-bit float; any sample rate, e.g. an SDR++ baseband recording) and prints every frame.
//   kavl_decwav FILE.wav [--symrate R] [--ivf VIDEO.ivf] [--ogg SOUND.ogg]
// With --ivf / --ogg the video (AV1) and sound (Opus) carried by the frames are written out (kavl_av.h).
#include "kavl/kavl_av.h"
#include "kavl/kavl_rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void le32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; i++) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

int opusSamples(const std::vector<uint8_t>& pk) {   // RFC 6716 section 3.1
    if (pk.empty()) return 0;
    static const int silk[4] = {480, 960, 1920, 2880}, celt[4] = {120, 240, 480, 960};
    const int cfg = pk[0] >> 3, code = pk[0] & 3;
    const int fs = cfg < 12 ? silk[cfg % 4] : cfg < 16 ? ((cfg % 2) ? 960 : 480) : celt[cfg % 4];
    return fs * (code == 0 ? 1 : code < 3 ? 2 : (pk.size() > 1 ? (pk[1] & 0x3F) : 0));
}

// Ogg pages, one packet each (RFC 3533; CRC-32 polynomial 0x04C11DB7, not reflected).
struct OggWriter {
    FILE* f = nullptr;
    uint32_t seq = 0, serial = 0x4B41564Cu;
    uint64_t granule = 0;
    void page(const std::vector<uint8_t>& pk, int flags) {
        std::vector<uint8_t> h = {'O', 'g', 'g', 'S', 0, static_cast<uint8_t>(flags)};
        for (int i = 0; i < 8; i++) h.push_back(static_cast<uint8_t>(granule >> (8 * i)));
        le32(h, serial);
        le32(h, seq++);
        le32(h, 0);
        std::vector<uint8_t> lace;
        size_t n = pk.size();
        while (n >= 255) {
            lace.push_back(255);
            n -= 255;
        }
        lace.push_back(static_cast<uint8_t>(n));
        h.push_back(static_cast<uint8_t>(lace.size()));
        h.insert(h.end(), lace.begin(), lace.end());
        h.insert(h.end(), pk.begin(), pk.end());
        uint32_t crc = 0;
        for (uint8_t b : h) {
            crc ^= static_cast<uint32_t>(b) << 24;
            for (int k = 0; k < 8; k++) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
        for (int i = 0; i < 4; i++) h[22 + static_cast<size_t>(i)] = static_cast<uint8_t>(crc >> (8 * i));
        std::fwrite(h.data(), 1, h.size(), f);
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: kavl_decwav FILE.wav [--symrate R]\n");
        return 2;
    }
    double symRate = kavl::kDefaultSymRate;
    std::string ivfPath, oggPath;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        if (k == "--symrate") symRate = std::atof(argv[i + 1]);
        else if (k == "--ivf") ivfPath = argv[i + 1];
        else if (k == "--ogg") oggPath = argv[i + 1];
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    uint8_t hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) || std::memcmp(hdr + 8, "WAVE", 4)) {
        std::fprintf(stderr, "not a WAV file\n");
        return 1;
    }
    uint32_t rate = 0;
    uint16_t fmt = 0, ch = 0, bits = 0;
    long dataLeft = -1;
    for (;;) {
        uint8_t c[8];
        if (std::fread(c, 1, 8, f) != 8) break;
        const uint32_t len = c[4] | (c[5] << 8) | (c[6] << 16) | (static_cast<uint32_t>(c[7]) << 24);
        if (!std::memcmp(c, "fmt ", 4)) {
            std::vector<uint8_t> b(len);
            if (std::fread(b.data(), 1, len, f) != len) break;
            fmt = static_cast<uint16_t>(b[0] | (b[1] << 8));
            ch = static_cast<uint16_t>(b[2] | (b[3] << 8));
            rate = b[4] | (b[5] << 8) | (b[6] << 16) | (static_cast<uint32_t>(b[7]) << 24);
            bits = static_cast<uint16_t>(b[14] | (b[15] << 8));
            if (fmt == 0xFFFE && len >= 26) fmt = static_cast<uint16_t>(b[24] | (b[25] << 8));   // WAVE_FORMAT_EXTENSIBLE
        } else if (!std::memcmp(c, "data", 4)) {
            dataLeft = len;
            break;
        } else {
            std::fseek(f, len + (len & 1), SEEK_CUR);
        }
    }
    if (dataLeft < 0 || ch != 2 || !((fmt == 1 && bits == 16) || (fmt == 3 && bits == 32))) {
        std::fprintf(stderr, "need a 2-channel 16-bit PCM or 32-bit float WAV (got format %u, %u channels, %u bits)\n", fmt, ch, bits);
        return 1;
    }
    std::printf("%s: %u samples/s, %s\n", argv[1], rate, fmt == 1 ? "16-bit" : "float");
    kavl::Receiver::Options o;
    o.symRate = symRate;
    o.inRate = rate;
    kavl::Receiver rx(o);
    const size_t chunk = 1 << 16, bps = fmt == 1 ? 4 : 8;
    std::vector<uint8_t> raw(chunk * bps);
    std::vector<kavl::cf> iq;
    long frames = 0, cw = 0, cwOk = 0;
    kavl::av::Demuxer demux;
    std::vector<uint8_t> ivfData;
    uint32_t ivfFrames = 0;
    long vUnits = 0, aPackets = 0;
    OggWriter ogg;
    bool oggStarted = false;
    std::vector<uint8_t> oggPending;
    if (!oggPath.empty()) ogg.f = std::fopen(oggPath.c_str(), "wb");
    auto avOut = [&]() {
        for (auto& p : demux.take()) {
            if (p.type == kavl::av::kVideo) {
                vUnits++;
                if (!ivfPath.empty()) {
                    le32(ivfData, static_cast<uint32_t>(p.data.size()));
                    le32(ivfData, p.ptsMs);
                    le32(ivfData, 0);
                    ivfData.insert(ivfData.end(), p.data.begin(), p.data.end());
                    ivfFrames++;
                }
            } else if (p.type == kavl::av::kAudioConfig && ogg.f && !oggStarted) {
                ogg.page(p.data, 2);   // beginning of stream: OpusHead
                ogg.page(std::vector<uint8_t>{'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 0, 0, 0, 0, 0, 0, 0, 0}, 0);
                ogg.granule = static_cast<uint64_t>(p.data.size() >= 12 ? (p.data[10] | (p.data[11] << 8)) : 0);
                oggStarted = true;
            } else if (p.type == kavl::av::kAudio) {
                aPackets++;
                if (oggStarted) {   // written one packet late, so the last one can carry the end-of-stream flag
                    if (!oggPending.empty()) ogg.page(oggPending, 0);
                    ogg.granule += static_cast<uint64_t>(opusSamples(p.data));
                    oggPending = p.data;
                }
            }
        }
    };
    auto report = [&](const std::vector<kavl::RxFrame>& v) {
        for (const auto& fr : v) {
            demux.pushFrame(fr);
            int ok = 0;
            for (const auto& b : fr.blocks) ok += b.ok;
            frames++;
            cw += static_cast<long>(fr.blocks.size());
            cwOk += ok;
            std::printf("frame seq %5d  %-15s  code words %d/%zu  Es/N0 %5.1f dB  MER %5.1f dB  offset %8.0f Hz  clock %6.1f ppm  at %.3f s\n", fr.hdr.seq,
                        fr.mc ? fr.mc->name : "?", ok, fr.blocks.size(), fr.esn0Db, fr.merDb, fr.cfoHz, fr.clockPpm, fr.startSample / rate);
        }
        avOut();
    };
    while (dataLeft > 0) {
        const size_t want = std::min(raw.size(), static_cast<size_t>(dataLeft));
        const size_t got = std::fread(raw.data(), 1, want, f);
        if (got == 0) break;
        dataLeft -= static_cast<long>(got);
        const size_t n = got / bps;
        iq.resize(n);
        for (size_t i = 0; i < n; i++) {
            if (fmt == 1) {
                const int16_t a = static_cast<int16_t>(raw[4 * i] | (raw[4 * i + 1] << 8)), b = static_cast<int16_t>(raw[4 * i + 2] | (raw[4 * i + 3] << 8));
                iq[i] = kavl::cf(a / 32768.f, b / 32768.f);
            } else {
                float a, b;
                std::memcpy(&a, &raw[8 * i], 4);
                std::memcpy(&b, &raw[8 * i + 4], 4);
                iq[i] = kavl::cf(a, b);
            }
        }
        report(rx.push(iq.data(), iq.size()));
    }
    const std::vector<kavl::cf> tail(static_cast<size_t>(rate / 10), kavl::cf(0, 0));
    report(rx.push(tail.data(), tail.size()));
    demux.flush();
    avOut();
    std::printf("%ld frames, %ld/%ld code words decoded\n", frames, cwOk, cw);
    const auto& ds = demux.stats();
    std::printf("audio/video: %ld video units, %ld audio packets, %ld bad packets, %ld resyncs, %ld code words rebuilt, %ld lost\n", vUnits, aPackets,
                ds.badPackets, ds.resyncs, ds.recovered, ds.lost);
    if (!ivfPath.empty()) {
        std::vector<uint8_t> h = {'D', 'K', 'I', 'F', 0, 0, 32, 0, 'A', 'V', '0', '1'};
        h.push_back(0x00); h.push_back(0x0A); h.push_back(0xA0); h.push_back(0x05);   // 2560 x 1440
        le32(h, 1000);   // time base 1/1000 s: the timestamps are milliseconds
        le32(h, 1);
        le32(h, ivfFrames);
        le32(h, 0);
        if (FILE* vf = std::fopen(ivfPath.c_str(), "wb")) {
            std::fwrite(h.data(), 1, h.size(), vf);
            std::fwrite(ivfData.data(), 1, ivfData.size(), vf);
            std::fclose(vf);
        }
    }
    if (ogg.f) {
        if (oggStarted && !oggPending.empty()) ogg.page(oggPending, 4);   // the last packet, with the end-of-stream flag
        std::fclose(ogg.f);
    }
    return frames > 0 && cwOk == cw ? 0 : 1;
}
