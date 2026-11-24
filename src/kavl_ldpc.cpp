// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL LDPC (kavl/include/kavl/kavl_ldpc.h). The encoder and the check graph follow A/322 section 6.1.3 for Type B codes, as in the author's OnAir
// decoder; the check-rule correction is the author's two-minimum box-plus approximation.
#include "kavl/kavl_ldpc.h"
#include "kavl_tables.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>

namespace kavl {

namespace {
// Table 6.7: Q of the Type B codes, Ninner = 64800
int qOf(int rate15) {
    switch (rate15) {
    case 8: return 84;
    case 9: return 72;
    case 10: return 60;
    case 11: return 48;
    case 12: return 36;
    case 13: return 24;
    default: return 0;
    }
}
// log(1 + e^-t), t >= 0, as the largest of four tangents (at most 0.02 below the curve)
inline float softplusNeg(float t) {
    const float a = std::max(0.6854f - 0.4378f * t, 0.5822f - 0.2689f * t), b = std::max(0.3653f - 0.1192f * t, 0.1323f - 0.0293f * t);
    return std::max(0.f, std::max(a, b));
}
inline float boxMin1(float min1, float min2) {
    const float c = min1 + softplusNeg(min1 + min2) - softplusNeg(min2 - min1);
    return c > 0.f ? c : 0.f;
}
}  // namespace

Ldpc::Ldpc(int rate15) {
    q_ = qOf(rate15);
    const int* lens = nullptr;
    int nrows = 0;
    const int* tab = tables::ldpcRows64800(rate15, &lens, &nrows);
    if (!tab || !q_) return;
    k_ = n_ / 15 * rate15;
    m_ = n_ - k_;
    int pos = 0;
    for (int r = 0; r < nrows; r++) {
        rows_.emplace_back(tab + pos, tab + pos + lens[r]);
        pos += lens[r];
    }
    std::vector<std::vector<int>> chk(static_cast<size_t>(m_));
    for (int b = 0; b < k_; b++) {
        const int mm = b % 360;
        for (int x : rows_[static_cast<size_t>(b / 360)]) chk[static_cast<size_t>((x + mm * q_) % m_)].push_back(b);
    }
    for (int c = 0; c < m_; c++) {
        chk[static_cast<size_t>(c)].push_back(k_ + c);
        if (c > 0) chk[static_cast<size_t>(c)].push_back(k_ + c - 1);
    }
    chkStart_.assign(static_cast<size_t>(m_ + 1), 0);
    for (int c = 0; c < m_; c++) chkStart_[static_cast<size_t>(c + 1)] = chkStart_[static_cast<size_t>(c)] + static_cast<int>(chk[static_cast<size_t>(c)].size());
    chkVar_.resize(static_cast<size_t>(chkStart_[static_cast<size_t>(m_)]));
    for (int c = 0; c < m_; c++) std::copy(chk[static_cast<size_t>(c)].begin(), chk[static_cast<size_t>(c)].end(), chkVar_.begin() + chkStart_[static_cast<size_t>(c)]);
}

void Ldpc::encode(std::vector<uint8_t>& bits) const {
    bits.resize(static_cast<size_t>(n_), 0);
    std::vector<uint8_t> p(static_cast<size_t>(m_), 0);
    for (int i = 0; i < k_; i++) {
        if (!bits[static_cast<size_t>(i)]) continue;
        const int mm = i % 360;
        for (int x : rows_[static_cast<size_t>(i / 360)]) p[static_cast<size_t>((x + mm * q_) % m_)] ^= 1;
    }
    for (int i = 1; i < m_; i++) p[static_cast<size_t>(i)] ^= p[static_cast<size_t>(i - 1)];
    for (int i = 0; i < m_; i++) bits[static_cast<size_t>(k_ + i)] = p[static_cast<size_t>(i)];
}

bool Ldpc::decode(const std::vector<float>& llr, int maxIter, std::vector<uint8_t>& hard, int* iters, float alpha) const {
    const size_t E = chkVar_.size();
    std::vector<float> msg(E, 0.f), total(llr.begin(), llr.end());
    total.resize(static_cast<size_t>(n_), 0.f);
    hard.assign(static_cast<size_t>(n_), 0);
    std::vector<float> v(256);
    for (int it = 1; it <= maxIter; it++) {
        for (int c = 0; c < m_; c++) {
            const int s = chkStart_[static_cast<size_t>(c)], e = chkStart_[static_cast<size_t>(c + 1)];
            float min1 = 1e30f, min2 = 1e30f;
            int minIdx = -1;
            unsigned neg = 0;
            for (int j = s; j < e; j++) {
                const float x = total[static_cast<size_t>(chkVar_[static_cast<size_t>(j)])] - msg[static_cast<size_t>(j)];
                v[static_cast<size_t>(j - s)] = x;
                const float a = std::fabs(x);
                neg ^= (x < 0) ? 1u : 0u;
                if (a < min1) {
                    min2 = min1;
                    min1 = a;
                    minIdx = j;
                } else if (a < min2) {
                    min2 = a;
                }
            }
            const float c1 = alpha * boxMin1(min1, min2), c2 = alpha * min2;
            for (int j = s; j < e; j++) {
                const float x = v[static_cast<size_t>(j - s)];
                const unsigned sg = neg ^ ((x < 0) ? 1u : 0u);
                const float mag = (j == minIdx) ? c2 : c1;
                const float nm = sg ? -mag : mag;
                total[static_cast<size_t>(chkVar_[static_cast<size_t>(j)])] = x + nm;
                msg[static_cast<size_t>(j)] = nm;
            }
        }
        for (int i = 0; i < n_; i++) hard[static_cast<size_t>(i)] = total[static_cast<size_t>(i)] < 0 ? 1 : 0;
        bool ok = true;
        for (int c = 0; c < m_ && ok; c++) {
            unsigned x = 0;
            for (int j = chkStart_[static_cast<size_t>(c)]; j < chkStart_[static_cast<size_t>(c + 1)]; j++) x ^= hard[static_cast<size_t>(chkVar_[static_cast<size_t>(j)])];
            if (x) ok = false;
        }
        if (ok) {
            if (iters) *iters = it;
            return true;
        }
    }
    if (iters) *iters = maxIter;
    return false;
}

const Ldpc& ldpcCode(int rate15) {
    static std::array<std::unique_ptr<Ldpc>, 16> cache;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    const int r = std::clamp(rate15, 0, 15);
    if (!cache[static_cast<size_t>(r)]) cache[static_cast<size_t>(r)] = std::make_unique<Ldpc>(r);
    return *cache[static_cast<size_t>(r)];
}

}  // namespace kavl
