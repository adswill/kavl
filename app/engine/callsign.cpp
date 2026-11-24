// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app callsign on the picture (callsign.h).
#include "callsign.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"   // stb_truetype as bundled with Dear ImGui (public domain / MIT)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

namespace {

struct Font {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ok = false;
};

const Font* loadFont(const std::string& path) {
    static std::map<std::string, std::unique_ptr<Font>> cache;
    auto& f = cache[path];
    if (!f) {
        f = std::make_unique<Font>();
        std::ifstream in(path, std::ios::binary);
        f->data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        const int off = f->data.empty() ? -1 : stbtt_GetFontOffsetForIndex(f->data.data(), 0);
        f->ok = off >= 0 && stbtt_InitFont(&f->info, f->data.data(), off);
    }
    return f->ok ? f.get() : nullptr;
}

// UTF-8 to code points (a callsign is ASCII, but a name or a locator may not be)
std::vector<int> codepoints(const std::string& s) {
    std::vector<int> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int cp = c, n = 1;
        if (c >= 0xF0) cp = c & 0x07, n = 4;
        else if (c >= 0xE0) cp = c & 0x0F, n = 3;
        else if (c >= 0xC0) cp = c & 0x1F, n = 2;
        for (int k = 1; k < n && i + static_cast<size_t>(k) < s.size(); k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3F);
        out.push_back(cp);
        i += static_cast<size_t>(n);
    }
    return out;
}

}  // namespace

std::string CallsignStyle::signature() const {
    char b[512];
    std::snprintf(b, sizeof(b), "%d|%s|%s|%.4f|%.4f|%.4f|%.3f,%.3f,%.3f,%.3f|%d", on ? 1 : 0, text.c_str(), font.c_str(), x, y, height, rgba[0], rgba[1], rgba[2],
                  rgba[3], outline ? 1 : 0);
    return b;
}

bool renderCallsign(const CallsignStyle& s, int w, int h, std::vector<uint8_t>& rgba, float box[4]) {
    rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    const Font* f = loadFont(s.font);
    const std::vector<int> cps = codepoints(s.text);
    if (!f || cps.empty() || w <= 0 || h <= 0) return false;
    const float px = std::max(4.f, s.height * h);
    const float scale = stbtt_ScaleForPixelHeight(&f->info, px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    // the width of the line, with kerning
    float tw = 0;
    for (size_t i = 0; i < cps.size(); i++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&f->info, cps[i], &adv, &lsb);
        tw += adv * scale;
        if (i + 1 < cps.size()) tw += stbtt_GetCodepointKernAdvance(&f->info, cps[i], cps[i + 1]) * scale;
    }
    const int r = s.outline ? std::max(1, static_cast<int>(std::lround(px / 14.f))) : 0;   // outline radius
    const int mw = static_cast<int>(std::ceil(tw)) + 2 * r + 2, mh = static_cast<int>(std::ceil((asc - desc) * scale)) + 2 * r + 2;
    // where it goes: inside the picture
    const int ox = std::clamp(static_cast<int>(std::lround(s.x * w)), 0, std::max(0, w - mw));
    const int oy = std::clamp(static_cast<int>(std::lround(s.y * h)), 0, std::max(0, h - mh));
    if (box) box[0] = static_cast<float>(ox) / w, box[1] = static_cast<float>(oy) / h, box[2] = static_cast<float>(mw) / w, box[3] = static_cast<float>(mh) / h;
    // the glyphs into a coverage mask
    std::vector<float> mask(static_cast<size_t>(mw) * mh, 0.f);
    const float base = r + 1 + asc * scale;
    float pen = static_cast<float>(r + 1);
    for (size_t i = 0; i < cps.size(); i++) {
        int x0, y0, x1, y1;
        const float sx = pen - std::floor(pen);
        stbtt_GetCodepointBitmapBoxSubpixel(&f->info, cps[i], scale, scale, sx, 0, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            std::vector<unsigned char> g(static_cast<size_t>(gw) * gh);
            stbtt_MakeCodepointBitmapSubpixel(&f->info, g.data(), gw, gh, gw, scale, scale, sx, 0, cps[i]);
            const int gx = static_cast<int>(std::floor(pen)) + x0, gy = static_cast<int>(std::lround(base)) + y0;
            for (int yy = 0; yy < gh; yy++)
                for (int xx = 0; xx < gw; xx++) {
                    const int mx = gx + xx, my = gy + yy;
                    if (mx < 0 || my < 0 || mx >= mw || my >= mh) continue;
                    float& m = mask[static_cast<size_t>(my) * mw + mx];
                    m = std::min(1.f, m + g[static_cast<size_t>(yy) * gw + xx] / 255.f);
                }
        }
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&f->info, cps[i], &adv, &lsb);
        pen += adv * scale;
        if (i + 1 < cps.size()) pen += stbtt_GetCodepointKernAdvance(&f->info, cps[i], cps[i + 1]) * scale;
    }
    // the outline: the mask grown by r (a disc), dark and slightly see-through
    std::vector<float> edge(mask.size(), 0.f);
    if (r > 0) {
        for (int yy = 0; yy < mh; yy++)
            for (int xx = 0; xx < mw; xx++) {
                float m = 0;
                for (int dy = -r; dy <= r && m < 1.f; dy++)
                    for (int dx = -r; dx <= r; dx++) {
                        if (dx * dx + dy * dy > r * r) continue;
                        const int sx2 = xx + dx, sy2 = yy + dy;
                        if (sx2 < 0 || sy2 < 0 || sx2 >= mw || sy2 >= mh) continue;
                        m = std::max(m, mask[static_cast<size_t>(sy2) * mw + sx2]);
                    }
                edge[static_cast<size_t>(yy) * mw + xx] = m;
            }
    }
    // text over its outline, straight alpha
    for (int yy = 0; yy < mh; yy++)
        for (int xx = 0; xx < mw; xx++) {
            const int X = ox + xx, Y = oy + yy;
            if (X >= w || Y >= h) continue;
            const float at = mask[static_cast<size_t>(yy) * mw + xx] * s.rgba[3];
            const float ao = edge[static_cast<size_t>(yy) * mw + xx] * 0.75f * s.rgba[3];
            const float a = at + ao * (1.f - at);
            if (a <= 0.f) continue;
            uint8_t* p = &rgba[(static_cast<size_t>(Y) * w + X) * 4];
            for (int c = 0; c < 3; c++) p[c] = static_cast<uint8_t>(std::lround(std::clamp(s.rgba[c] * at / a, 0.f, 1.f) * 255.f));
            p[3] = static_cast<uint8_t>(std::lround(std::clamp(a, 0.f, 1.f) * 255.f));
        }
    return true;
}

std::string writeCallsignImage(const CallsignStyle& s, int w, int h) {
    std::vector<uint8_t> img;
    if (!renderCallsign(s, w, h, img)) return "";
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) / "kavl";
    std::filesystem::create_directories(dir, ec);
    const std::string path = (dir / ("callsign_" + std::to_string(w) + "x" + std::to_string(h) + ".rgba")).string();
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
    return out ? path : "";
}
