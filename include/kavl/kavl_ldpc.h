// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// The ATSC 3.0 A/322 LDPC codes KAVL uses: Ninner = 64800, rates 8/15 to 13/15 (Type B: an accumulator chain like DVB-S2). Encoder and a layered
// normalised min-sum decoder with the two-minimum (box-plus) correction. Standalone.
#pragma once
#include <cstdint>
#include <vector>

namespace kavl {

class Ldpc {
public:
    explicit Ldpc(int rate15);   // 8 <= rate15 <= 13
    bool ok() const { return k_ > 0; }
    int n() const { return n_; }
    int k() const { return k_; }
    // bits[0..k) in; bits is extended to n (systematic code word).
    void encode(std::vector<uint8_t>& bits) const;
    // llr: n values, positive = bit 0, in their true scale. hard: n decided bits. Returns true when every check is satisfied (stops then).
    bool decode(const std::vector<float>& llr, int maxIter, std::vector<uint8_t>& hard, int* iters = nullptr, float alpha = 0.82f) const;
private:
    int n_ = 64800, k_ = 0, m_ = 0, q_ = 0;
    std::vector<std::vector<int>> rows_;
    std::vector<int> chkStart_, chkVar_;
};

// Cached code of the rate.
const Ldpc& ldpcCode(int rate15);

}  // namespace kavl
