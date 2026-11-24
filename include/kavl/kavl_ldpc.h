// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// The ATSC 3.0 A/322 LDPC codes KAVL uses: Ninner = 64800, rates 2/15 to 13/15 (Type B for 6/15 and 8/15 up: an accumulator chain like DVB-S2;
// Type A for 2/15-5/15 and 7/15: a dual-diagonal part and an identity part). Encoder and a layered
// normalised min-sum decoder with the two-minimum (box-plus) correction. Standalone.
#pragma once
#include <cstdint>
#include <vector>

namespace kavl {

class Ldpc {
public:
    explicit Ldpc(int rate15);   // rate15 2..13: Type A (2..5, 7) or Type B (6, 8..13) codes of A/322
    bool ok() const { return k_ > 0; }
    int n() const { return n_; }
    int k() const { return k_; }
    // bits[0..k) in; bits is extended to n (systematic code word).
    void encode(std::vector<uint8_t>& bits) const;
    // llr: n values, positive = bit 0, in their true scale. hard: n decided bits. Returns true when every check is satisfied (stops then).
    bool decode(const std::vector<float>& llr, int maxIter, std::vector<uint8_t>& hard, int* iters = nullptr, float alpha = 0.82f) const;
private:
    int n_ = 64800, k_ = 0, m_ = 0, q_ = 0;
    bool typeA_ = false;
    int m1_ = 0, m2_ = 0, q1_ = 0, q2_ = 0;   // Type A: the dual-diagonal part's M1 parity bits, the identity part's M2
    int addr(int x, int mm) const {             // parity address of table entry x for the mm-th bit of its group of 360
        return typeA_ ? (x < m1_ ? (x + mm * q1_) % m1_ : m1_ + (x - m1_ + mm * q2_) % m2_) : (x + mm * q_) % m_;
    }
    std::vector<std::vector<int>> rows_;
    std::vector<int> chkStart_, chkVar_;
};

// Cached code of the rate.
const Ldpc& ldpcCode(int rate15);

}  // namespace kavl
