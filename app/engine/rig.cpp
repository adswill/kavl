// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app: rig control through rigctld (rig.h).
#include "rig.h"
#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

std::string RigControl::findRigctld() {
    for (const char* p : {"/opt/homebrew/bin/rigctld", "/usr/local/bin/rigctld", "/usr/bin/rigctld"})
        if (access(p, X_OK) == 0) return p;
    return "";
}

void RigControl::start(const RigSettings& s) {
    stop();
    {
        std::lock_guard<std::mutex> lk(m_);
        st_ = Status();
        ptt_.clear();
        freqHz_ = 0;
        pttQueued_ = pttDone_ = 0;
    }
    if (s.mode == 0) return;
    quit_ = false;
    running_ = true;
    th_ = std::thread([this, s] { worker(s); });
}

void RigControl::stop() {
    if (th_.joinable()) {
        setPtt(false);   // never leave the rig keyed
        waitPtt();
        quit_ = true;
        cv_.notify_all();
        th_.join();
    }
    running_ = false;
}

void RigControl::setFrequency(double hz) {
    if (!running_) return;
    std::lock_guard<std::mutex> lk(m_);
    freqHz_ = hz;
    cv_.notify_all();
}

void RigControl::setPtt(bool on) {
    if (!running_) return;
    std::lock_guard<std::mutex> lk(m_);
    ptt_.push_back(on ? 1 : 0);
    pttQueued_++;
    cv_.notify_all();
}

bool RigControl::waitPtt() {
    std::unique_lock<std::mutex> lk(m_);
    const long want = pttQueued_;
    return doneCv_.wait_for(lk, std::chrono::milliseconds(1200), [&] { return pttDone_ >= want || !running_; }) && st_.connected;
}

RigControl::Status RigControl::status() {
    std::lock_guard<std::mutex> lk(m_);
    return st_;
}

void RigControl::setError(const std::string& e) {
    std::lock_guard<std::mutex> lk(m_);
    st_.error = e;
}

void RigControl::disconnect() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    std::lock_guard<std::mutex> lk(m_);
    st_.connected = false;
}

bool RigControl::connectTo(const std::string& host, int port) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return false;
    int fd = -1;
    for (addrinfo* a = res; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        timeval tv{1, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        if (connect(fd, a->ai_addr, a->ai_addrlen) != 0) close(fd), fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return false;
    fd_ = fd;
    return true;
}

bool RigControl::command(const std::string& line, std::string& reply) {
    reply.clear();
    if (fd_ < 0) return false;
    const std::string out = line + "\n";
    if (send(fd_, out.data(), out.size(), 0) != static_cast<ssize_t>(out.size())) return false;
    char c;
    for (;;) {
        const ssize_t n = recv(fd_, &c, 1, 0);
        if (n <= 0) return false;   // closed or 1 s without an answer
        if (c == '\n') return true;
        reply += c;
        if (reply.size() > 256) return false;
    }
}

void RigControl::worker(RigSettings s) {
    std::string host = s.host;
    int port = s.port;
    if (s.mode == 2) {   // start rigctld for this rig, on a port of our own, on this Mac only
        const std::string exe = findRigctld();
        if (exe.empty()) {
            setError("Hamlib is not installed: brew install hamlib");
            running_ = false;
            return;
        }
        host = "127.0.0.1";
        port = 4532 + 17;   // apart from a rigctld someone else runs on the usual 4532
        std::vector<std::string> args = {exe, "-m", std::to_string(s.model), "-T", host, "-t", std::to_string(port)};
        if (!s.device.empty()) args.insert(args.end(), {"-r", s.device});
        if (s.baud > 0) args.insert(args.end(), {"-s", std::to_string(s.baud)});
        if (s.pttType > 0 && !s.device.empty()) args.insert(args.end(), {"-p", s.device, "-P", s.pttType == 1 ? "RTS" : "DTR"});
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        const int rc = posix_spawn(&child_, exe.c_str(), &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        if (rc != 0) {
            child_ = -1;
            setError("Could not start rigctld");
            running_ = false;
            return;
        }
    }
    auto lastPoll = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto lastTry = lastPoll;
    while (!quit_) {
        if (fd_ < 0) {   // (re)connect, at most twice a second
            if (child_ > 0 && waitpid(child_, nullptr, WNOHANG) == child_) {
                child_ = -1;
                setError("rigctld stopped: check the rig model, the serial port and the speed");
                break;
            }
            if (std::chrono::steady_clock::now() - lastTry > std::chrono::milliseconds(500)) {
                lastTry = std::chrono::steady_clock::now();
                if (connectTo(host, port)) {
                    std::lock_guard<std::mutex> lk(m_);
                    st_.connected = true;
                    st_.error.clear();
                } else {
                    setError(s.mode == 2 ? "Starting rigctld..." : "No rigctld at " + host + ":" + std::to_string(port));
                }
            }
        }
        int ptt = -1;
        double hz = 0;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, std::chrono::milliseconds(fd_ < 0 ? 100 : 250), [&] { return quit_ || !ptt_.empty() || (freqHz_ > 0 && fd_ >= 0); });
            if (!ptt_.empty()) ptt = ptt_.front(), ptt_.pop_front();
            else if (freqHz_ > 0 && fd_ >= 0) hz = freqHz_, freqHz_ = 0;   // a frequency asked for before connecting waits for the connection
        }
        std::string r;
        if (ptt >= 0) {
            const bool ok = fd_ >= 0 && command(ptt ? "T 1" : "T 0", r);
            if (fd_ >= 0 && !ok) disconnect();
            std::lock_guard<std::mutex> lk(m_);
            if (ok && r.rfind("RPRT 0", 0) == 0) st_.ptt = ptt != 0;
            else if (ok) st_.error = "The rig refused PTT (" + r + ")";
            pttDone_++;
            doneCv_.notify_all();
            continue;
        }
        if (fd_ < 0) continue;
        if (hz > 0) {
            char line[64];
            std::snprintf(line, sizeof(line), "F %.0f", hz);
            if (!command(line, r)) disconnect();
            else if (r.rfind("RPRT 0", 0) != 0) setError("The rig refused " + std::string(line) + " (" + r + ")");
            lastPoll = std::chrono::steady_clock::now() - std::chrono::seconds(10);   // read it back at once
            continue;
        }
        if (std::chrono::steady_clock::now() - lastPoll > std::chrono::seconds(1)) {
            lastPoll = std::chrono::steady_clock::now();
            if (!command("f", r)) {
                disconnect();
                continue;
            }
            std::lock_guard<std::mutex> lk(m_);
            st_.rigHz = std::atof(r.c_str());
        }
    }
    // quitting: PTT was released by stop(); close, and stop the rigctld we started
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    if (child_ > 0) {
        kill(child_, SIGTERM);
        for (int i = 0; i < 20 && waitpid(child_, nullptr, WNOHANG) == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (waitpid(child_, nullptr, WNOHANG) == 0) {
            kill(child_, SIGKILL);
            waitpid(child_, nullptr, 0);
        }
        child_ = -1;
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        st_.connected = false;
        st_.ptt = false;
        pttDone_ = pttQueued_;
        doneCv_.notify_all();
    }
    running_ = false;
}
