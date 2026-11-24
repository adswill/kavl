// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app: the operator's callsign on the transmitted picture (station identification). KAVL draws the text itself (stb_truetype, the copy
// inside Dear ImGui) into a transparent RGBA image the size of the picture; ffmpeg lays it over the camera, the test pattern or the video
// file (overlay filter), so every receiver and every recording shows it. The same drawing feeds the preview the operator places it on.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct CallsignStyle {
    bool on = false;
    std::string text;
    std::string font;                    // a TTF/OTF file
    float x = 0.70f, y = 0.86f;          // top-left of the text, as a fraction of the picture
    float height = 0.06f;                // text height as a fraction of the picture height
    float rgba[4] = {1.f, 1.f, 1.f, 1.f};
    bool outline = true;                 // a dark edge so it reads on any picture
    std::string signature() const;       // everything that changes the drawing (an encoded file must match it)
};

// Draws the callsign into a w x h RGBA image (straight alpha, transparent elsewhere). box (optional): where the text landed, as fractions
// {x, y, w, h} of the picture. False if the text is empty or the font cannot be read.
bool renderCallsign(const CallsignStyle& s, int w, int h, std::vector<uint8_t>& rgba, float box[4] = nullptr);
// The same, written as a raw RGBA file for ffmpeg; the path, or empty on failure.
std::string writeCallsignImage(const CallsignStyle& s, int w, int h);
