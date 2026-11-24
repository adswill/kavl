// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL audio/video layer: every packet that goes into the muxer comes out of the demuxer, for every modcod, with packets of every size and frames
// that are sometimes padded (the transmitter pads when nothing more is due). Padding that leaves fewer bytes than a packet header at the end of a
// code word must not swallow the next code word. Also: a lost code word costs only the packets that touch it.
#include "kavl/kavl_av.h"
#include "kavl/kavl_fec.h"
#include <cstdio>

using namespace kavl;

static int fails = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
            fails++;                                                      \
        }                                                                 \
    } while (0)

static RxFrame toFrame(const Modcod& mc, const std::vector<uint8_t>& pay, int seq, int lostBlock = -1) {
    RxFrame f;
    f.mc = &mc;
    f.hdr.seq = seq;
    for (int b = 0; b < mc.blocks; b++) {
        RxBlock blk;
        blk.ok = b != lostBlock;
        blk.bytes.assign(pay.begin() + b * mc.payloadBytes, pay.begin() + (b + 1) * mc.payloadBytes);
        f.blocks.push_back(blk);
    }
    return f;
}

int main() {
    XorShift64 r{2026};
    for (int m = 0; m < kNumModcods; m++) {
        const Modcod& mc = *modcod(m);
        av::Muxer mux(mc);
        av::Demuxer dm;
        long pushed = 0, seq = 0;
        uint32_t pts = 0;
        for (int round = 0; round < 400; round++) {
            // a burst of packets of random sizes (1 .. 3 code words), then a frame that may be padded
            const int k = static_cast<int>(r.next() % 6);
            for (int i = 0; i < k; i++) {
                av::Packet p;
                p.type = (r.next() & 1) ? av::kVideo : av::kAudio;
                p.ptsMs = pts++;
                p.data.resize(1 + r.next() % static_cast<uint64_t>(3 * mc.payloadBytes));
                for (auto& b : p.data) b = static_cast<uint8_t>(r.next());
                mux.push(p);
                pushed++;
            }
            const auto pay = mux.takeFrame(!mux.frameReady());
            dm.pushFrame(toFrame(mc, pay, static_cast<int>(seq++ & 0xFFFF)));
        }
        while (mux.pending()) dm.pushFrame(toFrame(mc, mux.takeFrame(true), static_cast<int>(seq++ & 0xFFFF)));
        const long got = static_cast<long>(dm.take().size());
        std::printf("%-15s pushed %ld, demuxed %ld, bad %ld\n", mc.name, pushed, got, dm.stats().badPackets);
        CHECK(got == pushed);
        CHECK(dm.stats().badPackets == 0);
    }
    // padding of 1 .. 12 bytes at the very end of a frame, then a packet at the start of the next frame (it must not be taken for padding)
    for (int m = 0; m < kNumModcods; m++) {
        const Modcod& mc = *modcod(m);
        const long cap = static_cast<long>(mc.blocks) * (mc.payloadBytes - 2);
        for (int gap = 1; gap <= 12; gap++) {
            av::Muxer mux(mc);
            av::Demuxer dm;
            av::Packet a, b;
            a.type = av::kVideo;
            a.data.assign(static_cast<size_t>(cap - gap) - av::kOverhead, 7);
            b.type = av::kAudio;
            b.data.assign(100, 9);
            mux.push(a);
            dm.pushFrame(toFrame(mc, mux.takeFrame(true), 0));
            mux.push(b);
            dm.pushFrame(toFrame(mc, mux.takeFrame(true), 1));
            const auto out = dm.take();
            if (out.size() != 2) std::printf("%s, %d bytes of padding: %zu of 2 packets\n", mc.name, gap, out.size());
            CHECK(out.size() == 2);
        }
    }
    // a lost code word: the packets after it come back once the next packet start is seen
    {
        const Modcod& mc = *modcod(kMainModcod);
        av::Muxer mux(mc);
        av::Demuxer dm;
        for (int i = 0; i < 60; i++) {
            av::Packet p;
            p.type = av::kVideo;
            p.ptsMs = static_cast<uint32_t>(i);
            p.data.assign(1000, static_cast<uint8_t>(i));
            mux.push(p);
        }
        int seq = 0;
        while (mux.pending()) dm.pushFrame(toFrame(mc, mux.takeFrame(true), seq++, seq == 2 ? 1 : -1));
        const auto out = dm.take();
        std::printf("one code word lost: %zu of 60 packets back\n", out.size());
        CHECK(out.size() >= 50 && out.size() < 60);
    }
    // erasure protection, raw: any loss of up to P cells comes back exactly
    for (int idx = 1; idx < fec::kNumProfiles; idx++) {
        const fec::Profile fp = fec::profile(idx, 4);
        int bad = 0;
        for (int trial = 0; trial < 60; trial++) {
            std::vector<std::vector<uint8_t>> data(static_cast<size_t>(fp.data), std::vector<uint8_t>(97));
            for (auto& d : data)
                for (auto& b : d) b = static_cast<uint8_t>(r.next());
            auto cells = data;
            for (auto& p : fec::encode(fp, data)) cells.push_back(p);
            const int lose = 1 + static_cast<int>(r.next() % static_cast<uint64_t>(fp.parity));
            for (int k = 0; k < lose; k++) cells[static_cast<size_t>(r.next() % static_cast<uint64_t>(fp.n()))].clear();
            if (!fec::recover(fp, cells, 97)) bad++;
            for (int i = 0; i < fp.data; i++)
                if (cells[static_cast<size_t>(i)] != data[static_cast<size_t>(i)]) bad++;
        }
        std::printf("erasure %s: %d bad recoveries\n", fec::profileName(idx), bad);
        CHECK(bad == 0);
    }
    // erasure protection through the muxer and demuxer: up to P lost code words per group (and whole lost frames) cost nothing
    for (int m : {0, 2, 3, kMainModcod}) {
        for (int idx = 1; idx < fec::kNumProfiles; idx++) {
            const Modcod& mc = *modcod(m);
            const fec::Profile fp = fec::profile(idx, mc.blocks);
            av::Muxer mux(mc, idx);
            av::Demuxer dm;
            long pushed = 0;
            std::vector<std::vector<uint8_t>> frames;
            for (int i = 0; i < 300; i++) {
                av::Packet p;
                p.type = (i & 1) ? av::kVideo : av::kAudio;
                p.ptsMs = static_cast<uint32_t>(i);
                p.data.resize(1 + r.next() % static_cast<uint64_t>(2 * mc.payloadBytes));
                for (auto& b : p.data) b = static_cast<uint8_t>(r.next());
                mux.push(p);
                pushed++;
                while (mux.frameReady()) frames.push_back(mux.takeFrame());
            }
            for (int i = 0; i < 2 * fp.frames; i++) frames.push_back(mux.takeFrame(true));   // carry the last group and its parity out
            // lose up to P code words per group, in random places; sometimes a whole frame
            const long total = static_cast<long>(frames.size()) * mc.blocks;
            std::vector<char> lost(static_cast<size_t>(total), 0);
            for (long g0 = 0; g0 + fp.n() <= total; g0 += fp.n()) {   // complete groups only (the last one may not have its parity yet)
                if (r.next() & 1) {   // whole frames, as many as the group has parity frames
                    const int k = 1 + static_cast<int>(r.next() % static_cast<uint64_t>(fp.parityFrames));
                    for (int j = 0; j < k; j++) {
                        const long f0 = g0 + static_cast<long>(r.next() % static_cast<uint64_t>(fp.frames)) * mc.blocks;
                        for (int b = 0; b < mc.blocks; b++) lost[static_cast<size_t>(f0 + b)] = 1;
                    }
                } else {   // single code words
                    const int k = static_cast<int>(r.next() % static_cast<uint64_t>(fp.parity + 1));
                    for (int j = 0; j < k; j++) lost[static_cast<size_t>(g0 + static_cast<long>(r.next() % static_cast<uint64_t>(fp.n())))] = 1;
                }
            }
            for (size_t fi = 0; fi < frames.size(); fi++) {
                RxFrame f = toFrame(mc, frames[fi], static_cast<int>(fi & 0xFFFF));
                f.hdr.fec = idx;
                for (int b = 0; b < mc.blocks; b++)
                    if (lost[fi * static_cast<size_t>(mc.blocks) + static_cast<size_t>(b)]) f.blocks[static_cast<size_t>(b)].ok = false;
                dm.pushFrame(f);
            }
            dm.flush();
            const long got = static_cast<long>(dm.take().size());
            std::printf("%-15s erasure %-24s: pushed %ld, demuxed %ld, rebuilt %ld code words, lost %ld\n", mc.name, fec::profileName(idx), pushed, got,
                        dm.stats().recovered, dm.stats().lost);
            CHECK(got == pushed);
            CHECK(dm.stats().lost == 0);
        }
    }
    // more losses than parity in one group: only that group's packets go, the stream carries on
    {
        const Modcod& mc = *modcod(kMainModcod);
        av::Muxer mux(mc, 1);
        av::Demuxer dm;
        std::vector<std::vector<uint8_t>> frames;
        for (int i = 0; i < 400; i++) {
            av::Packet p;
            p.type = av::kVideo;
            p.ptsMs = static_cast<uint32_t>(i);
            p.data.assign(900, static_cast<uint8_t>(i));
            mux.push(p);
            while (mux.frameReady()) frames.push_back(mux.takeFrame());
        }
        for (int i = 0; i < 32; i++) frames.push_back(mux.takeFrame(true));
        for (size_t fi = 0; fi < frames.size(); fi++) {
            RxFrame f = toFrame(mc, frames[fi], static_cast<int>(fi), fi == 3 ? 0 : -1);
            f.hdr.fec = 1;
            if (fi == 3 || fi == 5) f.blocks[1].ok = f.blocks[2].ok = false;   // 2 frames hit in one group of 16 with 1 parity frame
            dm.pushFrame(f);
        }
        dm.flush();
        const size_t got = dm.take().size();
        std::printf("2 frames hit in one 16-frame group: %zu of 400 packets back\n", got);
        CHECK(got >= 360 && got < 400);
    }
    // a clock packet (type 4) among video packets comes back whole, with its wall clock, and costs no other packet
    {
        const Modcod& mc = *modcod(kMainModcod);
        av::Muxer mux(mc);
        av::Demuxer dm;
        const int64_t wall = 1795000000123LL;
        for (int i = 0; i < 30; i++) {
            av::Packet p;
            p.type = av::kVideo;
            p.ptsMs = static_cast<uint32_t>(i * 16);
            p.data.assign(200 + static_cast<size_t>(i) * 37, static_cast<uint8_t>(i));
            mux.push(p);
            if (i % 10 == 0) mux.push(av::clockPacket(p.ptsMs, wall + i));
        }
        int seq = 0;
        while (mux.pending()) dm.pushFrame(toFrame(mc, mux.takeFrame(true), seq++));
        int video = 0, clocks = 0;
        for (const auto& p : dm.take()) {
            int64_t w = 0;
            if (av::readClock(p, w)) {
                CHECK(w == wall + static_cast<int64_t>(p.ptsMs / 16));
                clocks++;
            } else if (p.type == av::kVideo) {
                video++;
            }
        }
        std::printf("clock packets: %d of 3 back, video %d of 30, bad %ld\n", clocks, video, dm.stats().badPackets);
        CHECK(clocks == 3 && video == 30 && dm.stats().badPackets == 0);
    }
    if (fails) {
        std::printf("test_kavl_av: %d failures\n", fails);
        return 1;
    }
    std::printf("test_kavl_av: all passed\n");
    return 0;
}
