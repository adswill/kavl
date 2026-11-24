// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL audio/video layer (kavl/include/kavl/kavl_av.h).
#include "kavl/kavl_av.h"
#include <algorithm>

namespace kavl::av {

std::vector<uint8_t> serialize(const Packet& p) {
    std::vector<uint8_t> b;
    const size_t n = std::min(p.data.size(), kMaxPacket);
    b.reserve(n + kOverhead);
    b.push_back(p.type);
    b.push_back(p.stream);
    b.push_back(p.key ? 1 : 0);
    for (int i = 3; i >= 0; i--) b.push_back(static_cast<uint8_t>(p.ptsMs >> (8 * i)));
    for (int i = 2; i >= 0; i--) b.push_back(static_cast<uint8_t>(n >> (8 * i)));
    b.insert(b.end(), p.data.begin(), p.data.begin() + static_cast<long>(n));
    const uint32_t c = crc32(b.data(), b.size());
    for (int i = 3; i >= 0; i--) b.push_back(static_cast<uint8_t>(c >> (8 * i)));
    return b;
}

void Muxer::push(const Packet& p) {
    const auto b = serialize(p);
    starts_.push_back(pos_ + q_.size());
    q_.insert(q_.end(), b.begin(), b.end());
}

std::vector<uint8_t> Muxer::takeFrame(bool flush) {
    std::vector<uint8_t> out(static_cast<size_t>(framePayloadBytes(m_)), 0);
    const size_t cap = static_cast<size_t>(m_.payloadBytes - 2);
    for (int c = 0; c < m_.blocks; c++) {
        uint8_t* cw = out.data() + static_cast<size_t>(c) * static_cast<size_t>(m_.payloadBytes);
        while (!starts_.empty() && starts_.front() < pos_) starts_.pop_front();
        const size_t take = std::min(cap, q_.size());
        if (take < cap && !flush) {   // not a whole code word: pad (only happens when the caller flushes or empties early)
        }
        uint16_t off = 0xFFFF;
        if (!starts_.empty() && starts_.front() < pos_ + take) off = static_cast<uint16_t>(starts_.front() - pos_);
        cw[0] = static_cast<uint8_t>(off >> 8);
        cw[1] = static_cast<uint8_t>(off);
        std::copy(q_.begin(), q_.begin() + static_cast<long>(take), cw + 2);   // the rest stays 0 = padding
        q_.erase(q_.begin(), q_.begin() + static_cast<long>(take));
        pos_ += take;
        if (take < cap) {   // padded: the next code word begins at a packet boundary (nothing is left, or the next packet starts there)
            while (!starts_.empty() && starts_.front() < pos_) starts_.pop_front();
        }
    }
    return out;
}

void Demuxer::pushFrame(const RxFrame& f) {
    if (!f.mc) return;
    if (lastSeq_ >= 0 && ((lastSeq_ + 1) & 0xFFFF) != f.hdr.seq) {   // frames missing
        sync_ = false;
        cur_.clear();
    }
    lastSeq_ = f.hdr.seq;
    for (const auto& b : f.blocks) {
        st_.codeWords++;
        if (!b.ok) {
            st_.lost++;
            sync_ = false;
            cur_.clear();
            continue;
        }
        codeWord(b.bytes.data(), b.bytes.size());
    }
}

void Demuxer::codeWord(const uint8_t* d, size_t n) {
    if (n < 2) return;
    const unsigned off = (static_cast<unsigned>(d[0]) << 8) | d[1];
    const uint8_t* s = d + 2;
    const size_t len = n - 2;
    if (!sync_) {
        if (off == 0xFFFF || off >= len) return;
        sync_ = true;
        st_.resyncs++;
        cur_.assign(s + off, s + len);
    } else {
        // a packet never starts with 0: what is left over and starts with 0 is the padding at the end of the last code word (shorter than a
        // packet header, so parse() could not tell it yet). Drop it, then start at this code word's first packet.
        if (!cur_.empty() && cur_[0] == 0) cur_.clear();
        if (cur_.empty() && off != 0xFFFF && off < len) cur_.assign(s + off, s + len);
        else if (cur_.empty() && off == 0xFFFF) return;   // no packet starts here and nothing is in progress: padding
        else cur_.insert(cur_.end(), s, s + len);
    }
    parse();
}

void Demuxer::parse() {
    size_t at = 0;
    while (cur_.size() - at >= 10) {
        const uint8_t* h = cur_.data() + at;
        if (h[0] == 0) {   // padding to the end of this code word: the next one starts with a packet
            at = cur_.size();
            break;
        }
        const size_t len = (static_cast<size_t>(h[7]) << 16) | (static_cast<size_t>(h[8]) << 8) | h[9];
        if (len > kMaxPacket || h[0] > kAudioConfig) {   // not a packet header: lost sync
            st_.badPackets++;
            sync_ = false;
            cur_.clear();
            return;
        }
        if (cur_.size() - at < len + kOverhead) break;
        const uint32_t c = (static_cast<uint32_t>(h[10 + len]) << 24) | (static_cast<uint32_t>(h[11 + len]) << 16) | (static_cast<uint32_t>(h[12 + len]) << 8) |
                           h[13 + len];
        if (c != crc32(h, 10 + len)) {
            st_.badPackets++;
            sync_ = false;
            cur_.clear();
            return;
        }
        Packet p;
        p.type = h[0];
        p.stream = h[1];
        p.key = (h[2] & 1) != 0;
        p.ptsMs = (static_cast<uint32_t>(h[3]) << 24) | (static_cast<uint32_t>(h[4]) << 16) | (static_cast<uint32_t>(h[5]) << 8) | h[6];
        p.data.assign(h + 10, h + 10 + len);
        out_.push_back(std::move(p));
        st_.packets++;
        at += len + kOverhead;
    }
    cur_.erase(cur_.begin(), cur_.begin() + static_cast<long>(at));
}

std::vector<Packet> Demuxer::take() {
    std::vector<Packet> o;
    o.swap(out_);
    return o;
}

}  // namespace kavl::av
