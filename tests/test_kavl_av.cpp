// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL audio/video layer: every packet that goes into the muxer comes out of the demuxer, for every modcod, with packets of every size and frames
// that are sometimes padded (the transmitter pads when nothing more is due). Padding that leaves fewer bytes than a packet header at the end of a
// code word must not swallow the next code word. Also: a lost code word costs only the packets that touch it.
#include "kavl/kavl_av.h"
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
    if (fails) {
        std::printf("test_kavl_av: %d failures\n", fails);
        return 1;
    }
    std::printf("test_kavl_av: all passed\n");
    return 0;
}
