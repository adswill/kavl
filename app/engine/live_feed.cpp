// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app live programme (live_feed.h).
#include "live_feed.h"
#include "kavl/kavl_media.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <regex>
#include <signal.h>
#include <fcntl.h>
#include <sstream>
#include <cmath>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

std::string findTool(const char* name) {
    // the copy inside KAVL.app first (Contents/MacOS, next to the app), then Homebrew (an app started from the Finder has a short PATH)
    char buf[4096];
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) == 0) {
        std::error_code ec;
        const auto own = std::filesystem::weakly_canonical(buf, ec).parent_path() / name;
        if (!ec && std::filesystem::exists(own) && own.filename() != std::filesystem::path(buf).filename()) return own.string();
    }
    for (const char* dir : {"/opt/homebrew/bin/", "/usr/local/bin/", "/usr/bin/"}) {
        const std::string p = std::string(dir) + name;
        if (std::filesystem::exists(p)) return p;
    }
    return name;
}

namespace {

std::string ffmpegPath() {
    const std::string p = findTool("ffmpeg");
    return p.find('/') == std::string::npos ? std::string() : p;
}

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

bool hasSequenceHeader(const std::vector<uint8_t>& d) {   // a key frame: the encoder repeats the sequence header (OBU type 1) in it
    for (size_t i = 0; i < d.size();) {
        const uint8_t h = d[i];
        const int type = (h >> 3) & 15, ext = (h >> 2) & 1, hasSize = (h >> 1) & 1;
        i += 1 + static_cast<size_t>(ext);
        uint64_t len = d.size() - i;
        if (hasSize) {
            len = 0;
            for (int s = 0; i < d.size(); s += 7) {
                const uint8_t b = d[i++];
                len |= static_cast<uint64_t>(b & 0x7F) << s;
                if (!(b & 0x80)) break;
            }
        }
        if (type == 1) return true;
        i += static_cast<size_t>(len);
    }
    return false;
}

struct Mode {
    int w = 0, h = 0;
    double fps = 0;
};

// The capture modes a camera offers. AVFoundation refuses a frame rate the camera does not have; asking for an impossible one makes ffmpeg list them.
std::vector<Mode> cameraModes(const std::string& ff, int camera) {
    std::vector<Mode> modes;
    const std::string cmd = "'" + ff + "' -hide_banner -nostdin -f avfoundation -framerate 9999 -i '" + std::to_string(camera) + ":none' -t 0 -f null - 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return modes;
    char line[512];
    static const std::regex re("([0-9]+)x([0-9]+)@\\[([0-9. ]+)\\]fps");
    while (std::fgets(line, sizeof(line), p)) {
        std::string l(line);
        std::smatch m;
        if (!std::regex_search(l, m, re)) continue;
        std::istringstream rates(m[3].str());
        double r;
        while (rates >> r) modes.push_back({std::stoi(m[1].str()), std::stoi(m[2].str()), r});
    }
    pclose(p);
    return modes;
}

// the capture mode for a picture of w x h at fps: the highest frame rate up to fps, then the smallest size that covers the picture (else the largest)
bool pickMode(const std::vector<Mode>& modes, int w, int h, int fps, Mode& out) {
    if (modes.empty()) return false;
    double best = -1;
    for (const auto& m : modes)
        if (m.fps <= fps + 0.01 && m.fps > best) best = m.fps;
    if (best < 0) {   // every rate is above the wanted one: the lowest
        best = 1e9;
        for (const auto& m : modes) best = std::min(best, m.fps);
    }
    const Mode* cover = nullptr;
    const Mode* largest = nullptr;
    for (const auto& m : modes) {
        if (std::abs(m.fps - best) > 0.01 || m.w <= m.h) continue;   // landscape modes only (not square or portrait)
        const long a = static_cast<long>(m.w) * m.h;
        if (!largest || a > static_cast<long>(largest->w) * largest->h) largest = &m;
        if (m.w >= w && m.h >= h && (!cover || a < static_cast<long>(cover->w) * cover->h)) cover = &m;
    }
    const Mode* pick = cover ? cover : largest;
    if (!pick) return false;
    out = *pick;
    return true;
}

}  // namespace

void listCaptureDevices(std::vector<CaptureDevice>& cameras, std::vector<CaptureDevice>& microphones) {
    cameras.clear();
    microphones.clear();
    const std::string ff = ffmpegPath();
    if (ff.empty()) return;
    const std::string cmd = "'" + ff + "' -hide_banner -f avfoundation -list_devices true -i '' 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return;
    char line[512];
    int section = 0;
    static const std::regex re("\\[([0-9]+)\\] (.+)");
    while (std::fgets(line, sizeof(line), p)) {
        std::string l(line);
        if (l.find("video devices") != std::string::npos) section = 1;
        else if (l.find("audio devices") != std::string::npos) section = 2;
        std::smatch m;
        const size_t br = l.rfind("] [");
        const std::string tail = br != std::string::npos ? l.substr(br + 2) : l;
        if (section && std::regex_search(tail, m, re)) {
            CaptureDevice d;
            d.index = std::stoi(m[1].str());
            d.name = m[2].str();
            while (!d.name.empty() && (d.name.back() == '\n' || d.name.back() == '\r')) d.name.pop_back();
            if (d.name.find("Capture screen") != std::string::npos) continue;   // screens are not cameras
            (section == 1 ? cameras : microphones).push_back(d);
        }
    }
    pclose(p);
}

bool LiveFeed::start(int camera, int mic, int width, int height, int fps, int videoKbps, const std::string& overlay) {
    stop();
    {
        std::lock_guard<std::mutex> lk(m_);
        err_.clear();
        v_.clear();
        vWall_.clear();
        a_.clear();
        head_.clear();
        st_ = Status();
        win_.clear();
    }
    const std::string ff = ffmpegPath();
    if (ff.empty()) {
        std::lock_guard<std::mutex> lk(m_);
        err_ = "ffmpeg was not found (it comes inside KAVL.app; for a build from source run tools/build_ffmpeg.sh, or brew install ffmpeg)";
        return false;
    }
    int vp[2], ap[2], ep[2];
    if (pipe(vp) != 0 || pipe(ap) != 0 || pipe(ep) != 0) return false;
    // move every end above fd 10, so the child's dup2 onto 2, 3 and 4 cannot collide with an end it still has to close
    for (int* p : {&vp[0], &vp[1], &ap[0], &ap[1], &ep[0], &ep[1]}) {
        const int hi = fcntl(*p, F_DUPFD_CLOEXEC, 10);
        close(*p);
        *p = hi;
    }
    Mode cam;
    if (camera >= 0) {
        if (!pickMode(cameraModes(ff, camera), width, height, fps, cam)) {
            close(vp[0]), close(vp[1]), close(ap[0]), close(ap[1]), close(ep[0]), close(ep[1]);
            std::lock_guard<std::mutex> lk(m_);
            err_ = "The camera did not answer (is it in use, or has KAVL no camera access? System Settings, Privacy & Security, Camera)";
            return false;
        }
        fps = std::max(1, static_cast<int>(std::lround(cam.fps)));   // send what the camera gives: a 30 fps camera makes a 30 fps programme
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        st_.captureW = cam.w, st_.captureH = cam.h, st_.captureFps = cam.fps;
    }
    char vf[256];
    std::snprintf(vf, sizeof(vf), "scale=%d:%d:force_original_aspect_ratio=decrease:flags=bicubic,pad=%d:%d:(ow-iw)/2:(oh-ih)/2,fps=%d,format=yuv420p", width,
                  height, width, height, fps);
    std::vector<std::string> args = {ff, "-hide_banner", "-loglevel", "error", "-nostdin"};
    if (camera < 0) {   // test pattern and tone
        args.insert(args.end(), {"-re", "-f", "lavfi", "-i", "testsrc2=size=" + std::to_string(width) + "x" + std::to_string(height) + ":rate=" + std::to_string(fps),
                                 "-re", "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000"});
    } else {
        char rate[32];
        std::snprintf(rate, sizeof(rate), "%.6f", cam.fps);
        // no probing and no input buffering: the camera's own parameters are known, and every frame should go to the encoder at once
        args.insert(args.end(), {"-fflags", "nobuffer", "-flags", "low_delay", "-probesize", "32", "-analyzeduration", "0", "-f", "avfoundation", "-framerate",
                                 rate, "-video_size", std::to_string(cam.w) + "x" + std::to_string(cam.h), "-i",
                                 std::to_string(camera) + ":" + (mic >= 0 ? std::to_string(mic) : std::string("none"))});
    }
    // the callsign: one more input (a still RGBA image the size of the picture), laid over the scaled picture
    std::string vmap = "0:v";
    std::vector<std::string> vfilter = {"-vf", vf};
    if (!overlay.empty()) {
        const int idx = camera < 0 ? 2 : 1;   // after the test pattern and its tone, or after the camera
        args.insert(args.end(), {"-f", "rawvideo", "-pix_fmt", "rgba", "-video_size", std::to_string(width) + "x" + std::to_string(height), "-i", overlay});
        vfilter = {"-filter_complex", std::string("[0:v]") + vf + "[base];[base][" + std::to_string(idx) + ":v]overlay=0:0:format=auto,format=yuv420p[v]"};
        vmap = "[v]";
    }
    const std::string bv = std::to_string(videoKbps) + "k";
    // SVT-AV1 real-time mode (rtc=1, CBR): it holds the bit rate (the plain low-delay CBR overshot 2x at 1440p). A key frame every second, so a
    // receiver that tunes in shows a picture within about a second.
    const long px = static_cast<long>(width) * height * fps;
    // two steps faster than the sharpest real-time choice (the owner's call, 2026-11-24): 1440p30 measured 232% -> 93% of a core, VMAF about
    // 79 -> 77.7. SVT-AV1 maps anything above 11 to 11.
    const char* preset = px <= 1920L * 1080 * 60 ? "9" : px <= 2560L * 1440 * 60 ? "10" : "11";
    args.insert(args.end(), vfilter.begin(), vfilter.end());
    args.insert(args.end(), {"-map", vmap, "-c:v", "libsvtav1", "-preset", preset, "-b:v", bv, "-g", std::to_string(fps), "-svtav1-params",
                             "lp=6:rtc=1:rc=2", "-bufsize", std::to_string(std::max(1, videoKbps / 4)) + "k", "-flush_packets", "1", "-f", "ivf", "pipe:3"});
    const bool sound = camera < 0 || mic >= 0;
    if (sound) {
        args.insert(args.end(), {"-map", camera < 0 ? "1:a" : "0:a", "-c:a", "libopus", "-b:a", "112k", "-ac", "2", "-ar", "48000", "-application", "lowdelay",
                                 "-frame_duration", "20", "-page_duration", "20000", "-flush_packets", "1", "-f", "ogg", "pipe:4"});
    }
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, vp[1], 3);
    posix_spawn_file_actions_adddup2(&fa, ap[1], 4);
    posix_spawn_file_actions_adddup2(&fa, ep[1], 2);   // ffmpeg's errors, shown in the window
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, ff.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(vp[1]);
    close(ap[1]);
    close(ep[1]);
    if (rc != 0) {
        close(vp[0]);
        close(ap[0]);
        close(ep[0]);
        std::lock_guard<std::mutex> lk(m_);
        err_ = "ffmpeg did not start";
        return false;
    }
    pid_ = pid;
    running_ = true;
    vth_ = std::thread([this, fd = vp[0]] { readVideo(fd); });
    eth_ = std::thread([this, fd = ep[0]] {   // keep the last error line
        std::string acc;
        char b[512];
        ssize_t k;
        while ((k = read(fd, b, sizeof(b))) > 0) {
            acc.append(b, static_cast<size_t>(k));
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string l = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                if (l.empty() || l.rfind("Svt[", 0) == 0) continue;
                std::lock_guard<std::mutex> lk(m_);
                err_ = l;
            }
        }
        close(fd);
    });
    ath_ = std::thread([this, fd = ap[0], sound] {
        if (sound) readAudio(fd);
        close(fd);
    });
    return true;
}

void LiveFeed::stop() {
    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        for (int i = 0; i < 40 && waitpid(pid_, nullptr, WNOHANG) == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (waitpid(pid_, nullptr, WNOHANG) == 0) {
            kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
        }
        pid_ = 0;
    }
    if (vth_.joinable()) vth_.join();
    if (ath_.joinable()) ath_.join();
    if (eth_.joinable()) eth_.join();
    running_ = false;
}

std::string LiveFeed::error() {
    std::lock_guard<std::mutex> lk(m_);
    return err_;
}

LiveFeed::Status LiveFeed::status() {
    std::lock_guard<std::mutex> lk(m_);
    return st_;
}

void LiveFeed::take(std::vector<kavl::av::Packet>& video, std::vector<kavl::av::Packet>& audio, std::vector<uint8_t>& head, std::vector<int64_t>* videoWallMs) {
    std::lock_guard<std::mutex> lk(m_);
    while (!v_.empty()) {
        video.push_back(std::move(v_.front()));
        v_.pop_front();
        if (videoWallMs) videoWallMs->push_back(vWall_.front());
        vWall_.pop_front();
    }
    while (!a_.empty()) {
        audio.push_back(std::move(a_.front()));
        a_.pop_front();
    }
    if (head.empty()) head = head_;
}

// IVF as it arrives: a 32-byte header (time base at 16/20), then 12-byte frame headers and the frames
void LiveFeed::readVideo(int fd) {
    std::vector<uint8_t> buf;
    uint8_t tmp[65536];
    bool haveHdr = false;
    uint32_t rate = 1000, scale = 1;
    size_t pos = 0;
    const auto t0 = std::chrono::steady_clock::now();
    long fpsN = 0;
    auto fpsT = t0;
    for (;;) {
        const ssize_t r = read(fd, tmp, sizeof(tmp));
        if (r <= 0) break;
        buf.insert(buf.end(), tmp, tmp + r);
        if (!haveHdr && buf.size() >= 32) {
            if (std::memcmp(buf.data(), "DKIF", 4)) break;
            rate = le32(&buf[16]);
            scale = le32(&buf[20]);
            pos = buf[6] | (buf[7] << 8);
            haveHdr = true;
        }
        while (haveHdr && pos + 12 <= buf.size()) {
            const uint32_t sz = le32(&buf[pos]);
            const uint64_t ts = le32(&buf[pos + 4]) | (static_cast<uint64_t>(le32(&buf[pos + 8])) << 32);
            if (pos + 12 + sz > buf.size()) break;
            kavl::av::Packet p;
            p.type = kavl::av::kVideo;
            p.data.assign(buf.begin() + static_cast<long>(pos + 12), buf.begin() + static_cast<long>(pos + 12 + sz));
            p.ptsMs = static_cast<uint32_t>(rate ? ts * 1000ull * scale / rate : 0);
            p.key = hasSequenceHeader(p.data);
            pos += 12 + sz;
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lk(m_);
            const double t = std::chrono::duration<double>(now - t0).count();
            win_.emplace_back(t, p.data.size());
            while (!win_.empty() && win_.front().first < t - 3.0) win_.pop_front();
            size_t bytes = 0;
            for (const auto& w : win_) bytes += w.second;
            st_.kbps = win_.size() > 1 ? bytes * 8.0 / std::max(0.5, win_.back().first - win_.front().first) / 1000.0 : 0;
            st_.videoUnits++;
            fpsN++;
            const double dt = std::chrono::duration<double>(now - fpsT).count();
            if (dt >= 1.0) {
                st_.fps = fpsN / dt;
                fpsN = 0;
                fpsT = now;
            }
            v_.push_back(std::move(p));
            vWall_.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            if (v_.size() > 600) {
                v_.pop_front();
                vWall_.pop_front();
            }
        }
        if (pos > (1u << 20)) {
            buf.erase(buf.begin(), buf.begin() + static_cast<long>(pos));
            pos = 0;
        }
    }
    close(fd);
    if (running_) {
        std::lock_guard<std::mutex> lk(m_);
        if (err_.empty()) err_ = "The live encoder stopped (no camera access, or the camera is in use?)";
    }
    running_ = false;
}

// Ogg as it arrives: pages -> packets; OpusHead, OpusTags, then sound
void LiveFeed::readAudio(int fd) {
    std::vector<uint8_t> buf, pk;
    uint8_t tmp[16384];
    int n = 0, preSkip = 0;
    long long samples = 0;
    bool timed = false;   // the first sound page set the time line from its granule position (the microphone may start later than the camera)
    for (;;) {
        const ssize_t r = read(fd, tmp, sizeof(tmp));
        if (r <= 0) break;
        buf.insert(buf.end(), tmp, tmp + r);
        size_t pos = 0;
        while (pos + 27 <= buf.size()) {
            if (std::memcmp(&buf[pos], "OggS", 4)) {   // lost the page start: look for the next one
                pos++;
                continue;
            }
            const int nseg = buf[pos + 26];
            if (pos + 27 + static_cast<size_t>(nseg) > buf.size()) break;
            size_t data = pos + 27 + static_cast<size_t>(nseg), total = 0;
            for (int s = 0; s < nseg; s++) total += buf[pos + 27 + static_cast<size_t>(s)];
            if (data + total > buf.size()) break;
            uint64_t granule = 0;
            for (int b = 7; b >= 0; b--) granule = (granule << 8) | buf[pos + 6 + static_cast<size_t>(b)];
            if (!timed && n >= 2 && pk.empty() && granule != ~0ull) {   // the end of this page in samples, less the packets that end on it
                long long inPage = 0;
                size_t at = data, len = 0;
                for (int s = 0; s < nseg; s++) {
                    const int l = buf[pos + 27 + static_cast<size_t>(s)];
                    len += static_cast<size_t>(l);
                    if (l < 255) {
                        inPage += kavl::media::opusSamples(std::vector<uint8_t>(buf.begin() + static_cast<long>(at), buf.begin() + static_cast<long>(at + len)));
                        at += len;
                        len = 0;
                    }
                }
                samples = std::max(0LL, static_cast<long long>(granule) - inPage);
                timed = true;
            }
            for (int s = 0; s < nseg; s++) {
                const int l = buf[pos + 27 + static_cast<size_t>(s)];
                pk.insert(pk.end(), buf.begin() + static_cast<long>(data), buf.begin() + static_cast<long>(data) + l);
                data += static_cast<size_t>(l);
                if (l < 255) {
                    std::lock_guard<std::mutex> lk(m_);
                    if (n == 0 && pk.size() >= 19 && !std::memcmp(pk.data(), "OpusHead", 8)) {
                        head_ = pk;
                        preSkip = pk[10] | (pk[11] << 8);
                    } else if (n >= 2) {
                        kavl::av::Packet p;
                        p.type = kavl::av::kAudio;
                        p.ptsMs = static_cast<uint32_t>(std::max(0LL, samples - preSkip) / 48);
                        samples += kavl::media::opusSamples(pk);
                        p.data = pk;
                        a_.push_back(std::move(p));
                        st_.audioPackets++;
                        if (a_.size() > 600) a_.pop_front();
                    }
                    n++;
                    pk.clear();
                }
            }
            pos = data;
        }
        buf.erase(buf.begin(), buf.begin() + static_cast<long>(pos));
    }
}
