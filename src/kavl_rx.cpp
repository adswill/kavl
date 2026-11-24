// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL receiver (kavl/include/kavl/kavl_rx.h).
#include "kavl/kavl_rx.h"
#include "kavl/kavl_ldpc.h"
#include "kavl/kavl_resampler.h"
#include <algorithm>
#include <cmath>
#include <thread>

namespace kavl {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kTwoPi = 6.283185307179586;

// Raised-cosine pulse (the transmit and matched filters together) at x samples, 4 samples per symbol.
double rcPulse(double x) {
    const double t = x / kSps;
    const double s = std::fabs(t) < 1e-9 ? 1.0 : std::sin(kPi * t) / (kPi * t);
    const double d = 1.0 - (2.0 * kRrcBeta * t) * (2.0 * kRrcBeta * t);
    if (std::fabs(d) < 1e-6) return s * (kPi / 4.0);
    return s * std::cos(kPi * kRrcBeta * t) / d;
}

// Gain of the early-late timing detector: y(T+1) - y(T-1) = -2 eps rc'(1) for a timing error eps (samples).
double timingGain() {
    const double h = 1e-3;
    return -2.0 * (rcPulse(1.0 + h) - rcPulse(1.0 - h)) / (2.0 * h);
}

void lsLine(const std::vector<double>& x, const std::vector<double>& y, double& a, double& b) {
    const size_t n = x.size();
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; i++) {
        sx += x[i];
        sy += y[i];
        sxx += x[i] * x[i];
        sxy += x[i] * y[i];
    }
    const double den = n * sxx - sx * sx;
    b = std::fabs(den) > 1e-12 ? (n * sxy - sx * sy) / den : 0.0;
    a = n ? (sy - b * sx) / n : 0.0;
}

}  // namespace

struct Receiver::Impl {
    Options o;
    std::unique_ptr<Resampler> rs;
    std::vector<cf> rsOut;
    cf dc{0, 0}, w{0, 0};
    bool dcInit = false;
    std::vector<cf> buf, mf, q;   // input at 4 sps, matched filter output, q[k] = mf[k + 4] conj(mf[k])
    long mfDone = 0;              // mf[i] valid for i < mfDone
    long scan = 0;
    long long base = 0;           // absolute 4-sps index of buf[0]
    int rejects = 0;
    double bPrev = 0.0;           // clock drift of the last frame (samples per symbol)
    bool haveB = false;
    std::vector<cf> dref;         // conj of the preamble's symbol-to-symbol products
    double kt = timingGain();

    explicit Impl(const Options& op) : o(op) {
        const double r4 = kSps * o.symRate;
        if (o.inRate > 0 && std::fabs(o.inRate - r4) > 1e-6 * r4) {
            rs = std::make_unique<Resampler>();
            if (!rs->configure(o.inRate, r4)) rs.reset();
        }
        const auto& p = preambleSymbols();
        dref.resize(kPreambleSyms - 1);
        for (int m = 0; m < kPreambleSyms - 1; m++) dref[static_cast<size_t>(m)] = std::conj(p[static_cast<size_t>(m + 1)] * std::conj(p[static_cast<size_t>(m)]));
    }

    void reset() {
        if (rs) rs->reset();
        dc = w = cf(0, 0);
        dcInit = false;
        buf.clear();
        mf.clear();
        q.clear();
        mfDone = scan = 0;
        base = 0;
        rejects = 0;
        bPrev = 0.0;
        haveB = false;
    }

    // DC removal and blind IQ-imbalance correction of a chunk, in place.
    void iqFix(cf* x, size_t n) {
        if (!o.iqCorrect || n == 0) return;
        cf mean(0, 0);
        for (size_t i = 0; i < n; i++) mean += x[i];
        mean /= static_cast<float>(n);
        const float a = dcInit ? std::min(1.f, static_cast<float>(n) / 200000.f) : 1.f;
        dc = dc * (1.f - a) + mean * a;
        dcInit = true;
        double pr = 0, pi = 0, pw = 0;
        for (size_t i = 0; i < n; i++) {
            const cf y = x[i] - dc;
            const cf z = y - w * std::conj(y);
            x[i] = z;
            const cf z2 = z * z;
            pr += z2.real();
            pi += z2.imag();
            pw += std::norm(z);
        }
        if (pw > 1e-20) {
            const float g = std::min(1.f, static_cast<float>(n) / 100000.f);
            w += cf(static_cast<float>(pr / pw), static_cast<float>(pi / pw)) * (0.5f * g);
        }
    }

    // The frame search runs on a short, cheap filter (roll-off 0.25, 12 symbols): the differential preamble metric does not need the exact matched filter,
    // which buildLocal applies per frame.
    static const std::vector<float>& detectTaps() {
        static const std::vector<float> t = rrcTaps(0.25f, kSps, 12);
        return t;
    }
    void computeMf() {
        const auto& taps = detectTaps();
        const long nt = static_cast<long>(taps.size()), half = nt / 2;
        const long avail = static_cast<long>(buf.size()) - (nt - 1 - half);
        if (avail <= mfDone) return;
        mf.resize(static_cast<size_t>(avail));
        for (long i = mfDone; i < avail; i++) {
            cf acc(0, 0);
            const long s = i - half;
            for (long k = 0; k < nt; k++) {
                const long j = s + k;
                if (j >= 0) acc += buf[static_cast<size_t>(j)] * taps[static_cast<size_t>(k)];
            }
            mf[static_cast<size_t>(i)] = acc;
        }
        const long q0 = static_cast<long>(q.size());
        if (avail - kSps > q0) {
            q.resize(static_cast<size_t>(avail - kSps));
            for (long i = q0; i < avail - kSps; i++) q[static_cast<size_t>(i)] = mf[static_cast<size_t>(i + kSps)] * std::conj(mf[static_cast<size_t>(i)]);
        }
        mfDone = avail;
    }

    // Differential preamble metric at sample n (needs mf up to n + 4 * 63).
    double metric(long n, cf* dOut = nullptr) const {
        cf d(0, 0);
        double e = 0;
        for (int m = 0; m < kPreambleSyms - 1; m++) d += q[static_cast<size_t>(n + kSps * m)] * dref[static_cast<size_t>(m)];
        for (int m = 0; m < kPreambleSyms; m++) e += std::norm(mf[static_cast<size_t>(n + kSps * m)]);
        if (dOut) *dOut = d;
        return e > 1e-20 ? std::abs(d) / e : 0.0;
    }

    // The matched filter output of one frame with the carrier offset removed BEFORE the filter (at roll-off 0.02 the filter's edge is only about 4 kHz
    // wide: filtering first would cut an offset signal's edge and let noise in at the other side). loc[i - locBase] for buffer indices from .. to.
    std::vector<cf> loc, mixed;
    long locBase = 0;
    void buildLocal(long from, long to, double fSym) {
        const auto& taps = rrc();
        const long nt = static_cast<long>(taps.size()), half = nt / 2, len = to - from;
        mixed.assign(static_cast<size_t>(len + nt), cf(0, 0));
        // the offset removed by a running phasor, made exact again every 1024 samples
        const double w = -kTwoPi * fSym / kSps;
        const cf step(static_cast<float>(std::cos(w)), static_cast<float>(std::sin(w)));
        cf ph(0, 0);
        for (long i = 0; i < len + nt; i++) {
            const long j = from - half + i;
            if ((i & 1023) == 0) {
                const double a = w * static_cast<double>(j);
                ph = cf(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
            }
            if (j >= 0 && j < static_cast<long>(buf.size())) mixed[static_cast<size_t>(i)] = buf[static_cast<size_t>(j)] * ph;
            ph *= step;
        }
        loc.assign(static_cast<size_t>(len), cf(0, 0));
        firFft(mixed.data(), len, taps, loc.data());   // the taps are symmetric, so this is the matched filter
        locBase = from;
    }

    // Cubic (4-point Lagrange) interpolation of the frame's matched filter output (buildLocal) at fractional buffer index t.
    cf at(double t) const {
        const long i = static_cast<long>(std::floor(t));
        const float m = static_cast<float>(t - static_cast<double>(i));
        auto v = [&](long k) {
            const long j = k - locBase;
            return (j >= 0 && j < static_cast<long>(loc.size())) ? loc[static_cast<size_t>(j)] : cf(0, 0);
        };
        return v(i - 1) * (-m * (m - 1) * (m - 2) / 6.0f) + v(i) * ((m + 1) * (m - 1) * (m - 2) / 2.0f) + v(i + 1) * (-(m + 1) * m * (m - 2) / 2.0f) +
               v(i + 2) * ((m + 1) * m * (m - 1) / 6.0f);
    }

    bool decodeFrame(double t0, double f0, RxFrame& rf, double& endT);
    std::vector<RxFrame> push(const cf* iq, size_t n);
};

bool Receiver::Impl::decodeFrame(double t0, double f0, RxFrame& rf, double& endT) {
    const auto& pre = preambleSymbols();
    double a = 0.0, b = 0.0;   // timing: T(k) = t0 + a + (kSps + b) k
    auto T = [&](int k) { return t0 + a + (kSps + b) * k; };
    auto rot = [](double f, int k) {
        const double ph = -kTwoPi * f * k;
        return cf(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
    };
    // coherent preamble correlation at sample t (carrier offset f removed), normalised to 0..1
    auto preCorr = [&](double t, double f, cf* sum = nullptr) {
        cf c(0, 0);
        double e = 0;
        for (int m = 0; m < kPreambleSyms; m++) {
            const cf y = at(t + kSps * m) * rot(f, m);
            c += y * std::conj(pre[static_cast<size_t>(m)]);
            e += std::norm(y);
        }
        if (sum) *sum = c;
        return e > 1e-20 ? std::abs(c) / std::sqrt(e * kPreambleSyms) : 0.0;
    };
    auto parabola = [](double m0, double m1, double m2) {
        const double den = m0 - 2.0 * m1 + m2;
        return std::fabs(den) > 1e-12 ? std::clamp(0.5 * (m0 - m2) / den, -0.5, 0.5) : 0.0;
    };

    // 0. the frame (and the next frame's preamble) through the matched filter with the coarse offset removed; f below is the residual offset
    buildLocal(static_cast<long>(std::floor(t0)) - 16, static_cast<long>(std::ceil(t0)) + kSps * (kFrameSyms + kPreambleSyms) + 120, f0);

    // 1. fine carrier offset from the preamble quarters, then the start time from the coherent correlation (parabola over +-1 sample), twice
    double f = 0.0;
    cf h(0, 0);
    double nvPre = 1.0;
    for (int pass = 0; pass < 2; pass++) {
        cf Q[4] = {};
        for (int m = 0; m < kPreambleSyms; m++) Q[m / 16] += at(T(m)) * rot(f, m) * std::conj(pre[static_cast<size_t>(m)]);
        cf acc(0, 0);
        for (int i = 0; i < 3; i++) acc += Q[i + 1] * std::conj(Q[i]);
        f += std::arg(acc) / (kTwoPi * 16.0);
        const double c0 = preCorr(t0 - 1.0, f), c1 = preCorr(t0, f), c2 = preCorr(t0 + 1.0, f);
        t0 += parabola(c0, c1, c2);
    }
    preCorr(t0, f, &h);
    h /= static_cast<float>(kPreambleSyms);
    {
        double nv = 0;
        for (int m = 0; m < kPreambleSyms; m++) nv += std::norm(at(T(m)) * rot(f, m) * std::conj(pre[static_cast<size_t>(m)]) - h);
        nvPre = std::max(1e-4, nv / kPreambleSyms / std::max(1e-20, static_cast<double>(std::norm(h))));
    }
    if (std::norm(h) < 1e-20) return false;

    // 2. header (BPSK turned by a quarter turn per symbol)
    static const cf quarter[4] = {cf(1, 0), cf(0, -1), cf(-1, 0), cf(0, 1)};   // conj(j^i)
    std::vector<float> hl(kHeaderCodedBits);
    for (int i = 0; i < kHeaderCodedBits; i++) {
        const int k = kHeaderStart + i;
        const cf y = at(T(k)) * rot(f, k) * quarter[i % 4] / h;
        hl[static_cast<size_t>(i)] = static_cast<float>(4.0 * y.real() / nvPre);
    }
    const auto hb = viterbiDecode(hl);
    if (hb.size() < static_cast<size_t>(kHeaderInfoBits) || !parseHeader(hb.data(), rf.hdr)) return false;
    const Modcod* mc = modcod(rf.hdr.modcod);
    if (!mc) return false;
    rf.mc = mc;

    // 3. clock drift from the next frame's preamble (exact: no data around it), else the last frame's
    bool haveDrift = false;
    {
        const double pred = t0 + (kSps + (haveB ? bPrev : 0.0)) * kFrameSyms;
        double best = 0.0, bt = 0.0;
        for (int d = -48; d <= 48; d++) {
            const double c = preCorr(pred + d, f);
            if (c > best) {
                best = c;
                bt = pred + d;
            }
        }
        if (best > 0.5) {
            bt += parabola(preCorr(bt - 1.0, f), best, preCorr(bt + 1.0, f));
            b = (bt - t0) / kFrameSyms - kSps;
            haveDrift = true;
            bPrev = b;
            haveB = true;
        } else if (haveB) {
            b = bPrev;
            haveDrift = true;
        }
    }

    // 4. pilots: timing fine-tuning, carrier phase, gains
    const RegionMap& rm = regionMap();
    std::vector<int> pk(kPilots);
    std::vector<double> pkd(kPilots);
    for (int i = 0; i < kPilots; i++) {
        pk[static_cast<size_t>(i)] = kRegionStart + rm.pilot[static_cast<size_t>(i)];
        pkd[static_cast<size_t>(i)] = pk[static_cast<size_t>(i)];
    }
    auto smooth = [](const std::vector<cf>& g, int W) {
        std::vector<cf> s(g.size());
        const int n = static_cast<int>(g.size());
        for (int i = 0; i < n; i++) {
            cf acc(0, 0);
            int c = 0;
            for (int j = std::max(0, i - W); j <= std::min(n - 1, i + W); j++, c++) acc += g[static_cast<size_t>(j)];
            s[static_cast<size_t>(i)] = acc / static_cast<float>(c);
        }
        return s;
    };
    std::vector<cf> g(kPilots);
    auto pilotGains = [&]() {
        for (int i = 0; i < kPilots; i++) g[static_cast<size_t>(i)] = at(T(pk[static_cast<size_t>(i)])) * rot(f, pk[static_cast<size_t>(i)]) * std::conj(pilotSymbol(i));
    };
    // Early-late error per pilot, fitted with a line. The data next to each pilot disturbs the detector a lot at roll-off 0.02, so the line needs many pilots:
    // with a known drift one fit over all of them; without one (a lone first frame) the fit grows from the first pilots outwards.
    auto fitTiming = [&](int nP, bool slope) {
        pilotGains();
        const auto gs = smooth(g, 3);
        std::vector<double> e(static_cast<size_t>(nP)), x(pkd.begin(), pkd.begin() + nP);
        for (int i = 0; i < nP; i++) {
            const int k = pk[static_cast<size_t>(i)];
            const cf dlt = (at(T(k) + 1.0) - at(T(k) - 1.0)) * rot(f, k) * std::conj(pilotSymbol(i));
            const double den = std::max(1e-20, static_cast<double>(std::norm(gs[static_cast<size_t>(i)]))) * kt;
            e[static_cast<size_t>(i)] = std::clamp((dlt * std::conj(gs[static_cast<size_t>(i)])).real() / den, -1.0, 1.0);
        }
        double ea, eb;
        lsLine(x, e, ea, eb);
        if (!slope) {
            double m = 0;
            for (double v : e) m += v;
            ea = m / nP;
            eb = 0;
        }
        a += ea;
        b += eb;
    };
    if (haveDrift) {
        fitTiming(kPilots, false);
        fitTiming(kPilots, false);
    } else {
        for (const int nP : {160, 480, kPilots}) {
            fitTiming(nP, true);
            fitTiming(nP, true);
        }
    }
    // carrier phase line over the pilots (residual offset)
    pilotGains();
    {
        std::vector<double> ph(kPilots);
        double prev = std::arg(g[0]);
        ph[0] = prev;
        for (int i = 1; i < kPilots; i++) {
            double p = std::arg(g[static_cast<size_t>(i)]);
            while (p - prev > kPi) p -= kTwoPi;
            while (p - prev < -kPi) p += kTwoPi;
            ph[static_cast<size_t>(i)] = p;
            prev = p;
        }
        double pa, pb;
        lsLine(pkd, ph, pa, pb);
        f += pb / kTwoPi;
    }
    pilotGains();
    const int W = std::max(1, o.pilotSmooth);
    const auto gs = smooth(g, W);
    double nvAbs = 0, sig = 0;
    for (int i = 0; i < kPilots; i++) {
        nvAbs += std::norm(g[static_cast<size_t>(i)] - gs[static_cast<size_t>(i)]);
        sig += std::norm(gs[static_cast<size_t>(i)]);
    }
    nvAbs = nvAbs / kPilots * (2.0 * W + 1.0) / (2.0 * W);
    sig /= kPilots;
    double n0 = std::max(1e-5, nvAbs / std::max(1e-20, sig));
    rf.esn0Db = 10.0 * std::log10(1.0 / n0);

    // raw (carrier-corrected, not equalised) data symbols and the pilot-interpolated gain
    std::vector<cf> yRaw(kRegionSyms), gain(kRegionSyms);
    for (int r = 0; r < kRegionSyms; r++) {
        const int k = kRegionStart + r;
        yRaw[static_cast<size_t>(r)] = at(T(k)) * rot(f, k);
    }
    for (int i = 0; i + 1 < kPilots; i++) {
        const int r0 = rm.pilot[static_cast<size_t>(i)], r1 = rm.pilot[static_cast<size_t>(i + 1)];
        for (int r = r0; r <= r1; r++) {
            const float u = static_cast<float>(r - r0) / static_cast<float>(r1 - r0);
            gain[static_cast<size_t>(r)] = gs[static_cast<size_t>(i)] * (1.f - u) + gs[static_cast<size_t>(i + 1)] * u;
        }
    }

    // 4. demap + decode
    const int nBits = mc->blocks * kLdpcN;
    const Ldpc& code = ldpcCode(mc->rate15);
    rf.blocks.assign(static_cast<size_t>(mc->blocks), RxBlock());
    std::vector<std::vector<uint8_t>> hard(static_cast<size_t>(mc->blocks));
    std::vector<cf> zs(kDataSyms);
    std::vector<float> llrTx(static_cast<size_t>(nBits)), llr(static_cast<size_t>(nBits));
    auto equalise = [&]() {
        for (int i = 0; i < kDataSyms; i++) {
            const int r = rm.data[static_cast<size_t>(i)];
            const cf gg = gain[static_cast<size_t>(r)];
            zs[static_cast<size_t>(i)] = std::norm(gg) > 1e-20f ? yRaw[static_cast<size_t>(r)] / gg : cf(0, 0);
        }
    };
    auto decodeBlocks = [&]() {
        demapSoft(*mc, zs.data(), zs.size(), static_cast<float>(n0), llrTx.data());
        deinterleaveDescrambleLlr(llrTx.data(), llr.data(), nBits);
        auto one = [&](int bI) {
            RxBlock& blk = rf.blocks[static_cast<size_t>(bI)];
            std::vector<float> l(llr.begin() + static_cast<long>(bI) * kLdpcN, llr.begin() + static_cast<long>(bI + 1) * kLdpcN);
            for (int i = mc->payloadBytes * 8 + 32; i < mc->kBits; i++) l[static_cast<size_t>(i)] = 50.f;   // known zero padding
            for (auto& v : l) v = std::clamp(v, -50.f, 50.f);
            std::vector<uint8_t> hd;
            int iters = -1;
            bool pass = false;
            pass = code.ok() && code.decode(l, o.maxIter, hd, &iters);
            hard[static_cast<size_t>(bI)] = hd;
            if (!pass || hd.size() < static_cast<size_t>(mc->kBits)) return;
            std::vector<uint8_t> bytes(static_cast<size_t>(mc->payloadBytes), 0);
            for (int i = 0; i < mc->payloadBytes * 8; i++) bytes[static_cast<size_t>(i / 8)] = static_cast<uint8_t>(bytes[static_cast<size_t>(i / 8)] | ((hd[static_cast<size_t>(i)] & 1) << (7 - i % 8)));
            uint32_t crc = 0;
            for (int i = 0; i < 32; i++) crc = (crc << 1) | (hd[static_cast<size_t>(mc->payloadBytes * 8 + i)] & 1u);
            if (crc != crc32(bytes.data(), bytes.size())) return;
            blk.ok = true;
            blk.iters = iters;
            blk.bytes = std::move(bytes);
        };
        std::vector<std::thread> th;
        for (int bI = 1; bI < mc->blocks; bI++)
            if (!rf.blocks[static_cast<size_t>(bI)].ok) th.emplace_back(one, bI);
        if (!rf.blocks[0].ok) one(0);
        for (auto& t : th) t.join();
        int okCount = 0;
        for (const auto& b : rf.blocks) okCount += b.ok ? 1 : 0;
        return okCount;
    };
    equalise();
    int okCount = decodeBlocks();

    // decisions of the whole frame (decoder output; exact for the code words that passed) -> data symbols
    auto decisions = [&]() {
        std::vector<uint8_t> coded(static_cast<size_t>(nBits), 0);
        for (int bI = 0; bI < mc->blocks; bI++) {
            const auto& hd = hard[static_cast<size_t>(bI)];
            if (hd.size() >= static_cast<size_t>(kLdpcN)) {
                std::copy(hd.begin(), hd.begin() + kLdpcN, coded.begin() + static_cast<long>(bI) * kLdpcN);
            } else {   // no decoder output: hard decisions of the LLRs
                for (int i = 0; i < kLdpcN; i++) coded[static_cast<size_t>(bI * kLdpcN + i)] = llr[static_cast<size_t>(bI * kLdpcN + i)] < 0 ? 1 : 0;
            }
        }
        std::vector<uint8_t> tx(coded.size());
        interleaveScramble(coded.data(), tx.data(), nBits);
        return mapBits(*mc, tx.data(), tx.size());
    };

    // 5. decision-directed passes
    const int L = std::max(1, o.ddWindow);
    for (int pass = 0; o.ddPass && pass < 2 && okCount < mc->blocks; pass++) {
        const auto dec = decisions();
        std::vector<cf> ref(kRegionSyms);
        for (int i = 0; i < kPilots; i++) ref[static_cast<size_t>(rm.pilot[static_cast<size_t>(i)])] = pilotSymbol(i);
        for (int i = 0; i < kDataSyms; i++) ref[static_cast<size_t>(rm.data[static_cast<size_t>(i)])] = dec[static_cast<size_t>(i)];
        // least-squares gain over +-L symbols (running sums)
        std::vector<cf> num(kRegionSyms + 1, cf(0, 0));
        std::vector<double> den(kRegionSyms + 1, 0.0);
        for (int r = 0; r < kRegionSyms; r++) {
            num[static_cast<size_t>(r + 1)] = num[static_cast<size_t>(r)] + yRaw[static_cast<size_t>(r)] * std::conj(ref[static_cast<size_t>(r)]);
            den[static_cast<size_t>(r + 1)] = den[static_cast<size_t>(r)] + std::norm(ref[static_cast<size_t>(r)]);
        }
        std::vector<cf> g2(kRegionSyms);
        double err = 0, pw = 0;
        for (int r = 0; r < kRegionSyms; r++) {
            const int lo = std::max(0, r - L), hi = std::min(kRegionSyms, r + L + 1);
            const double dd = den[static_cast<size_t>(hi)] - den[static_cast<size_t>(lo)];
            g2[static_cast<size_t>(r)] = dd > 1e-20 ? (num[static_cast<size_t>(hi)] - num[static_cast<size_t>(lo)]) / static_cast<float>(dd) : gain[static_cast<size_t>(r)];
            err += std::norm(yRaw[static_cast<size_t>(r)] - g2[static_cast<size_t>(r)] * ref[static_cast<size_t>(r)]);
            pw += std::norm(g2[static_cast<size_t>(r)]);
        }
        const double n0new = std::max(1e-5, err / std::max(1e-20, pw));
        const auto savedGain = gain;
        const double savedN0 = n0;
        gain = g2;
        n0 = n0new;
        equalise();
        const int nowOk = decodeBlocks();
        rf.ddPasses = pass + 1;
        if (nowOk <= okCount) {   // no gain: keep the earlier estimate for the display and the noise figure
            gain = savedGain;
            n0 = savedN0;
            equalise();
            break;
        }
        okCount = nowOk;
    }

    // modulation error ratio on the final decisions
    {
        const auto dec = decisions();
        double e = 0, s = 0;
        for (int i = 0; i < kDataSyms; i++) {
            e += std::norm(zs[static_cast<size_t>(i)] - dec[static_cast<size_t>(i)]);
            s += std::norm(dec[static_cast<size_t>(i)]);
        }
        rf.merDb = e > 1e-20 ? 10.0 * std::log10(s / e) : 99.0;
    }
    rf.allOk = okCount == mc->blocks;
    rf.cfoHz = (f0 + f) * o.symRate;
    rf.clockPpm = -b / kSps * 1e6;
    rf.dataSyms.assign(zs.begin(), zs.begin() + std::min<long>(2000, static_cast<long>(zs.size())));
    endT = T(kFrameSyms);
    return true;
}

std::vector<RxFrame> Receiver::Impl::push(const cf* iq, size_t n) {
    std::vector<RxFrame> out;
    const size_t old = buf.size();
    if (rs) {
        rsOut.clear();
        std::vector<cf> tmp(iq, iq + n);
        iqFix(tmp.data(), tmp.size());
        rs->process(tmp.data(), tmp.size(), rsOut);
        buf.insert(buf.end(), rsOut.begin(), rsOut.end());
    } else {
        buf.insert(buf.end(), iq, iq + n);
        iqFix(buf.data() + old, n);
    }
    computeMf();

    const long win = kSps * (kPreambleSyms - 1);
    const double ratio = (o.inRate > 0 ? o.inRate : kSps * o.symRate) / (kSps * o.symRate);
    for (;;) {
        if (scan + win + 16 >= static_cast<long>(q.size())) break;
        const double m = metric(scan);
        if (m < o.detect) {
            scan++;
            continue;
        }
        // peak within the next 12 samples
        if (scan + 13 + win >= static_cast<long>(q.size())) break;
        long best = scan;
        double bm = m;
        for (long k = scan + 1; k <= scan + 12; k++) {
            const double mk = metric(k);
            if (mk > bm) {
                bm = mk;
                best = k;
            }
        }
        // the whole frame must be in the buffer
        const long need = best + static_cast<long>(kSps * (kFrameSyms + kPreambleSyms) * 1.0005) + 160;   // the next frame's preamble too
        if (need >= mfDone) break;
        cf dpk;
        metric(best, &dpk);
        double frac = 0.0;
        if (best > 0) {
            const double m0 = metric(best - 1), m2 = metric(best + 1), den = m0 - 2.0 * bm + m2;
            if (std::fabs(den) > 1e-12) frac = std::clamp(0.5 * (m0 - m2) / den, -0.5, 0.5);
        }
        const double t0 = static_cast<double>(best) + frac;
        const double f0 = std::arg(dpk) / kTwoPi;
        RxFrame rf;
        double endT = 0;
        const bool okF = decodeFrame(t0, f0, rf, endT);
        if (okF) {
            rf.startSample = (static_cast<double>(base) + t0) * ratio;
            out.push_back(std::move(rf));
            scan = std::max(scan + 1, static_cast<long>(std::floor(endT)) - 3 * kSps);
        } else {
            rejects++;
            scan = best + kSps;
        }
    }
    // drop what is behind the scan position
    if (scan > (1L << 21)) {
        const long drop = scan - 4096;
        buf.erase(buf.begin(), buf.begin() + drop);
        mf.erase(mf.begin(), mf.begin() + drop);
        q.erase(q.begin(), q.begin() + std::min<long>(drop, static_cast<long>(q.size())));
        mfDone -= drop;
        scan -= drop;
        base += drop;
    }
    return out;
}

Receiver::Receiver() : Receiver(Options()) {}
Receiver::Receiver(const Options& o) : d_(std::make_unique<Impl>(o)) {}
Receiver::~Receiver() = default;
std::vector<RxFrame> Receiver::push(const cf* iq, size_t n) { return d_->push(iq, n); }
void Receiver::reset() { d_->reset(); }
int Receiver::headerRejects() const { return d_->rejects; }
cf Receiver::iqWeight() const { return d_->w; }

}  // namespace kavl
