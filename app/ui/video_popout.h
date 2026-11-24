// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app: the received picture in a window of its own (to put on a second screen). A second GLFW window whose OpenGL context shares the main
// one's objects, so it can draw the picture's RGBA texture (video_gl.h) as it is: fitted to the window with black bars, plain OpenGL (ImGui
// stays in the main window). F or a double-click: full screen on the screen the window is on; Esc: out of full screen, then closed.
#pragma once

#include <string>

struct GLFWwindow;

class VideoPopout {
public:
    ~VideoPopout() { close(); }
    bool open(GLFWwindow* share, bool visible = true);   // false if the window could not be made (visible = false: for checks)
    void close();
    bool isOpen() const { return win_ != nullptr; }
    // Draws the texture (w x h picture; 0 = nothing yet) and swaps; the main window's context is current again afterwards. Closes the window when
    // its user closed it.
    void draw(GLFWwindow* main, unsigned tex, int w, int h);
    void shotNext(const std::string& path) { shot_ = path; }   // a picture of the next draw (a PNG, for checks)

private:
    void setFullscreen(bool on);
    GLFWwindow* win_ = nullptr;
    unsigned prog_ = 0, vao_ = 0, vbo_ = 0;
    bool fullscreen_ = false, wantToggle_ = false, wantEsc_ = false;
    int x_ = 120, y_ = 120, w_ = 960, h_ = 540;
    double lastClick_ = -1;
    std::string shot_;
    int waiting_ = -1;   // the title says so while there is no picture
};
