// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app: the picture on the GPU. The Y, U and V planes go up as three one-channel textures; a small shader turns them into RGB (BT.709,
// limited range, as the AV1 encoder writes) into an RGBA texture through a framebuffer, which ImGui then draws like any image.
#pragma once
#include <cstdint>

class VideoGl {
public:
    ~VideoGl() { release(); }
    void release();   // the GPU objects, while the OpenGL context still exists
    // Upload one picture and convert it (GUI thread, a current OpenGL 3.2 core context). False if the GPU objects could not be made.
    bool update(const uint8_t* y, const uint8_t* u, const uint8_t* v, int w, int h);
    unsigned texture() const { return rgb_; }   // 0 until the first picture
    int width() const { return w_; }
    int height() const { return h_; }
private:
    bool setup();
    void resize(int w, int h);
    unsigned prog_ = 0, vao_ = 0, vbo_ = 0, fbo_ = 0, rgb_ = 0, tex_[3] = {0, 0, 0};
    int w_ = 0, h_ = 0;
    bool ok_ = false, tried_ = false;
};
