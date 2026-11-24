// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL media files (kavl/include/kavl/kavl_media.h).
#include "kavl/kavl_media.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace kavl::media {

static std::vector<uint8_t> readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}
static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

// IVF: 32-byte header (time base rate/scale at 16/20), then per frame 4-byte size, 8-byte timestamp, data.
bool readIvf(const std::string& path, std::vector<av::Packet>& out) {
    const auto d = readFile(path);
    if (d.size() < 32 || std::memcmp(d.data(), "DKIF", 4)) return false;
    const uint32_t rate = le32(&d[16]), scale = le32(&d[20]);
    size_t pos = d[6] | (d[7] << 8);
    while (pos + 12 <= d.size()) {
        const uint32_t sz = le32(&d[pos]);
        const uint64_t ts = le32(&d[pos + 4]) | (static_cast<uint64_t>(le32(&d[pos + 8])) << 32);
        pos += 12;
        if (pos + sz > d.size()) break;
        av::Packet p;
        p.type = av::kVideo;
        p.data.assign(d.begin() + static_cast<long>(pos), d.begin() + static_cast<long>(pos + sz));
        p.ptsMs = static_cast<uint32_t>(rate ? ts * 1000ull * scale / rate : 0);
        // key frame = carries a sequence header OBU (type 1); the encoder repeats it in every key frame
        for (size_t i = 0; i < p.data.size();) {
            const uint8_t h = p.data[i];
            const int type = (h >> 3) & 15, ext = (h >> 2) & 1, hasSize = (h >> 1) & 1;
            i += 1 + static_cast<size_t>(ext);
            uint64_t len = p.data.size() - i;
            if (hasSize) {
                len = 0;
                for (int s = 0; i < p.data.size(); s += 7) {
                    const uint8_t b = p.data[i++];
                    len |= static_cast<uint64_t>(b & 0x7F) << s;
                    if (!(b & 0x80)) break;
                }
            }
            if (type == 1) p.key = true;
            i += static_cast<size_t>(len);
        }
        out.push_back(std::move(p));
        pos += sz;
    }
    return true;
}

// Opus packet duration in 48 kHz samples from its TOC byte (RFC 6716 section 3.1).
int opusSamples(const std::vector<uint8_t>& pk) {
    if (pk.empty()) return 0;
    const int cfg = pk[0] >> 3, code = pk[0] & 3;
    static const int silk[4] = {480, 960, 1920, 2880}, celt[4] = {120, 240, 480, 960};
    int fs;   // samples of one frame at 48 kHz
    if (cfg < 12) fs = silk[cfg % 4];
    else if (cfg < 16) fs = (cfg % 2) ? 960 : 480;
    else fs = celt[cfg % 4];
    const int frames = code == 0 ? 1 : code < 3 ? 2 : (pk.size() > 1 ? (pk[1] & 0x3F) : 0);
    return fs * frames;
}

// Ogg Opus: pages -> packets; the first is OpusHead, the second OpusTags, then audio.
bool readOggOpus(const std::string& path, std::vector<uint8_t>& head, std::vector<av::Packet>& out) {
    const auto d = readFile(path);
    size_t pos = 0;
    std::vector<uint8_t> pk;
    std::vector<std::vector<uint8_t>> packets;
    while (pos + 27 <= d.size() && !std::memcmp(&d[pos], "OggS", 4)) {
        const int nseg = d[pos + 26];
        size_t data = pos + 27 + static_cast<size_t>(nseg);
        for (int s = 0; s < nseg; s++) {
            const int l = d[pos + 27 + static_cast<size_t>(s)];
            if (data + static_cast<size_t>(l) > d.size()) return false;
            pk.insert(pk.end(), d.begin() + static_cast<long>(data), d.begin() + static_cast<long>(data) + l);
            data += static_cast<size_t>(l);
            if (l < 255) {
                packets.push_back(pk);
                pk.clear();
            }
        }
        pos = data;
    }
    if (packets.size() < 2 || packets[0].size() < 19 || std::memcmp(packets[0].data(), "OpusHead", 8)) return false;
    head = packets[0];
    const int preSkip = head[10] | (head[11] << 8);
    long long samples = -preSkip;
    for (size_t i = 2; i < packets.size(); i++) {
        av::Packet p;
        p.type = av::kAudio;
        p.stream = 0;
        p.ptsMs = static_cast<uint32_t>(std::max(0LL, samples) / 48);
        samples += opusSamples(packets[i]);
        p.data = std::move(packets[i]);
        out.push_back(std::move(p));
    }
    return true;
}


}  // namespace kavl::media
