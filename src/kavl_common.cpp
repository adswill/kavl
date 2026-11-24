// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL shared helpers (kavl/include/kavl/kavl_common.h).
#include "kavl/kavl_common.h"
#include <array>
#include <cmath>

namespace kavl {

namespace {
constexpr double kPi = 3.141592653589793;
inline unsigned parity(unsigned v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1u;
}
constexpr unsigned kG0 = 0171, kG1 = 0133;
}  // namespace

uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= static_cast<uint16_t>(d[i]) << 8;
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x1021) : static_cast<uint16_t>(c << 1);
    }
    return c;
}

uint32_t crc32(const uint8_t* d, size_t n) {
    static const std::array<uint32_t, 256> tab = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::vector<cf> zadoffChu(int root, int length) {
    std::vector<cf> x(static_cast<size_t>(length > 0 ? length : 0));
    for (int n = 0; n < length; n++) {
        const double nn = static_cast<double>(n);
        const double ph = (length % 2) ? -kPi * root * nn * (nn + 1.0) / length : -kPi * root * nn * nn / length;
        x[static_cast<size_t>(n)] = cf(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
    }
    return x;
}

std::vector<float> rrcTaps(float beta, int sps, int spanSymbols) {
    const int n = spanSymbols * sps + 1, mid = n / 2;
    std::vector<float> h(static_cast<size_t>(n));
    const double b = beta;
    double e = 0;
    for (int i = 0; i < n; i++) {
        const double t = static_cast<double>(i - mid) / sps;
        double v;
        if (std::fabs(t) < 1e-12) v = 1.0 - b + 4.0 * b / kPi;
        else if (b > 0 && std::fabs(std::fabs(t) - 1.0 / (4.0 * b)) < 1e-9)
            v = b / std::sqrt(2.0) * ((1.0 + 2.0 / kPi) * std::sin(kPi / (4.0 * b)) + (1.0 - 2.0 / kPi) * std::cos(kPi / (4.0 * b)));
        else
            v = (std::sin(kPi * t * (1.0 - b)) + 4.0 * b * t * std::cos(kPi * t * (1.0 + b))) / (kPi * t * (1.0 - (4.0 * b * t) * (4.0 * b * t)));
        h[static_cast<size_t>(i)] = static_cast<float>(v);
        e += v * v;
    }
    const float g = static_cast<float>(1.0 / std::sqrt(e));
    for (auto& v : h) v *= g;
    return h;
}

void fft(cf* x, int n, bool inverse) {
    for (int i = 1, j = 0; i < n; i++) {   // bit reversal
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = (inverse ? 2.0 : -2.0) * kPi / len;
        const cf wl(static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang)));
        for (int i = 0; i < n; i += len) {
            cf w(1.f, 0.f);
            for (int k = 0; k < len / 2; k++) {
                const cf u = x[i + k], v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
                if ((k & 31) == 31) {   // keep the twiddle exact over long butterflies
                    const double a = ang * (k + 1);
                    w = cf(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
                } else {
                    w *= wl;
                }
            }
        }
    }
}

void corrFft(const cf* in, long nOut, const std::vector<cf>& taps, cf* out) {
    const int nt = static_cast<int>(taps.size());
    if (nOut <= 0 || nt == 0) return;
    int n = 1;
    while (n < 4 * nt) n <<= 1;
    n = std::max(n, 4096);
    const int step = n - (nt - 1);
    std::vector<cf> H(static_cast<size_t>(n), cf(0, 0));   // reversed taps: a convolution that reads as a correlation
    for (int k = 0; k < nt; k++) H[static_cast<size_t>(k)] = taps[static_cast<size_t>(nt - 1 - k)];
    fft(H.data(), n, false);
    std::vector<cf> b(static_cast<size_t>(n));
    for (long o0 = 0; o0 < nOut; o0 += step) {
        const long cnt = std::min<long>(step, nOut - o0), avail = cnt + nt - 1;
        for (long i = 0; i < n; i++) b[static_cast<size_t>(i)] = i < avail ? in[o0 + i] : cf(0, 0);
        fft(b.data(), n, false);
        for (int i = 0; i < n; i++) b[static_cast<size_t>(i)] *= H[static_cast<size_t>(i)];
        fft(b.data(), n, true);
        const float inv = 1.f / static_cast<float>(n);
        for (long i = 0; i < cnt; i++) out[o0 + i] = b[static_cast<size_t>(i + nt - 1)] * inv;
    }
}

void firFft(const cf* in, long nOut, const std::vector<float>& taps, cf* out) {
    const int nt = static_cast<int>(taps.size());
    int n = 1;
    while (n < 4 * nt) n <<= 1;
    n = std::max(n, 4096);
    const int step = n - (nt - 1);   // outputs per block
    // the spectrum of the taps, laid out for correlation-style indexing (out[i] = sum taps[k] in[i + k]): reversed taps
    std::vector<cf> H(static_cast<size_t>(n), cf(0, 0));
    for (int k = 0; k < nt; k++) H[static_cast<size_t>(k)] = cf(taps[static_cast<size_t>(nt - 1 - k)], 0.f);
    fft(H.data(), n, false);
    const long blocks = (nOut + step - 1) / step;
    parallelFor(blocks, workThreads(), [&](long lo, long hi) {
        std::vector<cf> buf(static_cast<size_t>(n));
        for (long b = lo; b < hi; b++) {
            const long o0 = b * step, cnt = std::min<long>(step, nOut - o0);
            const long avail = cnt + nt - 1;
            for (long i = 0; i < n; i++) buf[static_cast<size_t>(i)] = i < avail ? in[o0 + i] : cf(0, 0);
            fft(buf.data(), n, false);
            for (int i = 0; i < n; i++) buf[static_cast<size_t>(i)] *= H[static_cast<size_t>(i)];
            fft(buf.data(), n, true);
            const float inv = 1.f / static_cast<float>(n);
            for (long i = 0; i < cnt; i++) out[o0 + i] = buf[static_cast<size_t>(i + nt - 1)] * inv;
        }
    }, 1);
}

std::vector<uint8_t> convEncode(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> out;
    out.reserve((bits.size() + 6) * 2);
    unsigned state = 0;   // the last six input bits, newest in bit 5
    auto step = [&](unsigned u) {
        const unsigned reg = (u << 6) | state;
        out.push_back(static_cast<uint8_t>(parity(reg & kG0)));
        out.push_back(static_cast<uint8_t>(parity(reg & kG1)));
        state = reg >> 1;
    };
    for (uint8_t b : bits) step(b & 1u);
    for (int i = 0; i < 6; i++) step(0);
    return out;
}

std::vector<uint8_t> viterbiDecode(const std::vector<float>& llr) {
    const size_t steps = llr.size() / 2;
    if (steps < 6) return {};
    std::vector<float> pm(64, -1e30f), nm(64);
    pm[0] = 0.f;
    std::vector<uint8_t> dec(steps * 64);   // the input bit of the survivor into each state... stored as the predecessor's dropped bit
    std::array<uint8_t, 128> o0{}, o1{};
    for (unsigned reg = 0; reg < 128; reg++) {
        o0[reg] = static_cast<uint8_t>(parity(reg & kG0));
        o1[reg] = static_cast<uint8_t>(parity(reg & kG1));
    }
    for (size_t t = 0; t < steps; t++) {
        const float l0 = llr[2 * t], l1 = llr[2 * t + 1];
        for (auto& v : nm) v = -1e30f;
        for (unsigned s = 0; s < 64; s++) {
            if (pm[s] < -1e29f) continue;
            for (unsigned u = 0; u < 2; u++) {
                const unsigned reg = (u << 6) | s, ns = reg >> 1;
                const float bm = (o0[reg] ? -l0 : l0) + (o1[reg] ? -l1 : l1);
                const float m = pm[s] + bm;
                if (m > nm[ns]) {
                    nm[ns] = m;
                    dec[t * 64 + ns] = static_cast<uint8_t>(s & 1u);   // the bit shifted out tells the predecessor
                }
            }
        }
        pm.swap(nm);
    }
    // terminated: end in state 0; trace back
    std::vector<uint8_t> bits(steps);
    unsigned s = 0;
    for (size_t t = steps; t-- > 0;) {
        bits[t] = static_cast<uint8_t>((s >> 5) & 1u);   // the newest bit of the state is the input of step t
        s = ((s << 1) & 63u) | dec[t * 64 + s];
    }
    bits.resize(steps - 6);
    return bits;
}

}  // namespace kavl
