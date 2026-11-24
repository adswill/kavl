// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL erasure protection across code words: groups of whole frames, the last p frames of each group carrying parity code words (Reed-Solomon
// over GF(256) with a Cauchy matrix, byte by byte). Any G of the G + P code words of a group give back all the data. Errors come a frame at a
// time (all code words of a frame share its noise and its synchronisation), so the groups count frames: any p whole frames of a group can be lost.
// The header's fec field (3 bits) names the profile, so a receiver needs no setting. The frames per group is a power of two, so the groups stay
// aligned when the 16-bit frame number wraps (group = frame number / frames per group).
#pragma once
#include <cstdint>
#include <vector>

namespace kavl::fec {

struct Profile {
    int data = 0, parity = 0;     // code words
    int frames = 0, parityFrames = 0;
    int n() const { return data + parity; }
};
constexpr int kNumProfiles = 4;
// For `blocks` code words per frame. 0: none, 1: 16 frames with 1 of parity (6.25 %), 2: 32 frames with 2 of parity (6.25 %, longer bursts),
// 3: 8 frames with 1 of parity (12.5 %).
Profile profile(int idx, int blocks);
const char* profileName(int idx);

// parity code words (P) of G data code words, all of the same size
std::vector<std::vector<uint8_t>> encode(const Profile& p, const std::vector<std::vector<uint8_t>>& data);
// cells: N entries in group order (data then parity), an empty vector = lost. Fills the lost data cells when at most P are lost.
// Returns true when every data cell is present afterwards.
bool recover(const Profile& p, std::vector<std::vector<uint8_t>>& cells, size_t cellSize);

}  // namespace kavl::fec
