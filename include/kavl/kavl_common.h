// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL shared helpers (no outside code): CRCs, Zadoff-Chu sequence, root-raised-cosine taps, the xorshift64 generator, the K = 7 rate 1/2
// convolutional code and its soft Viterbi decoder.
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <thread>
#include <vector>

namespace kavl {

using cf = std::complex<float>;

uint16_t crc16(const uint8_t* d, size_t n);   // CRC-16-CCITT, polynomial 0x1021, init 0xFFFF ("123456789" -> 0x29B1)
uint32_t crc32(const uint8_t* d, size_t n);   // zlib CRC-32 ("123456789" -> 0xCBF43926)

// x[n] = exp(-j pi root n (n + 1) / L) for odd L, exp(-j pi root n^2 / L) for even L.
std::vector<cf> zadoffChu(int root, int length);
// Root-raised-cosine pulse, spanSymbols * sps + 1 taps centred on the middle one, unit energy.
std::vector<float> rrcTaps(float beta, int sps, int spanSymbols);

struct XorShift64 {   // shifts 13, 7, 17
    uint64_t s;
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
};

// K = 7 rate 1/2 convolutional code (generators 0171, 0133), terminated with six zero tail bits: 2 (n + 6) coded bits.
std::vector<uint8_t> convEncode(const std::vector<uint8_t>& bits);
// Soft Viterbi of a terminated block: llr = 2 (n + 6) values (positive = bit 0) -> n bits. Empty when the block is shorter than the tail.
std::vector<uint8_t> viterbiDecode(const std::vector<float>& llr);

// f(lo, hi) over [0, n) split into `threads` contiguous parts (the calling thread takes the first). Small jobs run on the caller alone.
template <class F>
void parallelFor(long n, int threads, F f, long minPerThread = 2048) {
    threads = static_cast<int>(std::max(1L, std::min<long>(threads, n / std::max(1L, minPerThread))));
    if (threads <= 1) {
        f(0L, n);
        return;
    }
    std::vector<std::thread> th;
    const long step = (n + threads - 1) / threads;
    for (int t = 1; t < threads; t++) {
        const long lo = t * step, hi = std::min(n, lo + step);
        if (lo < hi) th.emplace_back([=] { f(lo, hi); });
    }
    f(0L, std::min(n, step));
    for (auto& x : th) x.join();
}
// Decoder worker threads: all cores but two (the rest of the machine stays usable), at least 2, at most 8.
inline int workThreads() {
    static const int n = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 2, 2, 8);
    return n;
}
#define kWorkThreads (::kavl::workThreads())

// In-place radix-2 complex FFT (n a power of two); inverse = true for the inverse transform without the 1/n scaling.
void fft(cf* x, int n, bool inverse);

// Linear-phase FIR filtering of a long complex block by overlap-save FFT convolution (real taps, odd length). out[i] = sum_k taps[k] in[i + k],
// i = 0 .. nOut - 1 (in holds nOut + taps.size() - 1 samples). Blocks run in parallel.
void firFft(const cf* in, long nOut, const std::vector<float>& taps, cf* out);

}  // namespace kavl
