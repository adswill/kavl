// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app spectrum and waterfall (spectrum_view.h).
#include "spectrum_view.h"
#include "settings.h"
#include "theme.h"

#include <OpenGL/gl3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// the waterfall colours: night blue through cyan, green and yellow to red (KAVL's own stops)
uint32_t colour(float v) {
    struct Stop {
        float v, r, g, b;
    };
    static const Stop s[] = {{0.00f, 0.02f, 0.02f, 0.06f}, {0.15f, 0.09f, 0.09f, 0.42f}, {0.30f, 0.06f, 0.38f, 0.84f}, {0.45f, 0.05f, 0.74f, 0.80f},
                             {0.60f, 0.36f, 0.89f, 0.36f}, {0.75f, 0.95f, 0.85f, 0.16f}, {0.88f, 0.98f, 0.46f, 0.10f}, {1.00f, 0.86f, 0.10f, 0.10f}};
    v = std::clamp(v, 0.f, 1.f);
    int i = 0;
    while (i < 6 && v > s[i + 1].v) i++;
    const float t = (v - s[i].v) / (s[i + 1].v - s[i].v);
    auto mix = [&](float a, float b) { return static_cast<uint32_t>(std::lround((a + (b - a) * t) * 255.f)); };
    return 0xFF000000u | (mix(s[i].b, s[i + 1].b) << 16) | (mix(s[i].g, s[i + 1].g) << 8) | mix(s[i].r, s[i + 1].r);
}

// a tick step of 1, 2 or 5 x 10^k for about n ticks over span
double niceStep(double span, int n) {
    const double raw = span / n, p = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (raw <= m * p) return m * p;
    return 10 * p;
}

}  // namespace

SpectrumView::~SpectrumView() {
    if (tex_) glDeleteTextures(1, &tex_);
}

void SpectrumView::clear() {
    rangeSet_ = false;
    disp_.clear();
    peakDb_.clear();
    std::fill(rows_.begin(), rows_.end(), 0xFF000000u);
    if (tex_) {
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, rows_.data());
    }
}

void SpectrumView::load(const Settings& s) {
    waterfall = s.flag("view.waterfall", true);
    peak_ = s.flag("spec.peak", false);
    autoRange_ = s.flag("spec.autoRange", true);
    avg_ = std::clamp(static_cast<float>(s.num("spec.avg", 0.5)), 0.f, 0.95f);
    minDb_ = static_cast<float>(s.num("spec.minDb", -100));
    maxDb_ = static_cast<float>(s.num("spec.maxDb", -30));
    split_ = std::clamp(static_cast<float>(s.num("spec.split", 0.45)), 0.15f, 0.85f);
}

void SpectrumView::save(Settings& s) const {
    s.set("view.waterfall", waterfall);
    s.set("spec.peak", peak_);
    s.set("spec.autoRange", autoRange_);
    s.set("spec.avg", static_cast<double>(avg_));
    s.set("spec.minDb", static_cast<double>(minDb_));
    s.set("spec.maxDb", static_cast<double>(maxDb_));
    s.set("spec.split", static_cast<double>(split_));
}

void SpectrumView::options() {
    // the row wraps when the centre is narrow: the next item goes on this line only if it fits
    auto flow = [](float w) {
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w <= right) ImGui::SameLine();
    };
    auto boxW = [](const char* l) { return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(l).x; };
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("##avg", &avg_, 0.f, 0.95f, "Averaging %.2f");
    ImGui::SetItemTooltip("Smooths the spectrum line: 0 shows each measurement as it comes, 0.95 a long average");
    flow(boxW("Peak hold"));
    if (ImGui::Checkbox("Peak hold", &peak_)) peakDb_.clear();
    ImGui::SetItemTooltip("Keeps the highest level each frequency reached (a thin line); switch off and on to start again");
    flow(boxW("Auto range"));
    ImGui::Checkbox("Auto range", &autoRange_);
    ImGui::SetItemTooltip("The dB range follows the noise floor and the strongest signal. Off: set it with the two sliders");
    if (!autoRange_) {
        flow(110);
        ImGui::SetNextItemWidth(110);
        ImGui::SliderFloat("##min", &minDb_, -140.f, maxDb_ - 5.f, "Min %.0f dB");
        ImGui::SetItemTooltip("The level at the bottom of the spectrum and the darkest waterfall colour");
        flow(110);
        ImGui::SetNextItemWidth(110);
        ImGui::SliderFloat("##max", &maxDb_, minDb_ + 5.f, 20.f, "Max %.0f dB");
        ImGui::SetItemTooltip("The level at the top of the spectrum and the brightest waterfall colour");
    }
    flow(boxW("Waterfall"));
    ImGui::Checkbox("Waterfall", &waterfall);
    ImGui::SetItemTooltip("The history of the spectrum below it");
    if (lo_ > 0.f || hi_ < 1.f) {
        flow(ImGui::CalcTextSize("Whole span").x + 2 * ImGui::GetStyle().FramePadding.x);
        if (ImGui::SmallButton("Whole span")) lo_ = 0.f, hi_ = 1.f;
        ImGui::SetItemTooltip("Zoom out (or double-click the spectrum)");
    }
}

void SpectrumView::addRow(const std::vector<float>& db) {
    const float lo = autoRange_ ? autoLo_ : minDb_, hi = autoRange_ ? autoHi_ : maxDb_;
    std::memmove(rows_.data() + kW, rows_.data(), sizeof(uint32_t) * kW * (kH - 1));
    const size_t n = db.size();
    for (int x = 0; x < kW; x++) rows_[static_cast<size_t>(x)] = colour((db[static_cast<size_t>(x) * n / kW] - lo) / (hi - lo));
    if (!tex_) {
        glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, rows_.data());
}

bool SpectrumView::draw(const Input& in, ImVec2 size, ImFont* small, double& tuneHz) {
    const bool have = in.db && !in.db->empty() && in.fs > 0;
    if (have && in.fresh) {
        const auto& db = *in.db;
        if (disp_.size() != db.size()) disp_ = db;
        for (size_t i = 0; i < db.size(); i++) disp_[i] = disp_[i] * avg_ + db[i] * (1.f - avg_);
        if (std::abs(in.rxHz - lastRxHz_) > 1.0) peakDb_.clear();   // retuned: the old peaks are elsewhere
        lastRxHz_ = in.rxHz;
        if (peak_) {
            if (peakDb_.size() != disp_.size()) peakDb_ = disp_;
            for (size_t i = 0; i < disp_.size(); i++) peakDb_[i] = std::max(peakDb_[i], disp_[i]);
        }
        std::vector<float> sorted(disp_);
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<long>(sorted.size() / 5), sorted.end());
        const float floorDb = sorted[sorted.size() / 5], top = *std::max_element(disp_.begin(), disp_.end());
        if (!rangeSet_) {   // the first measurement sets the range: no bright rows while it would still be easing in
            autoLo_ = floorDb - 4.f;
            autoHi_ = std::max(top + 4.f, floorDb + 25.f);
            rangeSet_ = true;
        }
        autoLo_ += ((floorDb - 4.f) - autoLo_) * 0.05f;   // the noise in dark blue
        autoHi_ += (std::max(top + 4.f, floorDb + 25.f) - autoHi_) * 0.05f;
        addRow(db);
    }
    const float dbLo = autoRange_ ? autoLo_ : minDb_, dbHi = autoRange_ ? autoHi_ : maxDb_;
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float scaleW = 38.f, axisH = 18.f, splitH = 6.f;
    const float plotX0 = o.x + scaleW, plotW = std::max(10.f, size.x - scaleW);
    const float avail = size.y - axisH - (waterfall ? splitH : 0.f);
    const float specH = waterfall ? std::max(30.f, avail * split_) : avail;
    const float wfH = waterfall ? std::max(0.f, avail - specH) : 0.f;
    const ImVec2 s0(plotX0, o.y), s1(plotX0 + plotW, o.y + specH);   // the spectrum
    const float axisY = s1.y, wfY0 = axisY + axisH + (waterfall ? splitH : 0.f);
    const double span = hi_ - lo_;
    auto xOfU = [&](double u) { return static_cast<float>(plotX0 + (u - lo_) / span * plotW); };
    auto uOfX = [&](float x) { return lo_ + (x - plotX0) / plotW * span; };
    auto uOfHz = [&](double hz) { return 0.5 + (hz - in.radioHz) / in.fs; };
    auto hzOfU = [&](double u) { return in.radioHz + (u - 0.5) * in.fs; };
    auto yOfDb = [&](float db) { return s1.y - std::clamp((db - dbLo) / (dbHi - dbLo), 0.f, 1.f) * specH; };

    // background, dB grid and scale
    dl->AddRectFilled(s0, s1, ImGui::GetColorU32(pal::plotBg()));
    ImGui::PushFont(small);
    const ImU32 grid = ImGui::GetColorU32(pal::plotGrid()), label = ImGui::GetColorU32(pal::grey());
    for (float g = std::ceil(dbLo / 10.f) * 10.f; g <= dbHi; g += 10.f) {
        const float y = yOfDb(g);
        dl->AddLine(ImVec2(s0.x, y), ImVec2(s1.x, y), grid);
        char t[16];
        std::snprintf(t, sizeof(t), "%.0f", g);
        const ImVec2 ts = ImGui::CalcTextSize(t);
        if (y - ts.y * 0.5f > s0.y && y + ts.y * 0.5f < s1.y) dl->AddText(ImVec2(plotX0 - ts.x - 5, y - ts.y * 0.5f), label, t);
    }
    dl->AddText(ImVec2(o.x + 2, s0.y + 2), label, "dB");
    // frequency axis: ticks and MHz labels, and the grid through the spectrum
    dl->AddRectFilled(ImVec2(o.x, axisY), ImVec2(s1.x, axisY + axisH), ImGui::GetColorU32(pal::plotBg()));
    if (have) {
        const double f0 = hzOfU(lo_), f1 = hzOfU(hi_), step = niceStep(f1 - f0, std::max(2, static_cast<int>(plotW / 110)));
        const int dec = step >= 1e6 ? 1 : step >= 1e5 ? 1 : step >= 1e4 ? 2 : step >= 1e3 ? 3 : 4;
        for (double f = std::ceil(f0 / step) * step; f <= f1; f += step) {
            const float x = xOfU(uOfHz(f));
            dl->AddLine(ImVec2(x, s0.y), ImVec2(x, s1.y), grid);
            dl->AddLine(ImVec2(x, axisY), ImVec2(x, axisY + 4), label);
            char t[32];
            std::snprintf(t, sizeof(t), "%.*f", dec, f / 1e6);
            const ImVec2 ts = ImGui::CalcTextSize(t);
            if (x - ts.x * 0.5f > plotX0 && x + ts.x * 0.5f < s1.x) dl->AddText(ImVec2(x - ts.x * 0.5f, axisY + 3), label, t);
        }
        dl->AddText(ImVec2(o.x + 2, axisY + 3), label, "MHz");
    }
    ImGui::PopFont();
    dl->PushClipRect(s0, ImVec2(s1.x, wfY0 + wfH), true);
    // the profile's band (dragged: where it would go) and the centre line
    if (have && in.bandHalfHz > 0) {
        const double c = in.rxHz + (bandDrag_ ? bandShiftHz_ : 0.0);
        const float bx0 = xOfU(uOfHz(c - in.bandHalfHz)), bx1 = xOfU(uOfHz(c + in.bandHalfHz)), cx = xOfU(uOfHz(c));
        dl->AddRectFilled(ImVec2(bx0, s0.y), ImVec2(bx1, s1.y), ImGui::GetColorU32(pal::accent(bandDrag_ ? 0.22f : 0.12f)));
        dl->AddLine(ImVec2(cx, s0.y), ImVec2(cx, s1.y), ImGui::GetColorU32(pal::accent(0.7f)), 1.f);
    }
    if (have && in.txHz > 0) {   // this Mac's transmitter
        const float tx = xOfU(uOfHz(in.txHz));
        for (float y = s0.y; y < s1.y; y += 6.f) dl->AddLine(ImVec2(tx, y), ImVec2(tx, std::min(y + 3.f, s1.y)), ImGui::GetColorU32(pal::badRed()), 1.f);
    }
    // the spectrum line and the peaks
    if (have && !disp_.empty()) {
        const int n = static_cast<int>(disp_.size());
        const int b0 = std::max(0, static_cast<int>(std::floor(lo_ * n)) - 1), b1 = std::min(n - 1, static_cast<int>(std::ceil(hi_ * n)) + 1);
        std::vector<ImVec2> line, fill;
        for (int b = b0; b <= b1; b++) line.emplace_back(xOfU((b + 0.5) / n), yOfDb(disp_[static_cast<size_t>(b)]));
        for (size_t i = 1; i < line.size(); i++)   // a soft fill under the line
            dl->AddQuadFilled(ImVec2(line[i - 1].x, s1.y), line[i - 1], line[i], ImVec2(line[i].x, s1.y), ImGui::GetColorU32(pal::accent(0.10f)));
        dl->AddPolyline(line.data(), static_cast<int>(line.size()), ImGui::GetColorU32(pal::accent()), 0, 1.3f);
        if (peak_ && peakDb_.size() == disp_.size()) {
            std::vector<ImVec2> pk;
            for (int b = b0; b <= b1; b++) pk.emplace_back(xOfU((b + 0.5) / n), yOfDb(peakDb_[static_cast<size_t>(b)]));
            dl->AddPolyline(pk.data(), static_cast<int>(pk.size()), ImGui::GetColorU32(pal::warnAmber()), 0, 1.0f);
        }
    } else {
        const char* t = "No spectrum: start receiving";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(s0.x + (plotW - ts.x) * 0.5f, s0.y + (specH - ts.y) * 0.5f), label, t);
    }
    // the waterfall (the part of the texture on show)
    if (waterfall && wfH > 1) {
        if (tex_) dl->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(tex_)), ImVec2(plotX0, wfY0), ImVec2(s1.x, wfY0 + wfH), ImVec2(lo_, 0), ImVec2(hi_, 1));
        else dl->AddRectFilled(ImVec2(plotX0, wfY0), ImVec2(s1.x, wfY0 + wfH), ImGui::GetColorU32(pal::pictureBlack()));
        if (have && in.bandHalfHz > 0) {   // the band's edges on the waterfall too
            const double c = in.rxHz + (bandDrag_ ? bandShiftHz_ : 0.0);
            for (double e : {c - in.bandHalfHz, c + in.bandHalfHz}) {
                const float x = xOfU(uOfHz(e));
                dl->AddLine(ImVec2(x, wfY0), ImVec2(x, wfY0 + wfH), ImGui::GetColorU32(pal::accent(0.35f)), 1.f);
            }
        }
    }
    dl->PopClipRect();
    dl->AddRect(s0, s1, ImGui::GetColorU32(pal::plotBorder()));

    // ---- the mouse: one button over the spectrum and the axis, the splitter, one over the waterfall ----
    bool tuned = false;
    const ImGuiIO& io = ImGui::GetIO();
    auto plotMouse = [&](const char* id, ImVec2 p0, ImVec2 sz, bool spectrumPart) {
        ImGui::SetCursorScreenPos(p0);
        ImGui::SetNextItemAllowOverlap();   // a control drawn over the plot (the "Back to" chip) takes its own clicks
        ImGui::InvisibleButton(id, ImVec2(std::max(1.f, sz.x), std::max(1.f, sz.y)));
        if (!have) return;
        const float mx = io.MousePos.x;
        if (ImGui::IsItemHovered()) {
            if (io.MouseWheel != 0.f) {   // zoom about the mouse
                const double u = uOfX(mx), k = io.MouseWheel > 0 ? 0.8 : 1.25;
                double nlo = u - (u - lo_) * k, nhi = u + (hi_ - u) * k;
                if (nhi - nlo > 1.0) nlo = 0, nhi = 1;
                if (nhi - nlo < 0.01) {
                    const double c = (nlo + nhi) / 2;
                    nlo = c - 0.005, nhi = c + 0.005;
                }
                const double sh = nlo < 0 ? -nlo : nhi > 1 ? 1 - nhi : 0;
                lo_ = static_cast<float>(nlo + sh), hi_ = static_cast<float>(nhi + sh);
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                lo_ = 0.f, hi_ = 1.f;
                pendingAt_ = -1;
            }
            // the readout
            const double hz = hzOfU(uOfX(mx));
            const int n = static_cast<int>(disp_.size());
            const int b = std::clamp(static_cast<int>(uOfX(mx) * n), 0, std::max(0, n - 1));
            char t[64];
            if (n) std::snprintf(t, sizeof(t), "%.4f MHz  %.1f dB", hz / 1e6, disp_[static_cast<size_t>(b)]);
            else std::snprintf(t, sizeof(t), "%.4f MHz", hz / 1e6);
            if (!dragging_) ImGui::SetTooltip("%s\nClick: tune here. Drag: pan (or drag the shaded band to retune). Wheel: zoom. Double-click: whole span.", t);
            dl->AddLine(ImVec2(mx, s0.y), ImVec2(mx, s1.y), ImGui::GetColorU32(pal::grey()), 1.f);
        }
        if (ImGui::IsItemActivated()) {
            pressX_ = mx;
            dragging_ = false;
            const double c = in.rxHz;
            const float bx0 = xOfU(uOfHz(c - in.bandHalfHz)), bx1 = xOfU(uOfHz(c + in.bandHalfHz));
            bandDrag_ = spectrumPart && in.bandHalfHz > 0 && mx >= bx0 - 3 && mx <= bx1 + 3;
            bandShiftHz_ = 0;
        }
        if (ImGui::IsItemActive()) {
            if (std::abs(mx - pressX_) > 3.f) dragging_ = true;
            if (dragging_) {
                const double du = io.MouseDelta.x / plotW * span;
                if (bandDrag_) {
                    bandShiftHz_ = static_cast<float>((mx - pressX_) / plotW * span * in.fs);
                } else {   // pan
                    const double sh = std::clamp(-du, -static_cast<double>(lo_), 1.0 - hi_);
                    lo_ = static_cast<float>(lo_ + sh), hi_ = static_cast<float>(hi_ + sh);
                }
            }
        }
        if (ImGui::IsItemDeactivated()) {
            if (bandDrag_ && dragging_) {
                tuneHz = std::round((in.rxHz + bandShiftHz_) / 1000.0) * 1000.0;
                tuned = true;
            } else if (!dragging_) {   // a click: tune there, unless it becomes a double-click
                pendingTuneHz_ = std::round(hzOfU(uOfX(mx)) / 1000.0) * 1000.0;
                pendingAt_ = ImGui::GetTime();
            }
            dragging_ = bandDrag_ = false;
            bandShiftHz_ = 0;
        }
    };
    plotMouse("##specmouse", o, ImVec2(size.x, specH + axisH), true);
    if (waterfall) {
        ImGui::SetCursorScreenPos(ImVec2(o.x, axisY + axisH));
        ImGui::InvisibleButton("##split", ImVec2(size.x, splitH));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive() && avail > 0) split_ = std::clamp(split_ + io.MouseDelta.y / avail, 0.15f, 0.85f);
        dl->AddLine(ImVec2(plotX0 + plotW * 0.45f, axisY + axisH + splitH * 0.5f), ImVec2(plotX0 + plotW * 0.55f, axisY + axisH + splitH * 0.5f),
                    ImGui::GetColorU32(pal::grey()), 2.f);
        plotMouse("##wfmouse", ImVec2(o.x, wfY0), ImVec2(size.x, wfH), false);
    }
    if (pendingAt_ >= 0 && ImGui::GetTime() - pendingAt_ > io.MouseDoubleClickTime) {
        tuneHz = pendingTuneHz_;
        tuned = true;
        pendingAt_ = -1;
    }
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(size);
    return tuned;
}
