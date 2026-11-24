// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL physical layer: frame layout, header, interleaver, clean loopback of every modcod, a link with RTL-SDR impairments, no false frames on noise.
#include "kavl_test_util.h"
#include <cstdlib>
#include <cstring>

using namespace kavl;
using namespace kavltest;

static int fails = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
            fails++;                                                      \
        }                                                                 \
    } while (0)

// The soft demapper (a min tree over the labels) gives exactly the LLRs of the direct max-log search, for every modcod.
static void testDemapExact() {
    kavl::XorShift64 r{2026};
    for (int id = 0; id < kNumModcods; id++) {
        const Modcod& m = *modcod(id);
        const auto& pts = constellation(m);
        const int eta = m.bitsPerSym, M = 1 << eta;
        const size_t n = 2000;
        std::vector<cf> sym(n);
        for (auto& s : sym) {
            const double u1 = (r.next() >> 11) * (1.0 / 9007199254740992.0), u2 = (r.next() >> 11) * (1.0 / 9007199254740992.0);
            s = pts[r.next() % pts.size()] + cf(static_cast<float>(0.3 * (u1 - 0.5)), static_cast<float>(0.3 * (u2 - 0.5)));
        }
        std::vector<float> got(n * static_cast<size_t>(eta));
        demapSoft(m, sym.data(), n, 0.02f, got.data());
        long diff = 0;
        for (size_t s = 0; s < n; s++)
            for (int k = 0; k < eta; k++) {
                float m0 = 1e30f, m1 = 1e30f;
                for (int i = 0; i < M; i++) {
                    const float a = sym[s].real() - pts[static_cast<size_t>(i)].real(), b = sym[s].imag() - pts[static_cast<size_t>(i)].imag();
                    const float d = a * a + b * b;
                    if ((i >> (eta - 1 - k)) & 1) m1 = std::min(m1, d);
                    else m0 = std::min(m0, d);
                }
                if (got[s * static_cast<size_t>(eta) + static_cast<size_t>(k)] != (m1 - m0) * (1.f / 0.02f)) diff++;
            }
        CHECK(diff == 0);
    }
}

// The profiles: each on its mode, never above 1 MHz, with about twice the first 0 dB family's video (4K60: what fits in 1 MHz).
static void testBudgets() {
    const int want[kNumVideoProfiles] = {44, 110, 284, 482, 774, 1170};
    const int mc[kNumVideoProfiles] = {12, 12, 12, 12, 0, 10};
    for (int p = 0; p < kNumVideoProfiles; p++) {
        const auto& vp = videoProfile(p);
        const int k = videoKbps(vp);
        std::printf("profile %d %-18s %4d kbit/s video, %4.0f kHz, %s\n", p, vp.name, k, occupiedHz(vp.symRate) / 1e3, modcod(vp.modcod)->name);
        CHECK(vp.modcod == mc[p]);
        CHECK(occupiedHz(vp.symRate) <= 1e6 + 0.5);
        CHECK(std::abs(k - want[p]) <= want[p] / 10 + 2);
    }
}

static void testLayout() {
    CHECK(kRegionSyms == static_cast<int>(regionMap().pilot.size() + regionMap().data.size()));
    CHECK(static_cast<int>(regionMap().pilot.size()) == kPilots);
    CHECK(occupiedHz(kDefaultSymRate) <= 400000.0);
    for (int i = 0; i < kNumModcods; i++) {
        const Modcod* m = modcod(i);
        CHECK(m != nullptr);
        CHECK(m->blocks * kLdpcN == kDataSyms * m->bitsPerSym);
        CHECK(m->payloadBytes * 8 + 32 <= m->kBits);
        std::printf("modcod %d %-15s %d code words, %5d bytes/frame, %.3f Mbit/s at %.0f ksym/s (%.0f kHz)\n", i, m->name, m->blocks, framePayloadBytes(*m),
                    netBitRate(*m, kDefaultSymRate) / 1e6, kDefaultSymRate / 1e3, occupiedHz(kDefaultSymRate) / 1e3);
    }
    CHECK(modcod(kNumModcods) == nullptr);
}

static void testHeader() {
    Header h;
    h.modcod = 9;
    h.fec = 5;
    h.layer = 1;
    h.seq = 0xBEEF;
    h.linkQ = 77;
    auto bits = headerBits(h);
    CHECK(bits.size() == 64u);
    Header g;
    CHECK(parseHeader(bits.data(), g));
    CHECK(g.modcod == 9 && g.fec == 5 && g.layer == 1 && g.seq == 0xBEEF && g.linkQ == 77);
    bits[20] ^= 1;
    CHECK(!parseHeader(bits.data(), g));
    // the station ID: two characters a frame, through real headers, across the wrap of seq (65533 .. 65540 = slots 5 6 7 0 1 2 3 4)
    StationIdReader rd;
    for (int s = 65533; s < 65533 + 8; s++) {
        Header f;
        f.seq = s & 0xFFFF;
        f.id = stationIdSlice("ea8/ly2xyz-9 abcdef", f.seq);   // the space dropped, lower case raised, 16 characters kept
        auto fb = headerBits(f);
        Header back;
        CHECK(parseHeader(fb.data(), back) && back.id == f.id);
        CHECK(rd.callsign().empty());
        rd.push(back);
    }
    std::printf("station ID: %s\n", rd.callsign().c_str());
    CHECK(rd.callsign() == "EA8/LY2XYZ-9ABCD");
    // a short one is whole after pairs 0-2 (the end is in pair 2)
    StationIdReader r2;
    for (int s = 0; s < 3; s++) {
        Header f;
        f.seq = s;
        f.id = stationIdSlice("M0ABC", s);
        r2.push(f);
    }
    CHECK(r2.callsign() == "M0ABC");
    CHECK(stationIdSlice("", 3) == 0);
    CHECK(headerSymbols(h).size() == static_cast<size_t>(kHeaderSyms));
}

static void testInterleaver() {
    Rng r(3);
    for (int b = 1; b <= 5; b++) {
        const int n = b * kLdpcN;
        std::vector<uint8_t> in(static_cast<size_t>(n)), tx(static_cast<size_t>(n));
        for (auto& v : in) v = static_cast<uint8_t>(r.below(2));
        interleaveScramble(in.data(), tx.data(), n);
        std::vector<float> l(static_cast<size_t>(n)), back(static_cast<size_t>(n));
        for (int i = 0; i < n; i++) l[static_cast<size_t>(i)] = tx[static_cast<size_t>(i)] ? -1.f : 1.f;
        deinterleaveDescrambleLlr(l.data(), back.data(), n);
        int bad = 0;
        for (int i = 0; i < n; i++) bad += ((back[static_cast<size_t>(i)] < 0) != (in[static_cast<size_t>(i)] != 0));
        CHECK(bad == 0);
    }
}

static void testClean() {
    for (int m = 0; m < kNumModcods; m++) {
        Channel c;   // no noise, no offsets
        const auto res = runLink(m, 2, c, 10 + static_cast<uint64_t>(m));
        std::printf("clean %-15s frames %d/%d code words %d/%d\n", modcod(m)->name, res.framesFound, res.framesSent, res.blocksOk, res.blocks);
        CHECK(res.framesFound == 2);
        CHECK(res.blocksOk == res.blocks);
    }
}

static void testRtlLink() {
    // the 1440p60 setting (NUC-256 12/15) well above its threshold through an RTL-SDR-like receiver at 2.4 Msps
    const auto c = rtlChannel(26.0);
    const auto res = runLink(kMainModcod, 3, c, 42);
    std::printf("rtl NUC-256 12/15 at 26 dB: frames %d/%d code words %d/%d, Es/N0 est %.1f dB, MER %.1f dB, offset %.0f Hz, clock %.1f ppm\n", res.framesFound,
                res.framesSent, res.blocksOk, res.blocks, res.esn0Est, res.merDb, res.cfoHz, res.clockPpm);
    CHECK(res.framesFound == 3);
    CHECK(res.blocksOk == res.blocks);
    CHECK(std::fabs(res.cfoHz - c.cfoHz) < 200.0);
    CHECK(std::fabs(res.clockPpm - c.clockPpm) < 10.0);
    // QPSK at a large offset and a narrower width
    auto c2 = rtlChannel(8.0, 380000.0);
    c2.cfoHz = -61000.0;
    const auto r2 = runLink(0, 3, c2, 43);
    std::printf("rtl QPSK 8/15 at 8 dB, 380 ksym/s, -61 kHz: frames %d/%d code words %d/%d, offset %.0f Hz\n", r2.framesFound, r2.framesSent, r2.blocksOk, r2.blocks,
                r2.cfoHz);
    CHECK(r2.framesFound == 3);
    CHECK(r2.blocksOk == r2.blocks);
    // a 12 kHz signal (NUC-64 10/15) through the same receiver: 2.4 Msps is 200 times its symbol rate, 3 kHz off
    auto c3 = rtlChannel(16.0, 12000.0 / 1.02);
    c3.cfoHz = 3000.0;
    c3.phaseNoiseDeg = 0.05 * std::sqrt(392000.0 / c3.symRate);   // the same oscillators: more phase wander per (longer) symbol
    const auto r3 = runLink(2, 2, c3, 44);
    std::printf("rtl 12 kHz NUC-64 10/15 at 16 dB, 3 kHz off: frames %d/%d code words %d/%d, MER %.1f dB, offset %.0f Hz\n", r3.framesFound, r3.framesSent,
                r3.blocksOk, r3.blocks, r3.merDb, r3.cfoHz);
    CHECK(r3.framesFound == 2);
    CHECK(r3.blocksOk == r3.blocks);
}

// Weak signal: QPSK 6/15 (the 0 dB family) at 1 dB on the noise channel. Before the coherent preamble search and the header's offset retry,
// a quarter of the frames were lost at this level (their headers failed: the preamble's offset estimate drifted by up to a quarter turn).
static void testWeakSignal() {
    Channel c;
    c.esn0Db = 1.0;
    c.cfoHz = 1234.0;
    c.delaySamples = 0.21;
    const auto r = runLink(12, 20, c, 4242);
    std::printf("weak signal QPSK 6/15 at 1 dB: frames %d/%d code words %d/%d, MER %.1f dB\n", r.framesFound, r.framesSent, r.blocksOk, r.blocks, r.merDb);
    CHECK(r.framesFound >= 19);
    CHECK(r.blocksOk >= 19);
}

// Deep sync: frames (preamble and header) found at -5 dB, where the hybrid fallback must still work (832-symbol preamble, header sent 4 times).
static void testDeepSync() {
    Channel c;
    c.esn0Db = -5.0;
    c.cfoHz = 1234.0;
    c.delaySamples = 0.21;
    const auto r = runLink(12, 20, c, 4343);
    std::printf("deep sync at -5 dB: frames found %d/%d\n", r.framesFound, r.framesSent);
    CHECK(r.framesFound >= 19);
}

static void testNoiseOnly() {
    Rng rng(7);
    std::vector<cf> x(400000);
    for (auto& v : x) v = cf(static_cast<float>(rng.gauss()), static_cast<float>(rng.gauss()));
    Receiver rx;
    size_t frames = 0;
    for (size_t at = 0; at < x.size(); at += 50000) frames += rx.push(x.data() + at, 50000).size();
    std::printf("noise only: %zu frames, %d header rejects\n", frames, rx.headerRejects());
    CHECK(frames == 0);
}

int main() {
    testDemapExact();
    testBudgets();
    testLayout();
    testHeader();
    testInterleaver();
    testClean();
    testRtlLink();
    testWeakSignal();
    testDeepSync();
    testNoiseOnly();
    if (fails) {
        std::printf("test_kavl_phy: %d failures\n", fails);
        return 1;
    }
    std::printf("test_kavl_phy: all passed\n");
    return 0;
}
