// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL physical layer, transmit side (layout in kavl/include/kavl/kavl_phy.h).
#include "kavl/kavl_phy.h"
#include "kavl/kavl_fec.h"
#include "kavl/kavl_ldpc.h"
#include "kavl_tables.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace kavl {

namespace {

Modcod make(int id, const char* name, int bitsPerSym, int rate15) {
    Modcod m{};
    m.id = id;
    m.name = name;
    m.bitsPerSym = bitsPerSym;
    m.rate15 = rate15;
    m.blocks = m.bitsPerSym / 2;
    m.kBits = kLdpcN * rate15 / 15;
    m.payloadBytes = (m.kBits - 32) / 8;
    return m;
}

const std::array<Modcod, kNumModcods>& table() {
    static const std::array<Modcod, kNumModcods> t = {
        make(0, "QPSK 8/15", 2, 8),           make(1, "NUC-16 9/15", 4, 9),        make(2, "NUC-64 10/15", 6, 10),
        make(3, "NUC-256 10/15", 8, 10),    make(4, "NUC-256 11/15", 8, 11),    make(5, "NUC-256 12/15", 8, 12),
        make(6, "NUC-256 13/15", 8, 13),    make(7, "NUC-1024 10/15", 10, 10),  make(8, "NUC-1024 11/15", 10, 11),
        make(9, "NUC-1024 12/15", 10, 12),
        // weak-signal modes within the same width (owner: never above 1 MHz, even for 1080p60): 1080p60 in about 950 kHz, 650 kHz
        make(10, "QPSK 11/15", 2, 11),      make(11, "NUC-16 8/15", 4, 8),
        make(12, "QPSK 6/15", 2, 6),   // about 0 dB: the 0 dB profile family
    };
    return t;
}

// A/322 constellations: QPSK uniform; 16, 64, 256 points two-dimensional non-uniform (first quadrant position vector, the other quadrants by symmetry);
// 1024 points one-dimensional non-uniform (the real part from the odd label bits, the imaginary part from the even ones, sign first, then the Gray code of the
// magnitude). The same construction as the author's ATSC 3.0 receiver in OnAir.
std::vector<cf> buildConstellation(int eta, int rate15) {
    const int M = 1 << eta;
    std::vector<cf> x(static_cast<size_t>(M));
    if (eta == 2) {
        const float s = 0.70710678f;
        return {cf(s, s), cf(-s, s), cf(s, -s), cf(-s, -s)};
    }
    const float* v = tables::nucVectors(eta, rate15);
    if (!v) return x;
    if (eta <= 8) {
        const int b = M / 4;
        for (int i = 0; i < b; i++) {
            const cf w(v[2 * i], v[2 * i + 1]);
            x[static_cast<size_t>(i)] = w;
            x[static_cast<size_t>(b + i)] = -std::conj(w);
            x[static_cast<size_t>(2 * b + i)] = std::conj(w);
            x[static_cast<size_t>(3 * b + i)] = -w;
        }
    } else {
        const int half = eta / 2, P = 1 << (half - 1);
        auto grayInverse = [](int g) {
            int r = g;
            for (int s = 1; s < 16; s <<= 1) r ^= r >> s;
            return r;
        };
        auto dim = [&](const int* bits) {
            int g = 0;
            for (int i = 1; i < half; i++) g = (g << 1) | bits[i];
            const int p = P - 1 - grayInverse(g);
            return bits[0] ? -v[p] : v[p];
        };
        for (int label = 0; label < M; label++) {
            int y[12], re[6], im[6];
            for (int k = 0; k < eta; k++) y[k] = (label >> (eta - 1 - k)) & 1;
            for (int i = 0; i < half; i++) {
                re[i] = y[2 * i + 1];
                im[i] = y[2 * i];
            }
            x[static_cast<size_t>(label)] = cf(dim(re), dim(im));
        }
    }
    double p = 0;   // unit average power
    for (const auto& c : x) p += std::norm(c);
    const float g = static_cast<float>(1.0 / std::sqrt(p / M));
    for (auto& c : x) c *= g;
    return x;
}

// One permutation and one scrambling sequence per frame size (1 .. 5 code words).
struct BitMaps {
    std::vector<uint32_t> perm;   // transmitted index -> code index
    std::vector<uint8_t> scr;
};
const BitMaps& bitMaps(int nBits) {
    static const std::array<BitMaps, 5> maps = [] {
        std::array<BitMaps, 5> a;
        for (int b = 1; b <= 5; b++) {
            const int n = b * kLdpcN;
            BitMaps& m = a[static_cast<size_t>(b - 1)];
            m.perm.resize(static_cast<size_t>(n));
            for (int i = 0; i < n; i++) m.perm[static_cast<size_t>(i)] = static_cast<uint32_t>(i);
            XorShift64 r{0x4B41564C00000000ull + static_cast<uint64_t>(b)};   // "KAVL"
            for (int i = n - 1; i > 0; i--) std::swap(m.perm[static_cast<size_t>(i)], m.perm[static_cast<size_t>(r.next() % static_cast<uint64_t>(i + 1))]);
            XorShift64 s{0x5343524D4B41564Cull + static_cast<uint64_t>(b)};
            m.scr.resize(static_cast<size_t>(n));
            for (int i = 0; i < n; i++) m.scr[static_cast<size_t>(i)] = static_cast<uint8_t>((s.next() >> 40) & 1u);
        }
        return a;
    }();
    const int b = nBits / kLdpcN;
    return maps[static_cast<size_t>(std::clamp(b, 1, 5) - 1)];
}

}  // namespace

const VideoProfile& videoProfile(int i) {
    // The family (owner, 2026-11-24): about twice the video of the first 0 dB family (its 241 kbit/s at 1080p60 looked too soft), each on the
    // most robust mode that fits under 1 MHz: QPSK 6/15 (about 1 dB) up to 1080p60, QPSK 8/15 (2 dB) for 1440p60, QPSK 11/15 (about 5 dB) for
    // 4K60. Stereo Opus 32 kbit/s; a key frame every 2 s (4 s at 360p). The width sets the video budget (videoKbps).
    static const VideoProfile t[kNumVideoProfiles] = {
        {"360p15, 135 kHz", 640, 360, 15, 135000.0 / 1.02, 12, 32, 2, 20, 12, 4},
        {"360p30, 230 kHz", 640, 360, 30, 230000.0 / 1.02, 12, 32, 2, 20, 12, 4},
        {"720p60, 500 kHz", 1280, 720, 60, 500000.0 / 1.02, 12, 32, 2, 20, 18, 2},
        {"1080p60, 800 kHz", 1920, 1080, 60, 800000.0 / 1.02, 12, 32, 2, 20, 18, 2},
        {"1440p60, 930 kHz", 2560, 1440, 60, 930000.0 / 1.02, 0, 32, 2, 20, 18, 2},
        {"4K60, 1 MHz", 3840, 2160, 60, 1000000.0 / 1.02, 10, 32, 2, 20, 18, 2},
    };
    return t[std::clamp(i, 0, kNumVideoProfiles - 1)];
}

int videoKbps(const VideoProfile& p, int fecIdx) {
    const Modcod* m = modcod(p.modcod);
    const fec::Profile f = fec::profile(fecIdx, m ? m->blocks : 1);
    const double share = f.parity > 0 ? static_cast<double>(f.data) / f.n() : 1.0;
    return m ? static_cast<int>(netBitRate(*m, p.symRate) * share / 1.10 / 1000.0) - p.soundKbps - p.overheadKbps : 0;
}

const Modcod* modcod(int id) {
    if (id < 0 || id >= kNumModcods) return nullptr;
    return &table()[static_cast<size_t>(id)];
}

const std::vector<cf>& constellation(const Modcod& m) {
    static const std::array<std::vector<cf>, kNumModcods> c = [] {
        std::array<std::vector<cf>, kNumModcods> a;
        for (int i = 0; i < kNumModcods; i++) a[static_cast<size_t>(i)] = buildConstellation(table()[static_cast<size_t>(i)].bitsPerSym, table()[static_cast<size_t>(i)].rate15);
        return a;
    }();
    return c[static_cast<size_t>(std::clamp(m.id, 0, kNumModcods - 1))];
}

std::vector<cf> mapBits(const Modcod& m, const uint8_t* bits, size_t nBits) {
    const auto& pts = constellation(m);
    const size_t eta = static_cast<size_t>(m.bitsPerSym), n = nBits / eta;
    std::vector<cf> s(n);
    for (size_t i = 0; i < n; i++) {
        unsigned label = 0;
        for (size_t k = 0; k < eta; k++) label = (label << 1) | (bits[i * eta + k] & 1u);
        s[i] = pts[label];
    }
    return s;
}

void demapSoft(const Modcod& m, const cf* sym, size_t n, float n0, float* llr) {
    const auto& pts = constellation(m);
    const int eta = m.bitsPerSym, M = 1 << eta;
    const float inv = 1.f / std::max(n0, 1e-12f);
    std::vector<float> pr(static_cast<size_t>(M)), pi(static_cast<size_t>(M));
    for (int i = 0; i < M; i++) {
        pr[static_cast<size_t>(i)] = pts[static_cast<size_t>(i)].real();
        pi[static_cast<size_t>(i)] = pts[static_cast<size_t>(i)].imag();
    }
    // Max-log: for each bit the nearest point with that bit 0 and with it 1. A min tree from the lowest bit up: the lowest bit's two minima are
    // those of the even and the odd labels; the pairwise minima form a half-size array whose lowest bit is the next bit, and so on (about 3 M
    // comparisons instead of eta M). The squared distances are compared as uint32: non-negative floats order like their bits, and integer
    // minima vectorise. The results are exactly those of the direct search (test_kavl_phy checks it).
    parallelFor(static_cast<long>(n), kWorkThreads, [&](long lo, long hi) {
    std::vector<float> d(static_cast<size_t>(M));
    std::vector<uint32_t> a(static_cast<size_t>(M)), b(static_cast<size_t>(M / 2 + 1));
    for (size_t s = static_cast<size_t>(lo); s < static_cast<size_t>(hi); s++) {
        const float cr = sym[s].real(), ci = sym[s].imag();
        for (int i = 0; i < M; i++) {
            const float x = cr - pr[static_cast<size_t>(i)], y = ci - pi[static_cast<size_t>(i)];
            d[static_cast<size_t>(i)] = x * x + y * y;
        }
        std::memcpy(a.data(), d.data(), sizeof(float) * static_cast<size_t>(M));
        uint32_t* cur = a.data();
        uint32_t* nxt = b.data();
        for (int k = eta - 1, L = M; k >= 0; k--, L >>= 1) {   // k: the bit (0 = the first transmitted), from the last
            uint32_t m0 = ~0u, m1 = ~0u;
            for (int j = 0; j < L / 2; j++) {
                const uint32_t e = cur[2 * j], o = cur[2 * j + 1];
                m0 = std::min(m0, e);
                m1 = std::min(m1, o);
                nxt[j] = std::min(e, o);
            }
            float f0, f1;
            std::memcpy(&f0, &m0, sizeof(f0));
            std::memcpy(&f1, &m1, sizeof(f1));
            llr[s * static_cast<size_t>(eta) + static_cast<size_t>(k)] = (f1 - f0) * inv;
            std::swap(cur, nxt);
        }
    }
    });
}

std::vector<uint8_t> headerBits(const Header& h) {
    std::vector<uint8_t> b;
    b.reserve(kHeaderInfoBits);
    auto put = [&](uint32_t v, int n) {
        for (int i = n - 1; i >= 0; i--) b.push_back(static_cast<uint8_t>((v >> i) & 1u));
    };
    put(0, 2);
    put(static_cast<uint32_t>(h.modcod) & 15u, 4);
    put(static_cast<uint32_t>(h.fec) & 7u, 3);
    put(static_cast<uint32_t>(h.layer) & 3u, 2);
    put(static_cast<uint32_t>(h.seq) & 0xFFFFu, 16);
    put(static_cast<uint32_t>(h.linkQ) & 0xFFu, 8);
    put(static_cast<uint32_t>(h.id) & 0x1FFFu, 13);
    uint8_t bytes[6] = {};
    for (int i = 0; i < 48; i++) bytes[i / 8] = static_cast<uint8_t>(bytes[i / 8] | (b[static_cast<size_t>(i)] << (7 - i % 8)));
    put(crc16(bytes, 6), 16);
    return b;
}

bool parseHeader(const uint8_t* bits, Header& h) {
    uint8_t bytes[6] = {};
    for (int i = 0; i < 48; i++) bytes[i / 8] = static_cast<uint8_t>(bytes[i / 8] | ((bits[i] & 1) << (7 - i % 8)));
    uint32_t crc = 0;
    for (int i = 48; i < 64; i++) crc = (crc << 1) | (bits[i] & 1u);
    if (crc != crc16(bytes, 6)) return false;
    auto get = [&](int at, int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) v = (v << 1) | (bits[at + i] & 1u);
        return static_cast<int>(v);
    };
    if (get(0, 2) != 0) return false;
    h.modcod = get(2, 4);
    h.fec = get(6, 3);
    h.layer = get(9, 2);
    h.seq = get(11, 16);
    h.linkQ = get(27, 8);
    h.id = get(35, 13);
    return true;
}

namespace {
// 6-bit characters of the station ID: 0 = end, 1-26 A-Z, 27-36 0-9, 37 '/', 38 '-'
int idCode(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return 1 + (c - 'A');
    if (c >= '0' && c <= '9') return 27 + (c - '0');
    if (c == '/') return 37;
    if (c == '-') return 38;
    return -1;
}
char idChar(int v) {
    if (v >= 1 && v <= 26) return static_cast<char>('A' + v - 1);
    if (v >= 27 && v <= 36) return static_cast<char>('0' + v - 27);
    return v == 37 ? '/' : v == 38 ? '-' : '\0';
}
}  // namespace

int stationIdSlice(const std::string& callsign, int seq) {
    std::string s;
    for (char c : callsign)
        if (idCode(c) > 0 && static_cast<int>(s.size()) < kStationIdChars) s += c;
    if (s.empty()) return 0;
    const int k = seq & 7;
    auto code = [&](size_t i) { return i < s.size() ? idCode(s[i]) : 0; };
    return (1 << 12) | (code(2 * static_cast<size_t>(k)) << 6) | code(2 * static_cast<size_t>(k) + 1);
}

void StationIdReader::push(const Header& h) {
    if (!(h.id & (1 << 12))) return;
    const int k = h.seq & 7;
    slice_[k] = h.id & 0xFFF;
    have_ |= 1 << k;
}

std::string StationIdReader::callsign() const {
    std::string s;
    for (int k = 0; k < 8; k++) {
        if (!(have_ & (1 << k))) return "";   // a pair before the end is missing
        for (int v : {slice_[k] >> 6, slice_[k] & 63}) {
            const char c = idChar(v);
            if (!c) return s;
            s += c;
        }
    }
    return s;
}

void StationIdReader::reset() {
    have_ = 0;
}

std::vector<cf> headerSymbols(const Header& h) {
    const auto coded = convEncode(headerBits(h));   // kHeaderCodedBits
    static const cf q[4] = {cf(1, 0), cf(0, 1), cf(-1, 0), cf(0, -1)};
    std::vector<cf> s(static_cast<size_t>(kHeaderSyms));
    for (size_t i = 0; i < s.size(); i++) s[i] = q[i % 4] * (coded[i % coded.size()] ? -1.f : 1.f);   // the coded block kHeaderRepeat times
    return s;
}

namespace {
std::vector<cf> randomQpsk(uint64_t seed, int n) {
    std::vector<cf> v(static_cast<size_t>(n));
    XorShift64 r{seed};
    const float s = 0.70710678f;
    for (auto& x : v) {
        const uint64_t b = r.next() >> 40;
        x = cf((b & 1u) ? -s : s, (b & 2u) ? -s : s);
    }
    return v;
}
}  // namespace

const std::vector<cf>& preambleSymbols() {
    static const std::vector<cf> p = randomQpsk(0x4B41564C50524531ull, kPreambleSyms);   // "KAVLPRE1"
    return p;
}

cf pilotSymbol(int i) {
    static const std::vector<cf> v = randomQpsk(0x50494C4F544B4156ull, kPilots);
    return v[static_cast<size_t>(std::clamp(i, 0, kPilots - 1))];
}

const RegionMap& regionMap() {
    static const RegionMap m = [] {
        RegionMap r;
        int idx = 0, left = kDataSyms;
        while (left > 0) {
            r.pilot.push_back(idx++);
            const int g = std::min(kPilotGroup, left);
            for (int i = 0; i < g; i++) r.data.push_back(idx++);
            left -= g;
        }
        r.pilot.push_back(idx++);
        return r;
    }();
    return m;
}

void interleaveScramble(const uint8_t* in, uint8_t* out, int nBits) {
    const BitMaps& m = bitMaps(nBits);
    for (int i = 0; i < nBits; i++) out[i] = static_cast<uint8_t>((in[m.perm[static_cast<size_t>(i)]] ^ m.scr[static_cast<size_t>(i)]) & 1u);
}

void deinterleaveDescrambleLlr(const float* inTx, float* outCode, int nBits) {
    const BitMaps& m = bitMaps(nBits);
    for (int i = 0; i < nBits; i++) outCode[m.perm[static_cast<size_t>(i)]] = m.scr[static_cast<size_t>(i)] ? -inTx[i] : inTx[i];
}

std::vector<uint8_t> frameCodedBits(const Modcod& m, const uint8_t* payload) {
    const auto& code = ldpcCode(m.rate15);
    const int nBits = m.blocks * kLdpcN;
    std::vector<uint8_t> coded(static_cast<size_t>(nBits));
    for (int b = 0; b < m.blocks; b++) {
        const uint8_t* p = payload + static_cast<size_t>(b) * static_cast<size_t>(m.payloadBytes);
        std::vector<uint8_t> bits(static_cast<size_t>(m.kBits), 0);
        for (int i = 0; i < m.payloadBytes * 8; i++) bits[static_cast<size_t>(i)] = static_cast<uint8_t>((p[i / 8] >> (7 - i % 8)) & 1u);
        const uint32_t crc = crc32(p, static_cast<size_t>(m.payloadBytes));
        for (int i = 0; i < 32; i++) bits[static_cast<size_t>(m.payloadBytes * 8 + i)] = static_cast<uint8_t>((crc >> (31 - i)) & 1u);
        code.encode(bits);   // extended to the code length
        std::copy(bits.begin(), bits.begin() + kLdpcN, coded.begin() + static_cast<long>(b) * kLdpcN);
    }
    std::vector<uint8_t> tx(coded.size());
    interleaveScramble(coded.data(), tx.data(), nBits);
    return tx;
}

std::vector<cf> assembleFrame(const Header& h, const std::vector<cf>& dataSyms) {
    std::vector<cf> f;
    f.reserve(kFrameSyms);
    const auto& pre = preambleSymbols();
    f.insert(f.end(), pre.begin(), pre.end());
    const auto hs = headerSymbols(h);
    f.insert(f.end(), hs.begin(), hs.end());
    std::vector<cf> region(kRegionSyms);
    const RegionMap& rm = regionMap();
    for (int i = 0; i < kPilots; i++) region[static_cast<size_t>(rm.pilot[static_cast<size_t>(i)])] = pilotSymbol(i);
    for (int i = 0; i < kDataSyms; i++) region[static_cast<size_t>(rm.data[static_cast<size_t>(i)])] = i < static_cast<int>(dataSyms.size()) ? dataSyms[static_cast<size_t>(i)] : cf(0, 0);
    f.insert(f.end(), region.begin(), region.end());
    return f;
}

std::vector<cf> frameSymbols(const Modcod& m, const Header& h, const uint8_t* payload) {
    const auto bits = frameCodedBits(m, payload);
    const auto data = mapBits(m, bits.data(), bits.size());
    return assembleFrame(h, data);
}

const std::vector<float>& rrc() {
    static const std::vector<float> t = rrcTaps(kRrcBeta, kSps, kRrcSpan);
    return t;
}

Transmitter::Transmitter(int modcodId) : m_(::kavl::modcod(modcodId)) {
    if (!m_) m_ = ::kavl::modcod(0);
}

std::vector<cf> Transmitter::shape(const std::vector<cf>& syms) {
    const auto& taps = rrc();
    const int nt = static_cast<int>(taps.size()), half = nt / 2;
    const long nOut = static_cast<long>(syms.size()) * kSps;
    std::vector<cf> out(static_cast<size_t>(nOut), cf(0, 0));
    const float g = std::sqrt(static_cast<float>(kSps));   // unit-energy taps, one symbol per kSps samples -> power 1 per sample
    for (size_t s = 0; s < syms.size(); s++) {
        const cf v = syms[s] * g;
        if (v == cf(0, 0)) continue;
        const long c = static_cast<long>(s) * kSps;
        for (int k = 0; k < nt; k++) {
            const long i = c + k - half;
            if (i >= 0 && i < nOut) out[static_cast<size_t>(i)] += v * taps[static_cast<size_t>(k)];
        }
    }
    return out;
}

std::vector<cf> Transmitter::modulate(const std::vector<std::vector<uint8_t>>& frames, int fec, int linkQ, int startSeq, int leadSyms, int tailSyms) const {
    std::vector<cf> syms(static_cast<size_t>(std::max(0, leadSyms)), cf(0, 0));
    const size_t need = static_cast<size_t>(framePayloadBytes(*m_));
    int seq = startSeq;
    for (const auto& f : frames) {
        std::vector<uint8_t> pay(need, 0);
        std::memcpy(pay.data(), f.data(), std::min(need, f.size()));
        Header h;
        h.modcod = m_->id;
        h.fec = fec;
        h.seq = seq & 0xFFFF;
        h.linkQ = linkQ;
        const auto fs = frameSymbols(*m_, h, pay.data());
        syms.insert(syms.end(), fs.begin(), fs.end());
        seq++;
    }
    syms.insert(syms.end(), static_cast<size_t>(std::max(0, tailSyms)), cf(0, 0));
    return shape(syms);
}

}  // namespace kavl
