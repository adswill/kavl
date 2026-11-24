// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app the callsign editor (app.h).
#include "app.h"
#include <OpenGL/gl3.h>

#include "kavl/kavl_fec.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

// The callsign on the picture: the text, then a preview of the profile's picture (the received one behind it when there is one) where the
// text is dragged into place and sized (drag the corner or use the wheel), then font, colour and outline. The preview is the same drawing
// that goes out (callsign.h).
void callsignEditor(App& a, bool locked) {
    heading(a, "Callsign");
    if (locked) ImGui::BeginDisabled();
    ImGui::Checkbox("On the picture", &a.cs.on);
    ImGui::SetItemTooltip("Your callsign burned into what you send: every receiver and every recording shows it (station identification)");
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s", a.cs.text.c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##cstext", "Your callsign", buf, sizeof(buf))) a.cs.text = buf;
    if (locked) ImGui::EndDisabled();
    if (!a.cs.on) return;
    const auto& vp = kavl::videoProfile(a.txProfile);
    const float W = ImGui::GetContentRegionAvail().x, H = W * static_cast<float>(vp.height) / static_cast<float>(vp.width);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    // behind it: the received picture if there is one, else a neutral grey
    if (a.havePic && a.rx.running()) dl->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(a.video.texture())), p0, ImVec2(p0.x + W, p0.y + H));
    else dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + H), ImGui::GetColorU32(ImVec4(0.30f, 0.31f, 0.32f, 1)));
    // the preview texture: drawn again only when something changed
    const float fb = ImGui::GetIO().DisplayFramebufferScale.x;
    const int tw = std::max(16, static_cast<int>(W * fb)), th = std::max(9, static_cast<int>(H * fb));
    const std::string key = a.cs.signature() + "|" + std::to_string(tw) + "x" + std::to_string(th);
    if (key != a.csTexKey) {
        std::vector<uint8_t> img;
        float box[4] = {0, 0, 0, 0};
        if (renderCallsign(a.cs, tw, th, img, box)) std::copy(box, box + 4, a.csBox);
        else std::fill(a.csBox, a.csBox + 4, 0.f);
        if (!a.csTex) {
            glGenTextures(1, &a.csTex);
            glBindTexture(GL_TEXTURE_2D, a.csTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        glBindTexture(GL_TEXTURE_2D, a.csTex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.data());
        a.csTexKey = key;
    }
    dl->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(a.csTex)), p0, ImVec2(p0.x + W, p0.y + H));
    dl->AddRect(p0, ImVec2(p0.x + W, p0.y + H), ImGui::GetColorU32(pal::plotBorder()));
    // the text's box and its corner handle
    const ImVec2 b0(p0.x + a.csBox[0] * W, p0.y + a.csBox[1] * H), b1(b0.x + a.csBox[2] * W, b0.y + a.csBox[3] * H);
    const float hs = px(9);
    const ImVec2 h0(b1.x - hs * 0.5f, b1.y - hs * 0.5f), h1(b1.x + hs * 0.5f, b1.y + hs * 0.5f);
    if (locked) ImGui::BeginDisabled();
    ImGui::InvisibleButton("##csplace", ImVec2(W, H));
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    auto inside = [&](ImVec2 q0, ImVec2 q1) { return m.x >= q0.x && m.x <= q1.x && m.y >= q0.y && m.y <= q1.y; };
    const bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemActivated()) {
        if (inside(h0, h1)) a.csDrag = 2;
        else if (inside(b0, b1)) a.csDrag = 1;
        else {   // a click elsewhere puts the text there
            a.cs.x = std::clamp((m.x - p0.x) / W, 0.f, 1.f);
            a.cs.y = std::clamp((m.y - p0.y) / H, 0.f, 1.f);
            a.csDrag = 1;
        }
    }
    if (ImGui::IsItemActive() && a.csDrag == 1) {
        a.cs.x = std::clamp(a.cs.x + io.MouseDelta.x / W, 0.f, 1.f - a.csBox[2]);
        a.cs.y = std::clamp(a.cs.y + io.MouseDelta.y / H, 0.f, 1.f - a.csBox[3]);
    }
    if (ImGui::IsItemActive() && a.csDrag == 2) a.cs.height = std::clamp(a.cs.height + io.MouseDelta.y / H * 0.75f, 0.02f, 0.3f);
    if (ImGui::IsItemDeactivated()) a.csDrag = 0;
    if (hov && io.MouseWheel != 0.f) a.cs.height = std::clamp(a.cs.height * std::pow(1.08f, io.MouseWheel), 0.02f, 0.3f);
    if (hov && inside(h0, h1)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    else if (hov && inside(b0, b1)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (!locked && (hov || a.csDrag) && a.csBox[2] > 0) {
        dl->AddRect(b0, b1, ImGui::GetColorU32(pal::accent(0.9f)), 0.f, 0, 1.f);
        dl->AddRectFilled(h0, h1, ImGui::GetColorU32(pal::accent()));
    }
    if (hov && !a.csDrag) ImGui::SetTooltip("Drag the callsign to place it; drag its corner (or use the wheel) to size it. This is what receivers see.");
    if (a.cs.text.empty()) {
        const char* t = "Type your callsign above";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(p0.x + (W - ts.x) * 0.5f, p0.y + (H - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), t);
    }
    // font, colour, outline, size
    static const char* names[] = {"Inter", "Inter SemiBold", "JetBrains Mono", "A font file..."};
    static const char* files[] = {"Inter-Regular.ttf", "Inter-SemiBold.ttf", "JetBrainsMono-Regular.ttf"};
    int cur = 3;
    for (int i = 0; i < 3; i++)
        if (a.cs.font == resourceDir() + "/" + files[i]) cur = i;
    std::string custom = cur == 3 ? std::filesystem::path(a.cs.font).filename().string() : std::string();
    ImGui::SetNextItemWidth(-px(112));   // room for the colour swatch and "Edge"
    if (ImGui::BeginCombo("##csfont", cur == 3 ? custom.c_str() : names[cur])) {
        for (int i = 0; i < 4; i++) {
            if (ImGui::Selectable(names[i], i == cur)) {
                if (i < 3) a.cs.font = resourceDir() + "/" + files[i];
                else a.chooser.open(4, "Choose a font (TTF or OTF)", false);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The typeface of the callsign; any TrueType or OpenType file works");
    ImGui::SameLine();
    ImGui::ColorEdit4("##cscolour", a.cs.rgba, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf);
    ImGui::SetItemTooltip("Colour and see-through");
    ImGui::SameLine();
    ImGui::Checkbox("Edge", &a.cs.outline);
    ImGui::SetItemTooltip("A dark edge round the letters, so they read on any picture");
    float pct = a.cs.height * 100.f;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##cssize", &pct, 2.f, 30.f, "Size %.1f %% of the picture height")) a.cs.height = pct / 100.f;
    if (locked) ImGui::EndDisabled();
    if (locked) ImGui::TextDisabled("Stop transmitting to change the callsign.");
}
