// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app GPU colour conversion (video_gl.h).
#include "video_gl.h"
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>

namespace {
const char* kVs = R"(#version 150
in vec2 pos;
out vec2 uv;
void main() { uv = vec2(pos.x * 0.5 + 0.5, pos.y * 0.5 + 0.5); gl_Position = vec4(pos, 0.0, 1.0); }   // texture row 0 = picture row 0 (ImGui draws row 0 at the top)
)";
// BT.709, limited range (16..235 / 16..240)
const char* kFs = R"(#version 150
in vec2 uv;
out vec4 col;
uniform sampler2D ty, tu, tv;
void main() {
    float y = (texture(ty, uv).r - 16.0 / 255.0) * (255.0 / 219.0);
    float u = (texture(tu, uv).r - 128.0 / 255.0) * (255.0 / 224.0);
    float v = (texture(tv, uv).r - 128.0 / 255.0) * (255.0 / 224.0);
    col = vec4(clamp(vec3(y + 1.5748 * v, y - 0.1873 * u - 0.4681 * v, y + 1.8556 * u), 0.0, 1.0), 1.0);
}
)";

unsigned compile(unsigned type, const char* src) {
    const unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(s);
        return 0;
    }
    return s;
}
}  // namespace

void VideoGl::release() {
    if (!ok_) return;
    if (prog_) glDeleteProgram(prog_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (rgb_) glDeleteTextures(1, &rgb_);
    glDeleteTextures(3, tex_);
    prog_ = vbo_ = vao_ = fbo_ = rgb_ = 0;
    ok_ = false;
}

bool VideoGl::setup() {
    tried_ = true;
    const unsigned vs = compile(GL_VERTEX_SHADER, kVs), fs = compile(GL_FRAGMENT_SHADER, kFs);
    if (!vs || !fs) return false;
    prog_ = glCreateProgram();
    glAttachShader(prog_, vs);
    glAttachShader(prog_, fs);
    glBindAttribLocation(prog_, 0, "pos");
    glLinkProgram(prog_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok) return false;
    const float quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
    glGenTextures(3, tex_);
    glGenTextures(1, &rgb_);
    glGenFramebuffers(1, &fbo_);
    ok_ = true;
    return true;
}

void VideoGl::resize(int w, int h) {
    w_ = w;
    h_ = h;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    const int sizes[3][2] = {{w, h}, {cw, ch}, {cw, ch}};
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, tex_[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, sizes[i][0], sizes[i][1], 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    }
    glBindTexture(GL_TEXTURE_2D, rgb_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rgb_, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool VideoGl::update(const uint8_t* y, const uint8_t* u, const uint8_t* v, int w, int h) {
    if (!tried_ && !setup()) return false;
    if (!ok_ || w <= 0 || h <= 0) return false;
    if (w != w_ || h != h_) resize(w, h);
    // keep the state ImGui's renderer relies on
    GLint prevFbo = 0, prevProg = 0, prevVao = 0, prevTex = 0, prevActive = 0, vp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glGetIntegerv(GL_VIEWPORT, vp);
    const GLboolean blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST);
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    const uint8_t* planes[3] = {y, u, v};
    const int sizes[3][2] = {{w, h}, {cw, ch}, {cw, ch}};
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    for (int i = 0; i < 3; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, i == 0 ? &prevTex : &prevTex);
        glBindTexture(GL_TEXTURE_2D, tex_[i]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sizes[i][0], sizes[i][1], GL_RED, GL_UNSIGNED_BYTE, planes[i]);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(prog_);
    glUniform1i(glGetUniformLocation(prog_, "ty"), 0);
    glUniform1i(glGetUniformLocation(prog_, "tu"), 1);
    glUniform1i(glGetUniformLocation(prog_, "tv"), 2);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // restore
    glBindVertexArray(static_cast<GLuint>(prevVao));
    glUseProgram(static_cast<GLuint>(prevProg));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (blend) glEnable(GL_BLEND);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    for (int i = 2; i >= 0; i--) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(static_cast<GLenum>(prevActive));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
    return true;
}
