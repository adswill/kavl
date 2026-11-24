// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL media files: AV1 video as IVF and Opus sound as Ogg (what FFmpeg writes with -f ivf / -f ogg), read into kavl_av packets with times in ms.
#pragma once
#include "kavl/kavl_av.h"
#include <string>
#include <vector>

namespace kavl::media {
// IVF: one packet per temporal unit; key = it carries a sequence header (the encoder repeats it in every key frame).
bool readIvf(const std::string& path, std::vector<av::Packet>& out);
// Ogg Opus: the OpusHead in head, the audio packets in out (the times count from the pre-skip).
bool readOggOpus(const std::string& path, std::vector<uint8_t>& head, std::vector<av::Packet>& out);
// Duration of an Opus packet in 48 kHz samples (RFC 6716 section 3.1).
int opusSamples(const std::vector<uint8_t>& pk);
}  // namespace kavl::media
