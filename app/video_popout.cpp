// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app pop-out video window (video_popout.h).
#include "video_popout.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <OpenGL/gl3.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
const char* kVs = R"(#version 150
in vec2 pos;
out vec2 uv;
void main() { uv = vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5); gl_Position = vec4(pos, 0.0, 1.0); }   // texture row 0 = the top of the picture
)";
const char* kFs = R"(#version 150
in vec2 uv;
out vec4 col;
uniform sampler2D tex;
void main() { col = vec4(texture(tex, uv).rgb, 1.0); }
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

bool VideoPopout::open(GLFWwindow* share, bool visible) {
    if (win_) {
        glfwFocusWindow(win_);
        return true;
    }
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
    win_ = glfwCreateWindow(w_, h_, "KAVL video", nullptr, share);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    if (!win_) return false;
    glfwSetWindowPos(win_, x_, y_);
    glfwSetWindowUserPointer(win_, this);
    glfwSetKeyCallback(win_, [](GLFWwindow* w, int key, int, int action, int) {
        auto* self = static_cast<VideoPopout*>(glfwGetWindowUserPointer(w));
        if (action != GLFW_PRESS || !self) return;
        if (key == GLFW_KEY_F || key == GLFW_KEY_F11) self->wantToggle_ = true;
        if (key == GLFW_KEY_ESCAPE) self->wantEsc_ = true;
    });
    glfwSetMouseButtonCallback(win_, [](GLFWwindow* w, int button, int action, int) {
        auto* self = static_cast<VideoPopout*>(glfwGetWindowUserPointer(w));
        if (!self || button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS) return;
        const double t = glfwGetTime();
        if (self->lastClick_ >= 0 && t - self->lastClick_ < 0.35) {
            self->wantToggle_ = true;
            self->lastClick_ = -1;
        } else {
            self->lastClick_ = t;
        }
    });
    GLFWwindow* prev = glfwGetCurrentContext();
    glfwMakeContextCurrent(win_);
    glfwSwapInterval(0);   // the main window waits for the screen already
    const unsigned vs = compile(GL_VERTEX_SHADER, kVs), fs = compile(GL_FRAGMENT_SHADER, kFs);
    if (vs && fs) {
        prog_ = glCreateProgram();
        glAttachShader(prog_, vs);
        glAttachShader(prog_, fs);
        glBindAttribLocation(prog_, 0, "pos");
        glLinkProgram(prog_);
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenVertexArrays(1, &vao_);   // vertex arrays are not shared between contexts: this one's own
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
    glfwMakeContextCurrent(prev);
    return true;
}

void VideoPopout::close() {
    if (!win_) return;
    GLFWwindow* prev = glfwGetCurrentContext();
    glfwMakeContextCurrent(win_);
    if (prog_) glDeleteProgram(prog_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    prog_ = vao_ = vbo_ = 0;
    glfwMakeContextCurrent(prev == win_ ? nullptr : prev);
    if (!fullscreen_) {
        glfwGetWindowPos(win_, &x_, &y_);
        glfwGetWindowSize(win_, &w_, &h_);
    }
    glfwDestroyWindow(win_);
    win_ = nullptr;
    fullscreen_ = false;
    waiting_ = -1;
}

void VideoPopout::setFullscreen(bool on) {
    if (on == fullscreen_ || !win_) return;
    if (on) {
        glfwGetWindowPos(win_, &x_, &y_);
        glfwGetWindowSize(win_, &w_, &h_);
        // the screen under the window's middle
        int count = 0;
        GLFWmonitor** mons = glfwGetMonitors(&count);
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        const int cx = x_ + w_ / 2, cy = y_ + h_ / 2;
        for (int i = 0; i < count; i++) {
            int mx, my;
            glfwGetMonitorPos(mons[i], &mx, &my);
            const GLFWvidmode* m = glfwGetVideoMode(mons[i]);
            if (m && cx >= mx && cx < mx + m->width && cy >= my && cy < my + m->height) mon = mons[i];
        }
        const GLFWvidmode* m = glfwGetVideoMode(mon);
        glfwSetWindowMonitor(win_, mon, 0, 0, m->width, m->height, m->refreshRate);
    } else {
        glfwSetWindowMonitor(win_, nullptr, x_, y_, w_, h_, 0);
    }
    fullscreen_ = on;
}

void VideoPopout::draw(GLFWwindow* main, unsigned tex, int w, int h) {
    if (!win_) return;
    if (wantToggle_) setFullscreen(!fullscreen_);
    if (wantEsc_) {
        if (fullscreen_) setFullscreen(false);
        else glfwSetWindowShouldClose(win_, 1);
    }
    wantToggle_ = wantEsc_ = false;
    if (glfwWindowShouldClose(win_)) {
        close();
        return;
    }
    const int waiting = tex && w > 0 ? 0 : 1;
    if (waiting != waiting_) {
        glfwSetWindowTitle(win_, waiting ? "KAVL video \xE2\x80\x94 waiting for a picture" : "KAVL video");
        waiting_ = waiting;
    }
    glfwMakeContextCurrent(win_);
    int fw, fh;
    glfwGetFramebufferSize(win_, &fw, &fh);
    glViewport(0, 0, fw, fh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (tex && w > 0 && h > 0 && prog_ && fw > 0 && fh > 0) {   // fitted, black bars
        const double a = static_cast<double>(w) / h;
        int vw = fw, vh = static_cast<int>(fw / a);
        if (vh > fh) vh = fh, vw = static_cast<int>(fh * a);
        glViewport((fw - vw) / 2, (fh - vh) / 2, vw, vh);
        glUseProgram(prog_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(glGetUniformLocation(prog_, "tex"), 0);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glUseProgram(0);
    }
    if (!shot_.empty()) {
        std::vector<unsigned char> px(static_cast<size_t>(fw) * fh * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, fw, fh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
        const std::string ppm = shot_ + ".ppm";
        if (FILE* f = std::fopen(ppm.c_str(), "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", fw, fh);
            for (int y = fh - 1; y >= 0; y--) std::fwrite(px.data() + static_cast<size_t>(y) * fw * 3, 1, static_cast<size_t>(fw) * 3, f);
            std::fclose(f);
            std::system(("sips -s format png '" + ppm + "' --out '" + shot_ + "' >/dev/null 2>&1 && rm -f '" + ppm + "'").c_str());
        }
        shot_.clear();
    }
    glfwSwapBuffers(win_);
    glfwMakeContextCurrent(main);
}
