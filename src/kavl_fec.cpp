// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL erasure protection (kavl/include/kavl/kavl_fec.h). GF(256) with the polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11D); parity row j, data
// column i: C[j][i] = 1 / (x_j + y_i) with y_i = i and x_j = G + j (all distinct, so every square submatrix is invertible).
#include "kavl/kavl_fec.h"

#include <algorithm>
#include <array>

namespace kavl::fec {

namespace {

struct Gf {
    std::array<uint8_t, 512> exp{};
    std::array<int, 256> log{};
    Gf() {
        int x = 1;
        for (int i = 0; i < 255; i++) {
            exp[static_cast<size_t>(i)] = static_cast<uint8_t>(x);
            log[static_cast<size_t>(x)] = i;
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; i++) exp[static_cast<size_t>(i)] = exp[static_cast<size_t>(i - 255)];
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp[static_cast<size_t>(log[a] + log[b])] : 0; }
    uint8_t inv(uint8_t a) const { return exp[static_cast<size_t>(255 - log[a])]; }
};
const Gf& gf() {
    static const Gf g;
    return g;
}

uint8_t cauchy(const Profile& p, int j, int i) { return gf().inv(static_cast<uint8_t>((p.data + j) ^ i)); }

// y += c * x over a whole cell
void mulAdd(uint8_t* y, const uint8_t* x, uint8_t c, size_t n) {
    if (!c) return;
    const Gf& g = gf();
    const int lc = g.log[c];
    for (size_t k = 0; k < n; k++)
        if (x[k]) y[k] ^= g.exp[static_cast<size_t>(g.log[x[k]] + lc)];
}

}  // namespace

Profile profile(int idx, int blocks) {
    int n = 0, p = 0;
    switch (idx) {
    case 1: n = 16, p = 1; break;
    case 2: n = 32, p = 2; break;
    case 3: n = 8, p = 1; break;
    default: return {};
    }
    blocks = std::max(1, blocks);
    return {(n - p) * blocks, p * blocks, n, p};
}

const char* profileName(int idx) {
    switch (idx) {
    case 1: return "1 frame in 16 (6.25 %)";
    case 2: return "2 frames in 32 (6.25 %)";
    case 3: return "1 frame in 8 (12.5 %)";
    default: return "none";
    }
}

std::vector<std::vector<uint8_t>> encode(const Profile& p, const std::vector<std::vector<uint8_t>>& data) {
    const size_t sz = data.empty() ? 0 : data[0].size();
    std::vector<std::vector<uint8_t>> par(static_cast<size_t>(p.parity), std::vector<uint8_t>(sz, 0));
    for (int j = 0; j < p.parity; j++)
        for (int i = 0; i < p.data && i < static_cast<int>(data.size()); i++) mulAdd(par[static_cast<size_t>(j)].data(), data[static_cast<size_t>(i)].data(), cauchy(p, j, i), sz);
    return par;
}

bool recover(const Profile& p, std::vector<std::vector<uint8_t>>& cells, size_t sz) {
    std::vector<int> lost, parityHave;
    for (int i = 0; i < p.data; i++)
        if (cells[static_cast<size_t>(i)].size() != sz) lost.push_back(i);
    if (lost.empty()) return true;
    for (int j = 0; j < p.parity; j++)
        if (cells[static_cast<size_t>(p.data + j)].size() == sz) parityHave.push_back(j);
    const int e = static_cast<int>(lost.size());
    if (e > static_cast<int>(parityHave.size())) return false;
    parityHave.resize(static_cast<size_t>(e));
    const Gf& g = gf();
    // syndromes: s_r = parity_r - sum over the present data cells
    std::vector<std::vector<uint8_t>> s(static_cast<size_t>(e));
    for (int r = 0; r < e; r++) {
        const int j = parityHave[static_cast<size_t>(r)];
        s[static_cast<size_t>(r)] = cells[static_cast<size_t>(p.data + j)];
        for (int i = 0; i < p.data; i++)
            if (cells[static_cast<size_t>(i)].size() == sz) mulAdd(s[static_cast<size_t>(r)].data(), cells[static_cast<size_t>(i)].data(), cauchy(p, j, i), sz);
    }
    // invert the e x e Cauchy submatrix A[r][k] = C[parityHave[r]][lost[k]] (Gauss-Jordan)
    std::vector<std::vector<uint8_t>> a(static_cast<size_t>(e), std::vector<uint8_t>(static_cast<size_t>(2 * e), 0));
    for (int r = 0; r < e; r++) {
        for (int k = 0; k < e; k++) a[static_cast<size_t>(r)][static_cast<size_t>(k)] = cauchy(p, parityHave[static_cast<size_t>(r)], lost[static_cast<size_t>(k)]);
        a[static_cast<size_t>(r)][static_cast<size_t>(e + r)] = 1;
    }
    for (int c = 0; c < e; c++) {
        int piv = c;
        while (piv < e && !a[static_cast<size_t>(piv)][static_cast<size_t>(c)]) piv++;
        if (piv == e) return false;
        std::swap(a[static_cast<size_t>(piv)], a[static_cast<size_t>(c)]);
        const uint8_t iv = g.inv(a[static_cast<size_t>(c)][static_cast<size_t>(c)]);
        for (auto& v : a[static_cast<size_t>(c)]) v = g.mul(v, iv);
        for (int r = 0; r < e; r++) {
            if (r == c || !a[static_cast<size_t>(r)][static_cast<size_t>(c)]) continue;
            const uint8_t f = a[static_cast<size_t>(r)][static_cast<size_t>(c)];
            for (int k = 0; k < 2 * e; k++) a[static_cast<size_t>(r)][static_cast<size_t>(k)] ^= g.mul(f, a[static_cast<size_t>(c)][static_cast<size_t>(k)]);
        }
    }
    // lost data k = sum_r inv[k][r] * s_r
    for (int k = 0; k < e; k++) {
        std::vector<uint8_t> d(sz, 0);
        for (int r = 0; r < e; r++) mulAdd(d.data(), s[static_cast<size_t>(r)].data(), a[static_cast<size_t>(k)][static_cast<size_t>(e + r)], sz);
        cells[static_cast<size_t>(lost[static_cast<size_t>(k)])] = std::move(d);
    }
    return true;
}

}  // namespace kavl::fec
